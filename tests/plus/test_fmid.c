// SPDX-License-Identifier: GPL-3.0-or-later
// DM32 Plus FM caller ID decoders (integration/radiofoundry/fmid/fmid.c): MDC1200, FleetSync I and DTMF bursts made
// by this test's own encoders at 8 kHz, with noise, level, tone and clock offsets, de-emphasis tilt and an inverted
// audio sign; noise and voice-like audio must not produce an ID.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fmid.h"

#define FS 8000.0
#define MAXS (8000 * 12)

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// --- events ---
static fmid_event_t ev[64];
static int nev;
static char digits[64];
static void on_ev(void *ctx, const fmid_event_t *e)
{
	(void)ctx;
	if (nev < 64)
		ev[nev++] = *e;
	if (e->kind == FMID_DTMF) {
		size_t n = strlen(digits);
		if (n + 1 < sizeof digits) {
			digits[n] = e->digit;
			digits[n + 1] = 0;
		}
	}
}

// --- signal building ---
static double sig[MAXS];
static int16_t pcm[MAXS];
static int len;
static uint32_t rng = 12345;
static double urand(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return (rng & 0xFFFFFF) / (double)0x1000000;
}
static double grand(void)
{
	double s = 0;
	for (int i = 0; i < 6; i++)
		s += urand();
	return (s - 3.0) * sqrt(2.0);	// about unit variance
}

static void silence(double ms)
{
	int n = (int)(ms * FS / 1000.0);
	for (int i = 0; i < n && len < MAXS; i++)
		sig[len++] = 0;
}

// Phase continuous FFSK: t = 1 sends 1800 Hz, 0 sends 1200 Hz (tones scaled by tone_k, the clock by baud_k)
static double phase;
static void ffsk(const uint8_t *t, int nbits, double amp, double tone_k, double baud_k)
{
	double spb = FS / (1200.0 * baud_k);
	double start = len;
	for (int b = 0; b < nbits; b++) {
		int end = (int)(start + (b + 1) * spb + 0.5);
		double f = (t[b] ? 1800.0 : 1200.0) * tone_k;
		while (len < end && len < MAXS) {
			sig[len++] = amp * sin(phase);
			phase += 2 * M_PI * f / FS;
		}
	}
}

static void tones(double f1, double f2, double a1, double a2, double ms)
{
	int n = (int)(ms * FS / 1000.0);
	double p1 = urand() * 6.28, p2 = urand() * 6.28;
	for (int i = 0; i < n && len < MAXS; i++) {
		sig[len++] = a1 * sin(p1) + a2 * sin(p2);
		p1 += 2 * M_PI * f1 / FS;
		p2 += 2 * M_PI * f2 / FS;
	}
}

// Finish: de-emphasis tilt (a first-order low pass at fc Hz, 0 = none), noise at rms, sign, quantize; run the decoder
// in uneven chunks as the radio task would
static fmid_t dec;
static void run(double fc, double noise_rms, int sign)
{
	double a = fc > 0 ? 1.0 - exp(-2 * M_PI * fc / FS) : 1.0, y = 0;
	for (int i = 0; i < len; i++) {
		y = fc > 0 ? y + a * (sig[i] - y) : sig[i];
		double v = sign * (fc > 0 ? y * 2.0 : y) + noise_rms * grand() + 300.0;	// a DC offset too
		if (v > 32767)
			v = 32767;
		if (v < -32768)
			v = -32768;
		pcm[i] = (int16_t)lrint(v);
	}
	nev = 0;
	digits[0] = 0;
	fmid_init(&dec, on_ev, NULL);
	int i = 0, k = 0;
	while (i < len) {
		int n = 17 + (k++ * 37) % 190;
		if (n > len - i)
			n = len - i;
		fmid_audio(&dec, &pcm[i], (size_t)n);
		i += n;
	}
}

static void reset(void)
{
	len = 0;
	phase = 0;
}

