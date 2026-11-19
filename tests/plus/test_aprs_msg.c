// SPDX-License-Identifier: GPL-3.0-or-later
// DM32 Plus APRS messages and SOS (CONFIG_APRS_MSG, integration/radiofoundry/aprsmsg): the upstream radio-core APRS
// suite (tests/host/aprsRadioTest.c: the engine, radio_sm.c's APRS block, a fake AFSK transmitter whose tones are
// decoded back to frames) run again with the feature built in, then:
//   - the Mic-E encoder against the existing parser: APRS101's own example byte for byte, the emergency bits, all four
//     quadrants, every longitude band (offset or not), the equator / pole / 180 edges, speed and course, altitude;
//   - SOS through the engine: refused without a callsign, a position or an amateur frequency (nothing keyed); on, a
//     Mic-E "Emergency" frame at once with its destination and the configured path; the next one after 120 s; off
//     stops it; an emergency heard from another station is noted for the UI's toast;
//   - the composer: the addressee fixed (spaces, case), bad ones refused, the text cleaned, aprs_send_message given
//     the right addressee and text, one at a time; sending / delivered / not delivered (3 tries); the canned texts;
//   - the inbox: peeking leaves a message unread, opening it marks it read.
// Built like aprsRadioTest (tools/test_classic.py) with -DCONFIG_APRS_MSG=1 and core/aprs/aprs_plus.c.
#define main upstream_aprs_radio_main
#include "aprsRadioTest.c"
#undef main
#include <stdlib.h>
#include "aprs/aprs_plus.h"
#include "aprs/aprs_fmt.h"

#define HALF_HMIN	((APRS_DEG / 6000 + 1) / 2 + 1)	// half of 0.01' in 1e-7 degree, and the rounding

static aprs_pos_t pos_of(int32_t lat, int32_t lon, uint16_t kn, uint16_t course)
{
	aprs_pos_t p;
	memset(&p, 0, sizeof(p));
	p.lat = lat;
	p.lon = lon;
	p.speed_kn = kn;
	p.course = course;
	p.alt_m = APRS_ALT_NONE;
	p.sym_table = '/';
	p.sym_code = '[';
	return p;
}

// Encode, parse back with the engine's own parser (q->text points into info_back)
static char info_back[80];
static bool mice_back(const aprs_pos_t *p, uint8_t msg, const char *comment, aprs_parsed_t *q, char *dst)
{
	char *info = info_back;
	size_t n = aprs_mice_build(p, msg, comment, dst, info, sizeof(info_back));
	ax25_addr_t d;
	return n != 0 && strlen(dst) == 6 && ax25_addr_parse(dst, 6, &d) &&
	       aprs_parse(&d, (const uint8_t *)info, n, q) && q->type == APRS_T_MICE && q->has_pos;
}

static void round_trip(int32_t lat, int32_t lon, uint16_t kn, uint16_t course, uint8_t msg)
{
	aprs_pos_t p = pos_of(lat, lon, kn, course);
	aprs_parsed_t q;
	char dst[8];
	bool ok = mice_back(&p, msg, "SOS", &q, dst);
	CHECK(ok);
	if (!ok) {
		printf("# round trip %d %d failed (%s)\n", lat, lon, dst);
		return;
	}
	CHECK(llabs((long long)q.pos.lat - lat) <= HALF_HMIN && llabs((long long)q.pos.lon - lon) <= HALF_HMIN);
	if (llabs((long long)q.pos.lat - lat) > HALF_HMIN || llabs((long long)q.pos.lon - lon) > HALF_HMIN)
		printf("# %d %d -> %d %d (%s)\n", lat, lon, q.pos.lat, q.pos.lon, dst);
	CHECK(q.pos.speed_kn == kn);
	CHECK(q.pos.course == (course == 0 || course == APRS_COURSE_NONE ? APRS_COURSE_NONE : course));
	CHECK(q.mice_msg == msg);
	CHECK(q.pos.sym_code == '[' && q.pos.sym_table == '/');
	CHECK(q.text_len == 3 && memcmp(q.text, "SOS", 3) == 0);
}

