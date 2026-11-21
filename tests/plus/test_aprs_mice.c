// SPDX-License-Identifier: GPL-3.0-or-later
// DDM32 Plus Mic-E position beacons (CONFIG_APRS_MICE, tools/plus_aprs.py, item 47): the upstream radio-core APRS suite
// (tests/host/aprsRadioTest.c: the engine, radio_sm.c's APRS block, a fake AFSK transmitter whose tones are decoded
// back to frames) run again with it built in (the standard format stays the default), then:
//   - the encoder against the engine's own parser for every message M0-M6 over a grid of positions (all four
//     quadrants, every longitude band of the Mic-E code, the poles and 180), speeds (0-799 kn, the shifted codes),
//     courses (1-360, unknown) and altitudes: the destination address and the information field decode back;
//   - the engine: with the Mic-E format the manual beacon ("send now"), the automatic one and the SmartBeaconing one
//     (moving, from host NMEA) go out as Mic-E frames, destination and information field decoding back to our
//     position, speed, course, the message chosen and the comment, on the configured path; the APRS-IS copy carries
//     the same destination; the standard format again: APZRDF and '!' as before;
//   - the setting: saved and read back (spare settings bits), the default message En route, nothing past M6 (never
//     the emergency).
// Built like aprsRadioTest (tools/test_aprsx.py) with -DCONFIG_APRS_MSG=1 -DCONFIG_APRS_MICE=1 and aprs_plus.c.
#define main upstream_aprs_radio_main
#include "aprsRadioTest.c"
#undef main
#include <stdlib.h>
#include "aprs/aprs_plus.h"
#include "aprs/aprs_fmt.h"

#define HALF_HMIN	((APRS_DEG / 6000 + 1) / 2 + 1)	// half of 0.01' in 1e-7 degree, and the rounding
#define MSGS		7u				// M0 Off duty .. M6 Priority

static int trips;

// Encode, then decode with the engine's parser through an AX.25 frame (the destination as an address)
static void trip(int32_t lat, int32_t lon, uint16_t kn, uint16_t course, int32_t alt, uint8_t msg, const char *comment)
{
	aprs_pos_t p;
	memset(&p, 0, sizeof(p));
	p.lat = lat;
	p.lon = lon;
	p.speed_kn = kn;
	p.course = course;
	p.alt_m = alt;
	p.sym_table = '/';
	p.sym_code = '>';
	char dst[APRS_MICE_DST_LEN + 1], info[80];
	size_t n = aprs_mice_build(&p, msg, comment, dst, info, sizeof(info));
	ax25_addr_t d;
	aprs_parsed_t q;
	bool ok = n != 0 && strlen(dst) == APRS_MICE_DST_LEN && ax25_addr_parse(dst, APRS_MICE_DST_LEN, &d) &&
		  aprs_parse(&d, (const uint8_t *)info, n, &q) && q.type == APRS_T_MICE && q.has_pos;
	CHECK(ok);
	trips++;
	if (!ok) {
		printf("# %d %d M%u: no Mic-E back (%s)\n", lat, lon, msg, dst);
		return;
	}
	bool good = llabs((long long)q.pos.lat - lat) <= HALF_HMIN && llabs((long long)q.pos.lon - lon) <= HALF_HMIN &&
		    q.pos.speed_kn == (kn > 799u ? 799u : kn) &&
		    q.pos.course == (course == 0 || course == APRS_COURSE_NONE ? APRS_COURSE_NONE : course) &&
		    q.mice_msg == msg && q.pos.sym_table == '/' && q.pos.sym_code == '>' &&
		    (alt == APRS_ALT_NONE ? q.pos.alt_m == APRS_ALT_NONE : q.pos.alt_m == alt) &&
		    q.text_len == strlen(comment) && memcmp(q.text, comment, q.text_len) == 0;
	CHECK(good);
	if (!good)
		printf("# %d %d %u kn %u deg %d m M%u -> %d %d %u kn %u deg %d m M%u (%s)\n", lat, lon, kn, course, alt, msg,
		       q.pos.lat, q.pos.lon, q.pos.speed_kn, q.pos.course, q.pos.alt_m, q.mice_msg, dst);
	// The message bits: M0-M6 have at least one of the three set (the emergency has none)
	CHECK(dst[0] >= 'P' || dst[1] >= 'P' || dst[2] >= 'P');
}