// --- MDC1200 encoder (fmid.h's description of the format) ---
static int mdc_bits(uint8_t op, uint8_t arg, uint16_t unit, int flip_a, int flip_b, uint8_t *t)
{
	uint8_t cw[14];
	memset(cw, 0, sizeof cw);
	cw[0] = op;
	cw[1] = arg;
	cw[2] = (uint8_t)(unit >> 8);
	cw[3] = (uint8_t)unit;
	uint16_t crc = fmid_mdc_crc(cw, 4);
	cw[4] = (uint8_t)crc;
	cw[5] = (uint8_t)(crc >> 8);
	fmid_mdc_parity(cw, &cw[7]);
	if (flip_a >= 0)
		cw[flip_a >> 3] ^= (uint8_t)(1u << (flip_a & 7));	// channel errors the FEC must fix
	if (flip_b >= 0)
		cw[flip_b >> 3] ^= (uint8_t)(1u << (flip_b & 7));
	uint8_t d[7 * 8 + 40 + 112];
	int n = 0;
	for (int i = 0; i < 7 * 8; i++)
		d[n++] = (uint8_t)(i & 1);	// leader 0x55...: a steady 1800 Hz once precoded
	uint64_t sync = 0x07092A446FULL;
	for (int i = 39; i >= 0; i--)
		d[n++] = (uint8_t)((sync >> i) & 1u);
	for (int k = 0; k < 112; k++) {
		int i = (k % 16) * 7 + k / 16;	// the bit sent at k
		d[n++] = (uint8_t)((cw[i >> 3] >> (i & 7)) & 1u);
	}
	uint8_t last = 1;	// the bit before the leader (any start state)
	for (int i = 0; i < n; i++) {
		t[i] = d[i] ^ last;	// 1800 Hz where the data changes
		last = d[i];
	}
	t[n++] = 1;	// a tail bit
	return n;
}

static void mdc(uint8_t op, uint8_t arg, uint16_t unit, double amp, double tone_k, double baud_k)
{
	uint8_t t[256];
	int n = mdc_bits(op, arg, unit, -1, -1, t);
	ffsk(t, n, amp, tone_k, baud_k);
}

// --- FleetSync I encoder ---
static int fs_bits(uint64_t block, uint8_t *t)
{
	int n = 0;
	for (int i = 0; i < 24; i++)
		t[n++] = (uint8_t)((i & 1) == 0);	// 1010...
	for (int i = 15; i >= 0; i--)
		t[n++] = (uint8_t)((0xA23Eu >> i) & 1u);
	for (int i = 0; i < 4; i++)
		t[n++] = (uint8_t)(i & 1);
	for (int i = 63; i >= 0; i--)
		t[n++] = (uint8_t)((block >> i) & 1u);
	for (int i = 0; i < 64; i++)
		t[n++] = (uint8_t)(urand() < 0.5);	// the rest of the frame
	return n;
}

static uint64_t fs_block(unsigned fleet, unsigned unit, uint8_t cmd)
{
	unsigned f = fleet - 99u, u = unit - 999u;
	uint32_t w1 = ((uint32_t)cmd << 24) | (0x80u << 16) | (f << 8) | (u >> 4);
	uint32_t w2hi = ((u & 15u) << 12) | 0x305u;
	uint64_t b = ((uint64_t)w1 << 32) | ((uint64_t)w2hi << 16);
	return b | fmid_fs_check(b);
}

static void fleetsync(uint64_t block, double amp, double tone_k, double baud_k, int invert)
{
	uint8_t t[256];
	int n = fs_bits(block, t);
	for (int i = 0; invert && i < n; i++)
		t[i] ^= 1u;
	ffsk(t, n, amp, tone_k, baud_k);
}

// --- DTMF ---
static const double rows[4] = { 697, 770, 852, 941 }, cols[4] = { 1209, 1336, 1477, 1633 };
static const char *keys = "123A456B789C*0#D";
static void dtmf(const char *s, double amp, double twist_db, double f_k, double on_ms, double off_ms)
{
	for (; *s; s++) {
		int k = (int)(strchr(keys, *s) - keys);
		double ac = amp * pow(10.0, twist_db / 20.0);
		tones(rows[k / 4] * f_k, cols[k % 4] * f_k, amp, ac, on_ms);
		silence(off_ms);
	}
}

// --- voice-like audio: a glottal pulse train with a sliding pitch through two formant resonators ---
static void voice(double ms, double amp)
{
	int n = (int)(ms * FS / 1000.0);
	double y1[2] = { 0, 0 }, y2[2] = { 0, 0 }, ph = 0;
	for (int i = 0; i < n && len < MAXS; i++) {
		double f0 = 110 + 60 * sin(2 * M_PI * i / (FS * 0.7)) + 20 * sin(2 * M_PI * i / (FS * 0.13));
		ph += f0 / FS;
		double x = 0;
		if (ph >= 1) {
			ph -= 1;
			x = 1;
		}
		double fa = 700 + 300 * sin(2 * M_PI * i / (FS * 0.31)), fb = 1200 + 500 * sin(2 * M_PI * i / (FS * 0.23));
		double r = 0.97;
		double a1 = 2 * r * cos(2 * M_PI * fa / FS), b1 = 2 * r * cos(2 * M_PI * fb / FS);
		double v1 = x + a1 * y1[0] - r * r * y1[1];
		y1[1] = y1[0];
		y1[0] = v1;
		double v2 = v1 + b1 * y2[0] - r * r * y2[1];
		y2[1] = y2[0];
		y2[0] = v2;
		sig[len++] = amp * v2 / 40.0;
	}
}