static void test_mice_encode(void)
{
	// APRS101 ch. 10's example (and aprsTest.c's test_mice): 33 25.64 N, 112 07.74 W, 20 kn, 251 deg, /j, M0
	int32_t lat = 330000000 + (int32_t)(2564LL * APRS_DEG / 6000), lon = -(1120000000 + (int32_t)(774LL * APRS_DEG / 6000));
	aprs_pos_t p = pos_of(lat, lon, 20, 251);
	p.sym_code = 'j';
	char dst[8], info[32];
	size_t n = aprs_mice_build(&p, 0, NULL, dst, info, sizeof(info));
	CHECK(n == 9 && strcmp(dst, "SSRUVT") == 0 && memcmp(info, "`(_fn\"Oj/", 9) == 0);
	// The same as an emergency: message bits 000, the first three digits plain
	n = aprs_mice_build(&p, APRS_MICE_EMERGENCY, NULL, dst, info, sizeof(info));
	CHECK(n == 9 && strcmp(dst, "332UVT") == 0 && memcmp(info, "`(_fn\"Oj/", 9) == 0);
	// M3 (Returning) = 100: only the first bit set
	n = aprs_mice_build(&p, 3, NULL, dst, info, sizeof(info));
	CHECK(n == 9 && strcmp(dst, "S32UVT") == 0);
	// Quadrants, every longitude band, emergency and standard messages
	static const int32_t lats[] = { 490583333, -337500000, 1, -899000000, 0, 900000000 };
	static const int32_t lons[] = { 1, 50000000, 99999000, 100000000, 109990000, 110000000, 179999000, 289000000 / 2 };
	for (unsigned i = 0; i < sizeof(lats) / sizeof(lats[0]); i++) {
		for (unsigned k = 0; k < sizeof(lons) / sizeof(lons[0]); k++) {
			round_trip(lats[i], lons[k], 0, APRS_COURSE_NONE, APRS_MICE_EMERGENCY);
			round_trip(lats[i], -lons[k], 12, 90, APRS_MICE_EMERGENCY);
			round_trip(-lats[i], lons[k], 345, 360, (uint8_t)(k % 7u));
		}
	}
	// Speed and course: units, tens, the 800 / 400 shifts, 1 deg, 360 (north), unknown
	static const uint16_t kn[] = { 0, 1, 3, 5, 9, 10, 39, 40, 41, 99, 100, 255, 799 };
	static const uint16_t crs[] = { 1, 5, 99, 100, 101, 199, 251, 299, 300, 359, 360, 0, APRS_COURSE_NONE };
	for (unsigned i = 0; i < sizeof(kn) / sizeof(kn[0]); i++) {
		for (unsigned k = 0; k < sizeof(crs) / sizeof(crs[0]); k++) {
			round_trip(410000000, 290000000, kn[i], crs[k], APRS_MICE_EMERGENCY);
		}
	}
	// Faster than Mic-E carries: 799
	aprs_parsed_t q;
	p = pos_of(410000000, 290000000, 1000, 90);
	CHECK(mice_back(&p, APRS_MICE_EMERGENCY, NULL, &q, dst) && q.pos.speed_kn == 799);
	// 180 E / W has no code: 179 59.99
	p = pos_of(10000000, 1800000000, 0, 0);
	CHECK(mice_back(&p, APRS_MICE_EMERGENCY, NULL, &q, dst) && llabs((long long)q.pos.lon - 1800000000) <= 1700);
	p = pos_of(10000000, -1800000000, 0, 0);
	CHECK(mice_back(&p, APRS_MICE_EMERGENCY, NULL, &q, dst) && llabs((long long)q.pos.lon + 1800000000) <= 1700);
	// Altitude 61 m: "\"4T}" before the comment (aprsTest.c's own vector)
	p = pos_of(lat, lon, 20, 251);
	p.alt_m = 61;
	n = aprs_mice_build(&p, APRS_MICE_EMERGENCY, "SOS", dst, info, sizeof(info));
	CHECK(n == 16 && memcmp(info + 9, "\"4T}SOS", 7) == 0);
	CHECK(mice_back(&p, APRS_MICE_EMERGENCY, "SOS", &q, dst) && q.pos.alt_m == 61 && q.text_len == 3);
	// Too small a buffer: nothing
	CHECK(aprs_mice_build(&p, APRS_MICE_EMERGENCY, "SOS", dst, info, 15) == 0);
	// Printable for any position on the grid (no control characters but SE 0-3 and the hundredths 0-3, as APRS101)
	p = pos_of(0, 0, 0, APRS_COURSE_NONE);
	n = aprs_mice_build(&p, APRS_MICE_EMERGENCY, NULL, dst, info, sizeof(info));
	CHECK(n == 9 && (uint8_t)info[4] >= 0x20 && (uint8_t)info[5] >= 0x20);
}