static void test_encode_grid(void)
{
	// Latitudes: both hemispheres, the equator, minutes and hundredths edges, the poles
	static const int32_t lats[] = { 0, 1, 166667, 99999990, 405000000, 410083333, 490583333, 659999000, 899999000,
					900000000 };
	// Longitudes: every band of the Mic-E degree code (0-9, 10-99, 100-109, 110-179), its edges, 180
	static const int32_t lons[] = { 0, 1, 50000000, 99999000, 100000000, 289833333, 999999000, 1000000000, 1099999000,
					1100000000, 1500000000, 1799999000 };
	static const uint16_t kn[] = { 0, 1, 9, 10, 39, 40, 99, 100, 255, 799, 1000 };
	static const uint16_t crs[] = { 1, 99, 100, 199, 251, 300, 359, 360, 0, APRS_COURSE_NONE };
	static const int32_t alts[] = { APRS_ALT_NONE, 0, 61, 938, -50 };
	static const char *const comments[] = { "", "RadioFoundry test", "73 de TA" };
	unsigned k = 0;
	for (uint8_t m = 0; m < MSGS; m++) {
		for (unsigned i = 0; i < sizeof(lats) / sizeof(lats[0]); i++) {
			for (unsigned j = 0; j < sizeof(lons) / sizeof(lons[0]); j++, k++) {
				int32_t la = (k & 1u) ? -lats[i] : lats[i], lo = (k & 2u) ? -lons[j] : lons[j];
				trip(la, lo, kn[k % (sizeof(kn) / sizeof(kn[0]))], crs[k % (sizeof(crs) / sizeof(crs[0]))],
				     alts[k % (sizeof(alts) / sizeof(alts[0]))], m, comments[k % 3u]);
			}
		}
		// every speed against every course once per message
		for (unsigned i = 0; i < sizeof(kn) / sizeof(kn[0]); i++)
			for (unsigned j = 0; j < sizeof(crs) / sizeof(crs[0]); j++)
				trip(410000000, 290000000, kn[i], crs[j], APRS_ALT_NONE, m, "");
	}
	// 180 E / W: 179 59.99 (no Mic-E code for 180)
	aprs_pos_t p;
	memset(&p, 0, sizeof(p));
	p.lon = 1800000000;
	p.alt_m = APRS_ALT_NONE;
	p.course = APRS_COURSE_NONE;
	p.sym_table = '/';
	p.sym_code = '>';
	char dst[8], info[32];
	size_t n = aprs_mice_build(&p, 1, NULL, dst, info, sizeof(info));
	ax25_addr_t d;
	aprs_parsed_t q;
	CHECK(n == 9 && ax25_addr_parse(dst, 6, &d) && aprs_parse(&d, (const uint8_t *)info, n, &q) &&
	      q.mice_msg == 1 && llabs((long long)q.pos.lon - 1800000000) <= 1700);
	printf("# Mic-E encoder: %d positions decoded back\n", trips);
}

// The frame that went out: decoded, its destination a Mic-E one, the message, our fixed position
static bool on_air_mice(const char *tnc2, uint8_t msg, aprs_parsed_t *q)
{
	ax25_frame_t f;
	if (!ax25_from_tnc2(tnc2, strlen(tnc2), &f) || !aprs_parse(&f.dst, f.info, f.info_len, q))
		return false;
	return q->type == APRS_T_MICE && q->mice_msg == msg && strcmp(f.src.call, "N0TST") == 0 && f.src.ssid == 7 &&
	       f.ndigi == 2 && strcmp(f.digi[0].call, "WIDE1") == 0 && strcmp(f.digi[1].call, "WIDE2") == 0 &&
	       llabs((long long)q->pos.lat - 490583333) <= HALF_HMIN && llabs((long long)q->pos.lon + 720291667) <= HALF_HMIN &&
	       q->text_len == strlen("RadioFoundry test") && memcmp(q->text, "RadioFoundry test", q->text_len) == 0;
}

