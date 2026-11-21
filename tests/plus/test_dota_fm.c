/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus DOTA Transfer over FM, end to end through audio (docs/dota.md 4, 16.1): two whole sessions (core/radio/
 * dota.c twice, a_* sends, b_* receives, as tests/plus/test_dota_session.c) where each side's tone buffers become
 * Bell 202 audio (core/aprs/afsk.c's modulator, 8 kHz), the air adds the transmitter's CTCSS tone and noise, and the
 * other side hears it through the APRS demodulator (afsk.c) and a CTCSS detector. Time runs in 25 ms ticks: the audio
 * of a transmission plays out over its real length, so a sender waits for its own audio to end and a receiver hears
 * the carrier (not clear) meanwhile.
 *
 * The detector stands in for the FD6818's tone detect (hardware: register 0x0C b10, docs/spec/hardware-dm32.md 8.4):
 * the power at the receiver's own tone over the last 200 ms (a Hann-windowed Goertzel) as a share of the audio's
 * power; open at 0.8 % (the right tone gives 1.6-1.9 %, a tone 11.5 Hz off under 0.45 %). The sender's CTCSS is
 * put on the air at -16 dB under the AFSK (about 10 % of its deviation). Noise rides on the carrier; without a
 * carrier the receiver's squelch is shut (silence).
 *
 * Each half of the tone buffer (hrc_afsk's tx_more) starts its modulator's bit clock anew, and one sample is dropped or
 * repeated at each half boundary (a slip of the ring play's sample clock, risk R6). Cases: a clean link; noise at
 * 20 and 12 dB SNR; the sender without a tone, and with the wrong tone (100.0 Hz against 88.5): the receiver's
 * demodulator decodes the frames, the session takes none, the sender gets "No answer" and nothing is written. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "codeplug/codeplug.h"
#include "codeplug/zone_edit.h"
#include "settings/settings.h"
#include "storage/storage.h"
#include "sim/sim_flash.h"
#include "aprs/afsk.h"
#include "aprs/hdlc.h"
#include "radio/dota.h"
#include "radio/beep.h"
#include "radio/radio.h"
#include "dmr/dmrDataService.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* The two copies of dota.c */
#define SIDE(p)                                                                                                       \
	bool p##dota_start(const dota_req_t *r);                                                                     \
	void p##dota_stop(void);                                                                                      \
	void p##dota_view(dota_view_t *v);                                                                            \
	bool p##dota_job_wanted(void);                                                                                \
	void p##dota_job(void *ctx);                                                                                  \
	bool p##dota_begin(dota_req_t *r, uint32_t now);                                                              \
	void p##dota_poll(const dota_io_t *io, uint32_t now);                                                         \
	bool p##dota_owns_radio(void);                                                                                \
	void p##dota_fm_frame(const uint8_t *frame, size_t n);                                                       \
	void p##dota_dmr_rx(uint32_t src, const uint8_t *payload, int length);
SIDE(a_)
SIDE(b_)

void storage_lock(void) {}
void storage_unlock(void) {}
void storage_quick_lock(void) {}
void storage_quick_unlock(void) {}
void radio_req_dota(bool on) { (void)on; }

/* DMR is not used here */
int dmrDataServiceSendIPChain(bool group, uint32_t dst, uint32_t srcIp, uint32_t dstIp, uint8_t ttl,
			      const dmrDataChainPacket_t *p, int count)
{
	(void)group, (void)dst, (void)srcIp, (void)dstIp, (void)ttl, (void)p, (void)count;
	CHECK(false);
	return 0;
}
bool dmrDataServiceIpBusy(void) { return false; }
uint32_t dmrDataServiceRxBlockCount(void) { return 0; }

/* --- The air: each side's transmission as audio --- */
#define FS		8000u
#define TICK_MS		25u
#define TICK_N		(FS * TICK_MS / 1000u)
#define AIR_MAX		(FS * 20u)		/* 20 s: longer than any transmission (10 s limit) */
#define AMP		12000
#define DET_N		1600u			/* the detector's window: 200 ms */

typedef struct {
	int16_t audio[AIR_MAX];
	uint32_t wr, rd;
	afsk_mod_t mod;
	uint32_t halves;
	double ctcss_ph;
} air_t;
static air_t air[2];
static int side;			/* the side whose transmitter is being fed now */
static bool keyed[2];

/* The case's air */
static double snr_db;			/* noise under the AFSK; < 0: none */
static uint16_t tx_tone[2];		/* each transmitter's CTCSS, Hz x 10 (0: none) */
static uint16_t rx_tone[2];		/* the tone each receiver's detector looks for */

static void modulate(const uint8_t *t, uint16_t n)
{
	air_t *a = &air[side];
	CHECK(keyed[side]);
	if (a->halves++ != 0 && a->wr > 0) {
		if (a->halves & 1u)
			a->wr--;			/* a sample dropped at the boundary */
		else if (a->wr < AIR_MAX)
			a->audio[a->wr] = a->audio[a->wr - 1], a->wr++;	/* one repeated */
	}
	a->mod.sample = 0;		/* each half's bit clock starts anew, the oscillator's phase goes on */
	a->mod.bit = 0;
	size_t got;
	while ((got = afsk_mod(&a->mod, t, n, AMP, a->audio + a->wr, AIR_MAX - a->wr)) != 0)
		a->wr += (uint32_t)got;
	CHECK(a->wr < AIR_MAX);
}
static bool f_start(const uint8_t *t, uint16_t n, uint16_t baud, uint16_t m, uint16_t s, uint8_t l)
{
	(void)baud, (void)m, (void)s, (void)l;
	air_t *a = &air[side];
	a->wr = a->rd = 0;
	a->halves = 0;
	afsk_mod_init(&a->mod, FS);
	modulate(t, n);
	return true;
}
static bool f_more(const uint8_t *t, uint16_t n) { modulate(t, n); return true; }
static bool f_busy(void) { return air[side].rd < air[side].wr; }
static bool f_no(void) { return false; }
static void f_stop(void) { air[side].rd = air[side].wr; }
static void f_monitor(bool on) { (void)on; }
static void f_click(void) {}
static const hal_afsk_t fake_afsk = { .tx_start = f_start, .tx_busy = f_busy, .tx_stop = f_stop, .tx_more = f_more,
				      .tx_queued = f_no, .monitor = f_monitor, .click = f_click };

/* --- Each receiver: the demodulator and the tone detector --- */
static afsk_demod_t demod[2];
static int16_t heard[2][DET_N];		/* the last 200 ms, a ring */
static uint32_t heard_pos[2];
static bool tone_open[2];
static uint32_t decoded[2], taken_by[2];
static uint32_t rng = 7;

static double noise(void)
{
	/* about Gaussian, unit power: the sum of 12 uniforms */
	double s = 0;
	for (int i = 0; i < 12; i++) {
		rng = rng * 1103515245u + 12345u;
		s += (double)((rng >> 8) & 0xFFFFu) / 65536.0;
	}
	return s - 6.0;
}

static void frame_in(const uint8_t *f, size_t n, void *ctx)
{
	int rx = (int)(intptr_t)ctx;
	decoded[rx]++;
	if (rx == 1)
		b_dota_fm_frame(f, n);
	else
		a_dota_fm_frame(f, n);
}

/* The share of the window's power at the receiver's tone */
static double tone_share(int rx)
{
	double f = rx_tone[rx] / 10.0, w = 2.0 * M_PI * f / FS, c = 2.0 * cos(w);
	double s1 = 0, s2 = 0, total = 0;
	for (uint32_t i = 0; i < DET_N; i++) {
		double x = heard[rx][(heard_pos[rx] + i) % DET_N];
		x *= 0.5 - 0.5 * cos(2.0 * M_PI * i / (DET_N - 1));	/* Hann */
		double s0 = x + c * s1 - s2;
		s2 = s1;
		s1 = s0;
		total += x * x;
	}
	double p = s1 * s1 + s2 * s2 - c * s1 * s2;	/* |X(f)|^2 */
	return total > 0 ? 2.0 * p / (DET_N * total) : 0.0;
}

/* One tick of air from transmitter tx to receiver rx */
static void hear(int tx, int rx)
{
	air_t *a = &air[tx];
	int16_t buf[TICK_N];
	double amp_noise = snr_db < 0 ? 0.0 : (AMP / sqrt(2.0)) / pow(10.0, snr_db / 20.0);
	double amp_tone = AMP * pow(10.0, -16.0 / 20.0);
	for (uint32_t i = 0; i < TICK_N; i++) {
		double x = 0;
		if (a->rd < a->wr) {
			x = a->audio[a->rd++];
			if (tx_tone[tx]) {
				x += amp_tone * sin(a->ctcss_ph);
				a->ctcss_ph += 2.0 * M_PI * (tx_tone[tx] / 10.0) / FS;
			}
			x += amp_noise * noise();	/* the noise riding on the carrier */
		}				/* no carrier: the receiver's squelch is shut, silence */
		if (x > 32767)
			x = 32767;
		if (x < -32768)
			x = -32768;
		buf[i] = (int16_t)x;
		heard[rx][heard_pos[rx]] = buf[i];
		heard_pos[rx] = (heard_pos[rx] + 1u) % DET_N;
	}
	double sh = tone_share(rx);
	if (getenv("DOTA_FM_DEBUG") && a->rd < a->wr)
		printf("share %d %.5f\n", rx, sh);
	tone_open[rx] = sh >= 0.008;
	afsk_demod(&demod[rx], buf, TICK_N);
}

/* --- The codeplug (as the session test's) --- */
#define FLASH_SIZE	0x200000u
static const hal_flash_map_t map = {
	.settings_addr = 0x11000u, .settings_size = 0x8000u, .journal_addr = 0x19000u,
	.openfw_eeprom = 0, .openfw_flash = 0x20000u, .openfw_end = FLASH_SIZE,
	.calibration_addr = 0x10000u, .calibration_size = 0x200u,
};
static band_t bands[] = { { 136000000, 174000000, true }, { 400000000, 480000000, true } };
const board_t board = { .name = "test", .bands = bands, .band_count = 2, .power_count = 3, .flash = &sim_flash,
			.afsk = &fake_afsk };

static bool key_a(bool on) { keyed[0] = on; return true; }
static bool key_b(bool on) { keyed[1] = on; return true; }
static void beep_none(uint8_t b) { (void)b; }

static void fresh(void)
{
	sim_flash_create(FLASH_SIZE, &map);
	CHECK(storage_init());
	settings_init();
	codeplug_init();
	for (int i = 0; i < 64; i++) {
		channel_t c;
		memset(&c, 0, sizeof c);
		snprintf(c.name, sizeof c.name, "Ch %02d", i);
		c.rx_hz = c.tx_hz = 145000000u + (uint32_t)i * 12500u;
		c.mode = RADIO_MODE_FM;
		c.bw = RADIO_BW_12K5;
		c.power = CODEPLUG_POWER_DEFAULT;
		c.color_code = c.timeslot = 1;
		if (i & 1) {
			c.rx_tone.type = c.tx_tone.type = TONE_CTCSS;
			c.rx_tone.value = c.tx_tone.value = 1000;
		}
		CHECK(codeplug_set_channel((uint16_t)(i + 1), &c));
	}
	zone_t z = { "Home", 16, { 0 } };
	for (int i = 0; i < 16; i++)
		z.channels[i] = (uint16_t)(i + 1);
	CHECK(zone_edit_create(&z, NULL) == ZONE_EDIT_OK);
	zone_t big = { "Big", 64, { 0 } };
	for (int i = 0; i < 64; i++)
		big.channels[i] = (uint16_t)(i + 1);
	CHECK(zone_edit_create(&big, NULL) == ZONE_EDIT_OK);
}

static int find_zone(const char *name, zone_t *out)
{
	for (uint16_t i = 0; i < codeplug_zone_count(); i++)
		if (codeplug_zone(i, out) && strcmp(out->name, name) == 0)
			return i;
	return -1;
}

static uint32_t now = 1000;
static dota_view_t va, vb;

/* A zone of 16 from a to b (one shared codeplug: the copy lands in free slots), over the case's air */
static const char *send_zone = "Home";

static void run(void)
{
	memset(keyed, 0, sizeof keyed);
	memset(air, 0, sizeof air);
	memset(heard, 0, sizeof heard);
	memset(tone_open, 0, sizeof tone_open);
	decoded[0] = decoded[1] = taken_by[0] = taken_by[1] = 0;
	for (int s = 0; s < 2; s++)
		CHECK(afsk_demod_init(&demod[s], FS, frame_in, (void *)(intptr_t)s));
	zone_t z;
	uint16_t home = (uint16_t)find_zone(send_zone, &z);
	dota_req_t ra = { .role = DOTA_SEND, .method = DOTA_FM, .hz = 433500000, .ctcss = 885, .power = 0, .code = 4271,
			  .call = "TA1AAA", .kind = DOTA_KIND_ZONE, .zone = home };
	dota_req_t rb = { .role = DOTA_RECEIVE, .method = DOTA_FM, .hz = 433500000, .ctcss = 885, .code = 4271,
			  .call = "TA1BBB" };
	rb.imp.mode = DOTA_IMP_NEW;
	strcpy(rb.imp.name, "Copy");
	dota_req_t got;
	CHECK(b_dota_start(&rb) && b_dota_begin(&got, now));
	CHECK(a_dota_start(&ra) && a_dota_begin(&got, now));
	bool a_on = true, b_on = true;
	for (uint32_t t = 0; t < 300000u && (a_on || b_on); t += TICK_MS, now += TICK_MS) {
		dota_io_t ia = { .key = key_a, .beep = beep_none, .clear = air[1].rd >= air[1].wr,
				 .tone_ok = tone_open[0] };
		dota_io_t ib = { .key = key_b, .beep = beep_none, .clear = air[0].rd >= air[0].wr,
				 .tone_ok = tone_open[1] };
		side = 0;
		if (a_on)
			a_dota_poll(&ia, now);
		side = 1;
		if (b_on)
			b_dota_poll(&ib, now);
		if (b_dota_job_wanted())
			b_dota_job(NULL);
		hear(0, 1);	/* what each transmitter put on the air this tick */
		hear(1, 0);
		a_on = a_dota_owns_radio();
		b_on = b_dota_owns_radio();
		b_dota_view(&vb);
		if (!a_on && t > 30000u && b_on && vb.phase == DOTA_P_WAIT)
			break;		/* the receiver stays armed until its user leaves */
	}
	if (b_dota_owns_radio())
		b_dota_stop();
	for (int k = 0; k < 400 && b_dota_owns_radio(); k++, now += TICK_MS) {
		dota_io_t ib = { .key = key_b, .beep = beep_none, .clear = true };
		side = 1;
		b_dota_poll(&ib, now);
	}
	while (b_dota_job_wanted())
		b_dota_job(NULL);
	a_dota_view(&va);
	b_dota_view(&vb);
}

static void good(const char *what, double snr, const char *zone)
{
	fresh();
	send_zone = zone;
	snr_db = snr;
	tx_tone[0] = tx_tone[1] = 885;
	rx_tone[0] = rx_tone[1] = 885;
	uint32_t t0 = now;
	run();
	CHECK(va.result == DOTA_RES_SENT && vb.result == DOTA_RES_DONE);
	zone_t z, copy;
	CHECK(find_zone(zone, &z) >= 0 && find_zone("Copy", &copy) >= 0 && copy.count == z.count);
	for (int i = 0; i < copy.count && i < z.count; i++) {
		channel_t a, b;
		CHECK(codeplug_channel(z.channels[i], &a) && codeplug_channel(copy.channels[i], &b));
		CHECK(strcmp(a.name, b.name) == 0 && a.rx_hz == b.rx_hz && a.rx_tone.value == b.rx_tone.value);
	}
	printf("  %-28s done in %.1f s; frames decoded: receiver %u, sender %u; FCS errors %u / %u\n", what,
	       (now - t0) / 1000.0, decoded[1], decoded[0], afsk_demod_fcs_errors(&demod[1]),
	       afsk_demod_fcs_errors(&demod[0]));
}

static uint32_t shown(void)
{
	uint32_t h = 2166136261u;
	channel_t c;
	for (uint16_t n = 1; n <= CODEPLUG_CHANNELS_MAX; n++)
		if (codeplug_channel(n, &c))
			h = (h ^ n ^ c.rx_hz) * 16777619u;
	return (h ^ codeplug_zone_count()) * 16777619u;
}

static void shut(const char *what, uint16_t sender_tone)
{
	fresh();
	send_zone = "Home";
	uint32_t before = shown();
	snr_db = 30;
	tx_tone[0] = sender_tone;
	tx_tone[1] = 885;
	rx_tone[0] = rx_tone[1] = 885;
	run();
	CHECK(decoded[1] > 0);			/* the frames were on the air, demodulated fine */
	CHECK(va.result == DOTA_RES_NO_ANSWER);	/* none was taken: no answer ever went back */
	CHECK(decoded[0] == 0 && shown() == before);
	if (getenv("DOTA_FM_DEBUG"))
		printf("  results %u / %u, phases %u / %u, heard by the sender %u\n", va.result, vb.result, va.phase,
		       vb.phase, decoded[0]);
	printf("  %-34s %u frames demodulated, none taken: \"No answer\", nothing written\n", what, decoded[1]);
}

int main(void)
{
	good("zone of 16, clean", -1, "Home");
	good("zone of 16, noise 20 dB SNR", 20, "Home");
	good("zone of 16, noise 12 dB SNR", 12, "Home");
	good("zone of 64, noise 12 dB SNR", 12, "Big");
	shut("sender without a tone", 0);
	shut("sender on 100.0 Hz, receiver 88.5", 1000);
	printf("dota fm: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