static void sos_state(aprs_plus_t *s)
{
	aprs_plus_state(s);
}

static void test_sos(void)
{
	aprs_plus_t s;
	// No callsign: refused, nothing on the air
	reset();
	configure(0, false);
	run(1000);
	CHECK(aprs_sos_set(true) == APRS_NO_CALL);
	run(5u * 60u * 1000u);
	sos_state(&s);
	CHECK(!s.sos && hw.starts == 0 && hw.tx_calls == 0);
	// A callsign without a position: refused
	reset();
	aprs_set_test_call("N0TST");
	run(1000);
	CHECK(aprs_sos_set(true) == APRS_NO_POSITION);
	run(5u * 60u * 1000u);
	CHECK(hw.starts == 0 && hw.tx_calls == 0);
	// Not an amateur frequency: refused
	reset();
	aprs_set_test_call("N0TST");
	configure(0, false);
	aprs_cfg_t c;
	aprs_get_cfg(&c);
	c.region = APRS_REGION_CUSTOM;
	c.freq_hz = 150000000u;
	aprs_set_cfg(&c, now);
	run(1000);
	CHECK(aprs_sos_set(true) == APRS_NOT_AMATEUR);
	run(5u * 60u * 1000u);
	CHECK(hw.starts == 0 && hw.tx_calls == 0);
	// Everything there: the emergency position at once, Mic-E destination, the configured path
	reset();
	aprs_set_test_call("N0TST");
	configure(0, false);
	run(1000);
	CHECK(aprs_sos_set(true) == APRS_OK);
	sos_state(&s);
	CHECK(s.sos && s.sos_sent == 0);
	CHECK(wait_start(1, 6000));
	CHECK(nframes == 1 && hw.tx_hz == APRS_HZ);
	// 49 03.50 N 072 01.75 W: digits 4 9 0 3 5 0, bits 000, north, no offset, west -> "490S5P";
	// info '`', 72 + 28 'd', 1 + 88 'Y', 75 + 28 'g', speed 0 (+80) 'l', ' ', course unknown 0x1c, symbol, "SOS"
	CHECK(strcmp(frames[0], "N0TST-7>490S5P,WIDE1-1,WIDE2-1:`dYgl \x1c[/SOS") == 0);
	if (nframes > 0)
		printf("# SOS on air: %s\n", frames[0]);
	ax25_frame_t f;
	aprs_parsed_t q;
	CHECK(nframes > 0 && ax25_from_tnc2(frames[0], strlen(frames[0]), &f) &&
	      aprs_parse(&f.dst, f.info, f.info_len, &q) && q.type == APRS_T_MICE && q.mice_msg == APRS_MICE_EMERGENCY &&
	      llabs((long long)q.pos.lat - 490583333) <= HALF_HMIN && llabs((long long)q.pos.lon + 720291667) <= HALF_HMIN);
	wait_idle();
	sos_state(&s);
	CHECK(s.sos && s.sos_sent == 1 && s.sos_next_s > 100 && s.sos_next_s <= 120);
	// Nothing more for two minutes, then the next one
	run(100u * 1000u);
	CHECK(hw.starts == 1);
	CHECK(wait_start(2, 30000));
	wait_idle();
	sos_state(&s);
	CHECK(s.sos_sent == 2);
	// Turning it on again changes nothing; off stops it
	CHECK(aprs_sos_set(true) == APRS_OK);
	run(1000);
	CHECK(hw.starts == 2);
	CHECK(aprs_sos_set(false) == APRS_OK);
	run(10u * 60u * 1000u);
	sos_state(&s);
	CHECK(!s.sos && hw.starts == 2);
	// The radio is reset (power-off): off
	CHECK(aprs_sos_set(true) == APRS_OK);
	reset();
	sos_state(&s);
	CHECK(!s.sos && s.sos_sent == 0);
	// The callsign goes while it is on: it waits, then goes again once it's back
	aprs_set_test_call("N0TST");
	configure(0, false);
	run(1000);
	CHECK(aprs_sos_set(true) == APRS_OK);
	aprs_set_test_call("");
	run(5u * 60u * 1000u);
	CHECK(hw.starts == 0);
	aprs_set_test_call("N0TST");
	CHECK(wait_start(1, 10000));
	wait_idle();
	aprs_sos_set(false);
#if CONFIG_APRS_RX
	// Heard: another station's emergency is noted for the toast; an ordinary Mic-E position isn't
	reset();
	aprs_set_test_call("N0TST");
	configure(0, false);
	run(1000);
	sos_state(&s);
	uint8_t e = s.emergencies;
	rx_line("K1ABC-9>SSRUVT,WIDE1-1:`(_fn\"Oj/");
	sos_state(&s);
	CHECK(s.emergencies == e);
	rx_line("K1ABC-9>332UVT,WIDE1-1:`(_fn\"Oj/SOS");
	sos_state(&s);
	char call[AX25_CALL_LEN + 4];
	aprs_sos_heard(call);
	CHECK(s.emergencies == (uint8_t)(e + 1u) && strcmp(call, "K1ABC-9") == 0);
#endif
}