static int count(uint8_t kind)
{
	int c = 0;
	for (int i = 0; i < nev; i++)
		c += ev[i].kind == kind;
	return c;
}

static int ids(void)
{
	return count(FMID_MDC) + count(FMID_MDC_EMERG) + count(FMID_FLEETSYNC);
}

static void test_checks(void)
{
	// the FleetSync blocks of the GopherTrunk article (fleet 107, unit 1772)
	CHECK(fmid_fs_check(0xFE80083053059B6CULL) == 0x9B6C, "fs check 1: %04x", fmid_fs_check(0xFE80083053059B6CULL));
	CHECK(fmid_fs_check(0xFC80083053057E59ULL) == 0x7E59, "fs check 2: %04x", fmid_fs_check(0xFC80083053057E59ULL));
	// CRC-16, polynomial 0x1021 reflected, start 0, final XOR 0xFFFF: "123456789" gives 0x906E (CRC-16/X.25 starts
	// at 0xFFFF; this variant's check value was computed independently with a bitwise reference below)
	const uint8_t m[] = "123456789";
	uint16_t ref = 0;
	for (int i = 0; i < 9; i++) {
		for (int b = 0; b < 8; b++) {
			unsigned in = (m[i] >> b) & 1u, top = (ref >> 15) & 1u;
			ref = (uint16_t)(ref << 1);
			if (in ^ top)
				ref ^= 0x1021u;
		}
	}
	uint16_t rev = 0;
	for (int b = 0; b < 16; b++)
		rev = (uint16_t)(rev | (((ref >> b) & 1u) << (15 - b)));
	CHECK(fmid_mdc_crc(m, 9) == (uint16_t)(rev ^ 0xFFFFu), "mdc crc %04x vs %04x", fmid_mdc_crc(m, 9), rev ^ 0xFFFFu);
}

static void test_mdc(void)
{
	struct { double amp, noise, tone_k, baud_k, fc; int sign; } c[] = {
		{ 8000, 0, 1, 1, 0, 1 }, { 8000, 800, 1, 1, 0, 1 }, { 3000, 400, 1, 1, 0, -1 }, { 12000, 1500, 1.01, 1.005, 0, 1 },
		{ 8000, 600, 0.99, 0.995, 900, 1 }, { 20000, 2000, 1, 1, 600, -1 }, { 1500, 150, 1, 1, 0, 1 },
	};
	for (unsigned k = 0; k < sizeof c / sizeof c[0]; k++) {
		reset();
		voice(400, c[k].amp);
		silence(30);
		mdc(0x01, 0x80, 0x1777, c[k].amp, c[k].tone_k, c[k].baud_k);
		silence(200);
		voice(300, c[k].amp);
		mdc(0x00, 0x80, 0x0042, c[k].amp, c[k].tone_k, c[k].baud_k);
		silence(100);
		run(c[k].fc, c[k].noise, c[k].sign);
		CHECK(count(FMID_MDC) == 1 && count(FMID_MDC_EMERG) == 1 && nev == 2, "mdc case %u: %d events, syncs %u crc bad %u",
		      k, nev, dec.mdc_syncs, dec.mdc_crc_bad);
		for (int i = 0; i < nev; i++) {
			if (ev[i].kind == FMID_MDC)
				CHECK(ev[i].unit == 0x1777 && ev[i].op == 0x01 && ev[i].arg == 0x80, "mdc case %u unit %04x", k, ev[i].unit);
			if (ev[i].kind == FMID_MDC_EMERG)
				CHECK(ev[i].unit == 0x0042, "mdc case %u emergency unit %04x", k, ev[i].unit);
		}
	}
	// two codeword bits wrong (a data bit and a parity bit, apart): the FEC fixes them
	for (int a = 0; a < 56; a += 5) {
		reset();
		silence(50);
		uint8_t t[256];
		int n = mdc_bits(0x01, 0x00, 0x2345, a, 56 + (a + 23) % 56, t);
		ffsk(t, n, 8000, 1, 1);
		silence(50);
		run(0, 200, 1);
		CHECK(count(FMID_MDC) == 1 && ev[0].unit == 0x2345, "mdc fec bit %d: %d events", a, nev);
	}
	// other ops (here a status, 0x46) are not IDs
	reset();
	silence(50);
	mdc(0x46, 0x03, 0x1234, 8000, 1, 1);
	silence(50);
	run(0, 200, 1);
	CHECK(nev == 0, "mdc status op gave %d events", nev);
}