static void set_mice(uint8_t code)
{
	aprs_cfg_t c;
	aprs_get_cfg(&c);
	c.mice = code;
	aprs_set_cfg(&c, now);
}

static void test_engine(void)
{
	aprs_parsed_t q;
	// The default: the standard format, as before
	reset();
	aprs_cfg_t c;
	aprs_get_cfg(&c);
	CHECK(c.mice == 0);
	aprs_set_test_call("N0TST");
	configure(0, false);
	CHECK(aprs_send_now());
	CHECK(wait_start(1, 6000));
	CHECK(nframes == 1 && strcmp(frames[0], "N0TST-7>APZRDF,WIDE1-1,WIDE2-1:!4903.50N/07201.75W[RadioFoundry test") == 0);
	wait_idle();
	// Each message by hand ("send now")
	for (uint8_t m = 0; m < MSGS; m++) {
		reset();
		aprs_set_test_call("N0TST");
		configure(0, false);
		set_mice((uint8_t)(m + 1u));
		CHECK(aprs_send_now());
		CHECK(wait_start(1, 6000));
		CHECK(nframes == 1 && on_air_mice(frames[0], m, &q));
		if (nframes > 0 && m == 1)
			printf("# Mic-E En route on air: %s\n", frames[0]);
		CHECK(nframes == 0 || strstr(frames[0], ">APZRDF") == NULL);
		wait_idle();
	}
	// Automatic: every 60 s, Mic-E In service
	reset();
	aprs_set_test_call("N0TST");
	configure(60, false);
	set_mice(3);
	CHECK(wait_start(1, 70000));
	CHECK(nframes >= 1 && on_air_mice(frames[0], 2, &q));
	wait_idle();
	CHECK(wait_start(2, 70000));
	CHECK(nframes >= 2 && on_air_mice(frames[1], 2, &q));
	wait_idle();
	// SmartBeaconing, moving (NMEA from the host: 48 07.038 N 011 31.000 E, 22.4 kn, 84.4 deg), En route, to APRS-IS
	// too
	reset();
	aprs_set_test_call("N0TST");
	aprs_get_cfg(&c);
	c.kiss = true;
	c.to_is = true;
	c.tx_on = true;
	c.interval_s = 600;
	c.smart = true;
	c.mice = 2;
	aprs_set_cfg(&c, now);
	host_out_has("");
	bool went = false;
	for (int i = 0; i < 80 && !went; i++) {	// 41 km/h: about 180 s x 90 / 41 after turning on
		// a new fix every 10 s, each with its own time
		char rmc[96];
		int t = 10 * i;
		snprintf(rmc, sizeof rmc, "$GPRMC,12%02d%02d,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*", t / 60, t % 60);
		uint8_t sum = 0;
		for (const char *k = rmc + 1; *k != '*'; k++)
			sum ^= (uint8_t)*k;
		snprintf(rmc + strlen(rmc), sizeof rmc - strlen(rmc), "%02X", sum);
		aprs_nmea(rmc, strlen(rmc), false, now);	// the host's
		went = wait_start(1, 10000);
	}
	CHECK(went);
	if (!went) {
		aprs_view_t v;
		aprs_get_view(&v);
		printf("# smart: pos %d src %u speed %u course %u next %u block %u sent %u starts %d\n", v.pos_valid, v.pos_src,
		       v.speed_kmh, v.course, v.next_s, v.block, (unsigned)v.rf_sent, hw.starts);
	}
	ax25_frame_t f;
	CHECK(nframes >= 1 && ax25_from_tnc2(frames[0], strlen(frames[0]), &f) &&
	      aprs_parse(&f.dst, f.info, f.info_len, &q) && q.type == APRS_T_MICE && q.mice_msg == 1 &&
	      llabs((long long)q.pos.lat - 481173000) <= HALF_HMIN && llabs((long long)q.pos.lon - 115166667) <= HALF_HMIN &&
	      q.pos.speed_kn == 22 && q.pos.course == 84);
	if (nframes > 0)
		printf("# Mic-E SmartBeaconing on air: %s\n", frames[0]);
	char is[40] = "GN0TST-7>";
	if (nframes > 0) {
		strncat(is, frames[0] + 8, 6);	// the destination on air
		strcat(is, ",TCPIP*:`");
	}
	CHECK(host_out_has(is));
	wait_idle();
	// The standard format again: APZRDF
	reset();
	aprs_set_test_call("N0TST");
	configure(0, false);
	set_mice(5);
	set_mice(0);
	CHECK(aprs_send_now());
	CHECK(wait_start(1, 6000));
	CHECK(nframes == 1 && strstr(frames[0], "N0TST-7>APZRDF,WIDE1-1,WIDE2-1:!") == frames[0]);
	wait_idle();
}