static void test_compose(void)
{
	aprs_plus_t s;
	reset();
	aprs_set_test_call("N0TST");
	configure(0, false);
	run(1000);
	sos_state(&s);
	CHECK(s.out == APRS_OUT_NONE && s.tries == 0);
	// The canned texts: ASCII, short, nothing a message may not carry
	CHECK(strcmp(aprs_canned[0], "QSL") == 0 && strcmp(aprs_canned[1], "73") == 0);
	for (unsigned i = 0; i < APRS_CANNED_N; i++) {
		size_t l = strlen(aprs_canned[i]);
		CHECK(l > 0 && l <= APRS_MSG_TEXT_MAX && strpbrk(aprs_canned[i], "|~{") == NULL);
		for (size_t k = 0; k < l; k++)
			CHECK(aprs_canned[i][k] >= ' ' && aprs_canned[i][k] < 0x7f);
	}
	// Addressees
	char to[16], text[APRS_MSG_TEXT_MAX + 1];
	strcpy(to, " ta1abc-7 ");
	CHECK(aprs_addressee_fix(to) && strcmp(to, "TA1ABC-7") == 0);
	strcpy(to, "K1ABCDEF");
	CHECK(!aprs_addressee_fix(to));
	strcpy(to, "K1ABC-16");
	CHECK(!aprs_addressee_fix(to));
	strcpy(to, "");
	strcpy(text, "Hi");
	CHECK(aprs_compose_send(to, text) == APRS_SEND_BAD_TO);
	strcpy(to, "K1ABC-9");
	strcpy(text, "   ");
	CHECK(aprs_compose_send(to, text) == APRS_SEND_NO_TEXT);
	run(10u * 1000u);
	CHECK(hw.starts == 0);
	// A message, cleaned: to the addressee as typed in lower case, '|' replaced, trailing spaces gone
	strcpy(to, "k1abc-9");
	strcpy(text, "Hello|there  ");
	CHECK(aprs_compose_send(to, text) == APRS_SEND_OK);
	CHECK(strcmp(to, "K1ABC-9") == 0 && strcmp(text, "Hello-there") == 0);
	sos_state(&s);
	CHECK(s.out == APRS_OUT_SENDING);
	// One at a time
	strcpy(to, "K1ABC-9");
	strcpy(text, "Again");
	CHECK(aprs_compose_send(to, text) == APRS_SEND_BUSY);
	CHECK(wait_start(1, 6000));
	CHECK(nframes == 1 && strstr(frames[0], "N0TST-7>APZRDF,WIDE1-1,WIDE2-1::K1ABC-9  :Hello-there{") != NULL);
	wait_idle();
	sos_state(&s);
	CHECK(s.out == APRS_OUT_SENDING && s.tries == 1);
	// No ack: 30 s, then 60 s, then given up 120 s later: not delivered after three
	run(250u * 1000u);
	wait_idle();
	sos_state(&s);
	CHECK(hw.starts == 3 && s.out == APRS_OUT_FAILED && s.tries == 3);
	// A canned text, acked: delivered
	strcpy(to, "K1ABC-9");
	strcpy(text, aprs_canned[1]);
	CHECK(aprs_compose_send(to, text) == APRS_SEND_OK);
	nframes = 0;
	CHECK(wait_start(4, 6000));
	CHECK(nframes == 1 && strstr(frames[0], "::K1ABC-9  :73{") != NULL);
	wait_idle();
#if CONFIG_APRS_RX
	const char *brace = nframes > 0 ? strchr(frames[0], '{') : NULL;
	char ack[64];
	snprintf(ack, sizeof(ack), "K1ABC-9>APRS::N0TST-7  :ack%s", brace != NULL ? brace + 1 : "?");
	rx_line(ack);
	run(1000);
	sos_state(&s);
	CHECK(s.out == APRS_OUT_DELIVERED);
	run(200u * 1000u);
	sos_state(&s);
	CHECK(s.out == APRS_OUT_DELIVERED && hw.starts == 4);
#endif
}