static void test_fleetsync(void)
{
	struct { double amp, noise, tone_k, baud_k, fc; int sign, invert; } c[] = {
		{ 8000, 0, 1, 1, 0, 1, 0 }, { 8000, 800, 1, 1, 0, 1, 1 }, { 3000, 500, 1.01, 0.995, 0, -1, 0 },
		{ 10000, 1200, 0.99, 1.005, 900, 1, 1 }, { 1500, 150, 1, 1, 600, 1, 0 },
	};
	for (unsigned k = 0; k < sizeof c / sizeof c[0]; k++) {
		reset();
		voice(300, c[k].amp);
		fleetsync(0xFE80083053059B6CULL, c[k].amp, c[k].tone_k, c[k].baud_k, c[k].invert);
		silence(100);
		fleetsync(fs_block(349, 4999, 0xFE), c[k].amp, c[k].tone_k, c[k].baud_k, c[k].invert);
		silence(100);
		run(c[k].fc, c[k].noise, c[k].sign);
		CHECK(count(FMID_FLEETSYNC) == 2 && nev == 2, "fleetsync case %u: %d events, syncs %u crc bad %u", k, nev,
		      dec.fs_syncs, dec.fs_crc_bad);
		if (nev == 2) {
			CHECK(ev[0].fleet == 107 && ev[0].unit == 1772, "fleetsync case %u: %u-%u", k, ev[0].fleet, ev[0].unit);
			CHECK(ev[1].fleet == 349 && ev[1].unit == 4999, "fleetsync case %u: %u-%u", k, ev[1].fleet, ev[1].unit);
		}
	}
}

static void test_dtmf(void)
{
	struct { const char *s; double amp, twist, f_k, on, off, noise, fc; } c[] = {
		{ "1234567890*#ABCD", 4000, 0, 1, 80, 60, 0, 0 },
		{ "911", 3000, 3, 1.015, 60, 60, 300, 0 },
		{ "5550100", 2000, -4, 0.985, 50, 50, 200, 0 },
		{ "#77*", 6000, 6, 1, 100, 100, 500, 1500 },
		{ "2580", 800, 0, 1.005, 70, 50, 60, 0 },
		{ "1470", 3000, 2, 0.99, 40, 40, 150, 0 },	// the shortest tones and pauses taken
	};
	for (unsigned k = 0; k < sizeof c / sizeof c[0]; k++) {
		reset();
		silence(100);
		dtmf(c[k].s, c[k].amp, c[k].twist, c[k].f_k, c[k].on, c[k].off);
		silence(200);
		run(c[k].fc, c[k].noise, 1);
		CHECK(strcmp(digits, c[k].s) == 0 && ids() == 0, "dtmf case %u: \"%s\" for \"%s\"", k, digits, c[k].s);
	}
	// a 20 ms burst is too short; a single tone is not a digit
	reset();
	silence(100);
	dtmf("5", 4000, 0, 1, 20, 100);
	tones(1209, 1209, 2000, 2000, 300);
	silence(100);
	run(0, 50, 1);
	CHECK(nev == 0, "short / single tone gave %d events (\"%s\")", nev, digits);
}

static void test_false(void)
{
	// white noise (an open squelch with no carrier), 10 s at several levels
	for (int lvl = 0; lvl < 3; lvl++) {
		reset();
		silence(10000);
		run(0, lvl == 0 ? 200 : lvl == 1 ? 3000 : 12000, 1);
		CHECK(nev == 0, "noise level %d gave %d events", lvl, nev);
	}
	// voice-like audio, 10 s, plain and with noise
	for (int lvl = 0; lvl < 2; lvl++) {
		reset();
		voice(10000, 9000);
		run(lvl ? 1200 : 0, lvl ? 500 : 0, 1);
		CHECK(nev == 0, "voice %d gave %d events (\"%s\")", lvl, nev, digits);
	}
	// a steady 1500 Hz tone and a 1200 / 1800 Hz alternation without a sync (a test tone, other signaling)
	reset();
	tones(1500, 1500, 4000, 4000, 2000);
	uint8_t t[2400];
	for (int i = 0; i < 2400; i++)
		t[i] = (uint8_t)(urand() < 0.5);
	ffsk(t, 2400, 8000, 1, 1);
	run(0, 300, 1);
	CHECK(ids() == 0, "random FFSK gave %d IDs", ids());
}

int main(void)
{
	test_checks();
	test_mdc();
	test_fleetsync();
	test_dtmf();
	test_false();
	if (fails) {
		printf("test_fmid: %d failures\n", fails);
		return 1;
	}
	printf("test_fmid: ok (MDC1200, FleetSync I, DTMF, no false IDs)\n");
	return 0;
}