static void test_setting(void)
{
	reset();
	aprs_cfg_t c;
	settings_t s;
	// Saved in spare bits and read back; the other APRS bits as they were
	aprs_get_cfg(&c);
	c.igate = true;
	c.ambiguity = 3;
	c.digi = 2;
	c.mice = 7;		// M6 Priority
	aprs_set_cfg(&c, now);
	settings_read(&s);
	CHECK(settings_aprs_mice(&s) == 7);
	aprs_reload();
	run(200);
	aprs_get_cfg(&c);
	CHECK(c.mice == 7 && c.igate && c.ambiguity == 3 && c.digi == 2);
	for (uint8_t code = 0; code <= 7u; code++) {
		c.mice = code;
		aprs_set_cfg(&c, now);
		aprs_reload();
		run(200);
		aprs_cfg_t b;
		aprs_get_cfg(&b);
		CHECK(b.mice == code && b.igate && b.ambiguity == 3 && b.digi == 2);
	}
	// Settings > APRS's own calls (no copy of the configuration): used and saved at once
	aprs_set_mice(5, now);
	CHECK(aprs_mice() == 5);
	settings_read(&s);
	CHECK(settings_aprs_mice(&s) == 5);
	aprs_reload();
	run(200);
	aprs_get_cfg(&c);
	CHECK(c.mice == 5 && c.igate && c.ambiguity == 3 && c.digi == 2);
	aprs_set_mice(APRS_MICE_EMERGENCY + 1u, now);
	CHECK(aprs_mice() == 0);
	// Past M6: the standard format (never the emergency)
	c.mice = 8;
	aprs_set_cfg(&c, now);
	aprs_get_cfg(&c);
	CHECK(c.mice == 0);
	c.mice = 255;
	aprs_set_cfg(&c, now);
	aprs_get_cfg(&c);
	CHECK(c.mice == 0);
	// A record without the bits (an older one): the standard format; Mic-E turned on starts at En route
	CHECK(SETTINGS_APRS_MICE_DEFAULT == 2u);
	settings_t *w = settings_begin();
	w->aprs_flags2 &= (uint8_t)~0xC0u;
	w->aprs_misc &= 0x7Fu;
	settings_end(now);
	aprs_reload();
	run(200);
	aprs_get_cfg(&c);
	CHECK(c.mice == 0);
}

int main(void)
{
	int up = upstream_aprs_radio_main();
	CHECK(up == 0);
	test_encode_grid();
	test_engine();
	test_setting();
	printf("# test_aprs_mice: %d checks, %s\n", checks, failures ? "FAILED" : "PASSED");
	return failures ? 1 : 0;
}