#if CONFIG_APRS_RX
static void test_inbox(void)
{
	aprs_plus_t s;
	reset();
	aprs_set_test_call("N0TST");
	configure(0, false);
	run(1000);
	sos_state(&s);
	uint8_t r = s.received;
	rx_line("TA1ABC-7>APRS,WIDE1-1::N0TST-7  :Merhaba{7");
	rx_line("K1ABC-9>APRS,WIDE1-1::N0TST-7  :QSL?{8");
	sos_state(&s);
	CHECK(s.messages == 2 && s.unread == 2 && s.received == (uint8_t)(r + 2u));
	aprs_msg_t m;
	CHECK(aprs_msg_peek(0, &m) && strcmp(m.from, "K1ABC-9") == 0 && strcmp(m.text, "QSL?") == 0 && !m.read);
	CHECK(aprs_msg_peek(1, &m) && strcmp(m.from, "TA1ABC-7") == 0 && !aprs_msg_peek(2, &m));
	sos_state(&s);
	CHECK(s.unread == 2);
	CHECK(aprs_msg(1, &m) && strcmp(m.text, "Merhaba") == 0);
	sos_state(&s);
	CHECK(s.unread == 1);
	// The reply goes to the sender (once the two acks have gone out)
	run(30u * 1000u);
	wait_idle();
	char to[16], text[APRS_MSG_TEXT_MAX + 1];
	strcpy(to, m.from);
	strcpy(text, "Selam");
	CHECK(aprs_compose_send(to, text) == APRS_SEND_OK);
	nframes = 0;
	int st = hw.starts;
	CHECK(wait_start(st + 1, 8000));
	bool found = false;
	for (int i = 0; i < nframes; i++)
		found = found || strstr(frames[i], "::TA1ABC-7 :Selam{") != NULL;
	CHECK(found);
}
#endif

int main(void)
{
	int up = upstream_aprs_radio_main();
	CHECK(up == 0);
	test_mice_encode();
	test_sos();
	test_compose();
#if CONFIG_APRS_RX
	test_inbox();
#endif
	printf("# test_aprs_msg: %d checks, %s\n", checks, failures ? "FAILED" : "PASSED");
	return failures ? 1 : 0;
}
