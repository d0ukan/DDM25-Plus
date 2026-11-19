// SPDX-License-Identifier: GPL-3.0-or-later
// DM32 Plus DTMF / SelCall decoder (integration/radiofoundry/fmdec/selcall.c): 5-tone sequences in ZVEI1, ZVEI2, CCIR
// and EIA (the repeat tone included) and DTMF strings made by this test's own encoders at 8 kHz, with noise, level,
// frequency and timing variations, a lengthened first tone, twist; each set picked by hand too; voice-like audio,
// whistles and noise must decode nothing. Prints how long after the last tone each was decoded.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "selcall.h"

#define FS 8000.0
#define MAXS (8000 * 70)

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t rng = 4242;
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
	return (s - 3.0) * sqrt(2.0);
}

// --- what came out ---
static selcall_seq_t got[16];
static int ngot;
static long got_at[16];
static long cursor;
static void on_seq(void *ctx, const selcall_seq_t *s)
{
	(void)ctx;
	if (ngot < 16) {
		got_at[ngot] = cursor;
		got[ngot++] = *s;
	}
}

// --- signal building ---
static double sig[MAXS];
static int len;
static double phase;

static void tone(double f, double amp, double ms)
{
	int n = (int)(ms * FS / 1000.0);
	for (int i = 0; i < n && len < MAXS; i++) {
		sig[len++] += amp * sin(phase);
		phase += 2 * M_PI * f / FS;
	}
}
static void pair(double f1, double f2, double a1, double a2, double ms)
{
	int n = (int)(ms * FS / 1000.0);
	double p1 = urand() * 6.28, p2 = urand() * 6.28;
	for (int i = 0; i < n && len < MAXS; i++) {
		sig[len++] += a1 * sin(p1) + a2 * sin(p2);
		p1 += 2 * M_PI * f1 / FS;
		p2 += 2 * M_PI * f2 / FS;
	}
}
static void silence(double ms)
{
	int n = (int)(ms * FS / 1000.0);
	for (int i = 0; i < n && len < MAXS; i++)
		sig[len++] += 0;
}
static void reset(void)
{
	memset(sig, 0, sizeof sig);
	len = 0;
	phase = 0;
}

// A 5-tone call: digits as dialled; a digit equal to the tone before goes out as the repeat tone (55555: 5E5E5).
// foff: frequency error, toff: each tone's length error up to this fraction, first_ms: the first tone's length (0:
// normal)
static void sequence(uint8_t set, const char *call, double amp, double foff, double toff, double first_ms)
{
	char prev = 0;
	for (size_t i = 0; call[i]; i++) {
		char c = call[i] == prev ? 'E' : call[i];
		prev = c;
		double ms = selcall_tone_ms(set) * (1 + toff * (2 * urand() - 1));
		if (i == 0 && first_ms > 0)
			ms = first_ms;
		tone(selcall_tone_hz(set, c) * (1 + foff), amp, ms);
	}
}

static const double dtmf_lo[4] = { 697, 770, 852, 941 }, dtmf_hi[4] = { 1209, 1336, 1477, 1633 };
static const char *const dtmf_keys = "123A456B789C*0#D";
static double dtmf_off;	// the sender's frequency error (a fraction)
static void dtmf(const char *s, double amp, double twist_db, double on_ms, double off_ms)
{
	for (; *s; s++) {
		const char *p = strchr(dtmf_keys, *s);
		int k = (int)(p - dtmf_keys);
		double a_lo = amp, a_hi = amp * pow(10, twist_db / 20);
		pair(dtmf_lo[k / 4] * (1 + dtmf_off), dtmf_hi[k % 4] * (1 + dtmf_off), a_lo, a_hi, on_ms);
		silence(off_ms);
	}
}

// Noise, FM receive tilt (de-emphasis, a one-pole low pass near 1 kHz: the normal speaker path) and the decoder
static void decode(selcall_t *d, double noise, bool deemph)
{
	int16_t buf[200];
	double lp = 0;
	ngot = 0;
	for (int i = 0; i < len; i += 200) {
		int n = len - i < 200 ? len - i : 200;
		for (int k = 0; k < n; k++) {
			double x = sig[i + k] + noise * grand();
			if (deemph) {
				lp += 0.55 * (x - lp);
				x = lp * 1.6;
			}
			if (x > 32767) x = 32767;
			if (x < -32768) x = -32768;
			buf[k] = (int16_t)lrint(x);
		}
		cursor = i + n;
		selcall_audio(d, buf, (size_t)n);
	}
}

// A ZVEI call shows which ZVEI only when it holds a repeated digit (the repeat tones differ: 2600 / 970 Hz) or a
// letter only one of them has (ZVEI1 A 2800 Hz, ZVEI2 C 740 Hz; not B: 810 / 825 Hz are 1.8 % apart, either may fit). A set
// picked by hand names itself
static const char *expect_name(uint8_t set, uint8_t only, const char *call)
{
	if (only != SELCALL_ANY || (set != SELCALL_ZVEI1 && set != SELCALL_ZVEI2))
		return selcall_set_name(set);
	for (size_t i = 0; call[i]; i++)
		if ((i > 0 && call[i] == call[i - 1]) || (set == SELCALL_ZVEI1 && call[i] == 'A') ||
		    (set == SELCALL_ZVEI2 && call[i] == 'C'))
			return selcall_set_name(set);
	return "ZVEI";
}

static int runs, ok_runs;
static double t_sum, t_worst;

static void one(uint8_t set, uint8_t only, const char *call, double amp, double noise, double foff, double toff,
		double first_ms, bool deemph, const char *what)
{
	selcall_t d;
	selcall_init(&d, only, on_seq, NULL);
	reset();
	silence(300);
	sequence(set, call, amp, foff, toff, first_ms);
	int end = len;
	silence(400);
	decode(&d, noise, deemph);
	runs++;
	bool ok = ngot == 1 && strcmp(got[0].text, call) == 0 &&
		  strcmp(selcall_set_name(got[0].set), expect_name(set, only, call)) == 0;
	// a call missed (refused: a tone lost to noise) is counted; anything else but the call is a failure
	CHECK(ok || ngot == 0, "%s %s %s: got %d (%s %s)", what, selcall_set_name(set), call, ngot,
	      ngot ? selcall_set_name(got[0].set) : "-", ngot ? got[0].text : "");
	if (!ok && ngot == 0 && noise <= 150)
		CHECK(false, "%s %s %s missed with almost no noise", what, selcall_set_name(set), call);
	if (ok) {
		ok_runs++;
		double t = (got_at[0] - end) / FS;
		t_sum += t;
		if (t > t_worst)
			t_worst = t;
	}
}

// letters: NULL none, else one of them somewhere (not the first: a letter after a digit)
static void random_call(char *call, int n, const char *letters)
{
	for (int i = 0; i < n; i++)
		call[i] = (char)('0' + (int)(urand() * 10));
	if (letters)
		call[1 + (int)(urand() * (n - 1))] = letters[(int)(urand() * strlen(letters))];
	call[n] = 0;
}

static void sets(void)
{
	static const uint8_t all[] = { SELCALL_ZVEI1, SELCALL_ZVEI2, SELCALL_CCIR, SELCALL_EIA };
	// the letters each set can be told by (ZVEI1's C, D, F are ZVEI2's repeat, A, D, and the other way round; B is
	// 810 / 825 Hz, 1.8 % apart: either)
	static const char *const letters[] = { "A", "C", "ABCD", "ABCD" };
	static const char *const fixed[] = { "12345", "11223", "90210", "55555", "10000", "24680" };
	for (unsigned s = 0; s < 4; s++) {
		uint8_t set = all[s];
		runs = ok_runs = 0;
		t_sum = t_worst = 0;
		for (unsigned f = 0; f < sizeof fixed / sizeof fixed[0]; f++)
			one(set, SELCALL_ANY, fixed[f], 3000, 150, 0, 0, 0, false, "fixed");
		for (int k = 0; k < 40; k++) {
			char call[8];
			random_call(call, 5, NULL);
			one(set, SELCALL_ANY, call, 3000, 300, 0, 0.08, 0, k & 1, "random, noise 20 dB, +-8 % lengths");
		}
		for (int k = 0; k < 20; k++) {
			char call[8];
			random_call(call, 5, NULL);
			double foff = (urand() * 2 - 1) * 0.008;
			// tone 3000-8000 against noise 500 rms over 0-4 kHz: 12.6-21 dB
			one(set, SELCALL_ANY, call, 3000 + urand() * 5000, 500, foff, 0.08, 0, k & 1,
			    "SNR 12-21 dB, +-0.8 % frequency, +-8 % lengths, level");
		}
		for (int k = 0; k < 10; k++) {
			char call[8];
			random_call(call, 5, NULL);
			one(set, SELCALL_ANY, call, 3000, 300, 0, 0.05, 600, false, "first tone 600 ms");
		}
		for (int k = 0; k < 10; k++) {
			char call[8];
			random_call(call, 6, k < 5 ? letters[s] : NULL);
			one(set, SELCALL_ANY, call, 3000, 300, 0, 0.05, 0, false, "six tones, letters");
		}
		for (int k = 0; k < 10; k++) {
			char call[8];
			random_call(call, 5, NULL);
			one(set, set, call, 3000, 300, 0, 0.08, 0, false, "this set picked");
		}
		// noisy calls (12-21 dB) may be refused now and then, never read wrong: at most 3 of the 96
		CHECK(runs - ok_runs <= 3, "%s: %d of %d calls missed", selcall_set_name(set), runs - ok_runs, runs);
		printf("%-5s %3d/%3d decoded, none wrong, %.2f s after the last tone on average, %.2f s at worst\n",
		       selcall_set_name(set), ok_runs, runs, ok_runs ? t_sum / ok_runs : 0, t_worst);
	}
}

// A frequency at least 6 % from every tone of every set, near f
static double off_table(double f)
{
	for (;; f *= 1.01) {
		bool near = false;
		for (uint8_t s = SELCALL_ZVEI1; s <= SELCALL_EIA; s++)
			for (const char *c = "0123456789ABCDEF"; *c; c++)
				if (fabs(f / selcall_tone_hz(s, *c) - 1) < 0.06)
					near = true;
		if (!near)
			return f;
	}
}

// A call with a steady whistle right before it and a tone right after (no pause; both 6 % or more from every set's tones): the
// call is still found; a call with a tone lost to a noise burst in the middle is refused, not shown short
static void neighbours(void)
{
	static const uint8_t all[] = { SELCALL_ZVEI1, SELCALL_CCIR, SELCALL_EIA };
	int ok = 0, n = 0, refused = 0;
	for (unsigned s = 0; s < 3; s++)
		for (int k = 0; k < 10; k++) {
			char call[8];
			random_call(call, 5, NULL);
			selcall_t d;
			selcall_init(&d, SELCALL_ANY, on_seq, NULL);
			reset();
			silence(200);
			tone(off_table(900 + 40 * k), 3000, 400 + 50 * k);	// a whistle
			sequence(all[s], call, 3000, 0, 0.05, 0);
			tone(off_table(1900 - 30 * k), 3000, 200);		// and a tone after
			silence(400);
			decode(&d, 300, false);
			// found whole, or refused (the change from the whistle took too long): never short or wrong
			bool good = ngot == 1 && strcmp(got[0].text, call) == 0;
			CHECK(good || ngot == 0, "%s %s between a whistle and a tone: got %d (%s)", selcall_set_name(all[s]),
			      call, ngot, ngot ? got[0].text : "");
			ok += good;
			n++;
			// the third tone drowned in a noise burst
			selcall_init(&d, SELCALL_ANY, on_seq, NULL);
			reset();
			silence(200);
			sequence(all[s], call, 3000, 0, 0.05, 0);
			silence(400);
			int at = (int)(200 * FS / 1000 + 2.4 * selcall_tone_ms(all[s]) * FS / 1000);
			for (int i = at; i < at + (int)(selcall_tone_ms(all[s]) * FS / 1000) && i < len; i++)
				sig[i] = 8000 * grand();
			decode(&d, 300, false);
			CHECK(ngot == 0, "%s %s with a tone lost: got %s", selcall_set_name(all[s]), call, ngot ? got[0].text : "");
			refused += ngot == 0;
		}
	CHECK(ok * 4 >= n * 3, "between a whistle and a tone: only %d/%d decoded", ok, n);
	printf("calls between a whistle and a tone: %d/%d decoded, none wrong; a tone lost in the middle: %d/%d refused\n",
	       ok, n, refused, n);
}

// A set picked by hand hears no other set's calls
static void picked(void)
{
	static const uint8_t all[] = { SELCALL_ZVEI1, SELCALL_CCIR, SELCALL_EIA };
	int heard = 0, n = 0;
	for (unsigned s = 0; s < 3; s++)
		for (unsigned o = 0; o < 3; o++) {
			if (s == o)
				continue;
			for (int k = 0; k < 10; k++) {
				char call[8];
				random_call(call, 5, NULL);
				selcall_t d;
				selcall_init(&d, all[o], on_seq, NULL);
				reset();
				silence(200);
				sequence(all[s], call, 3000, 0, 0.05, 0);
				silence(400);
				decode(&d, 300, false);
				if (ngot)
					printf("  %s call %s heard as %s %s\n", selcall_set_name(all[s]), call,
					       selcall_set_name(got[0].set), got[0].text);
				heard += ngot;
				n++;
			}
		}
	CHECK(heard == 0, "a picked set decoded %d of %d calls of other sets", heard, n);
	printf("picked set: %d of %d other sets' calls decoded\n", heard, n);
}

static void dtmf_tests(void)
{
	static const char *const strings[] = { "1234#", "*0#", "A9D", "0123456789", "*#ABCD", "5", "99" };
	int n = 0, ok = 0;
	for (unsigned i = 0; i < sizeof strings / sizeof strings[0]; i++)
		for (int v = 0; v < 8; v++) {
			dtmf_off = v == 6 ? 0.015 : v == 7 ? -0.015 : 0;	// v 6, 7: tones 1.5 % off
			selcall_t d;
			selcall_init(&d, SELCALL_ANY, on_seq, NULL);
			reset();
			silence(200);
			// twist: high group 4 dB up / 6 dB down; v 5 through the speaker path's de-emphasis
			double twist = v == 1 ? 4 : v == 2 ? -6 : 0;
			double on = v == 3 ? 45 : v == 4 ? 120 : 70;
			double off = v == 3 ? 40 : v == 5 ? 300 : 60;
			dtmf(strings[i], 2500, twist, on, off);
			int end = len;
			silence(2000);
			decode(&d, v == 5 ? 500 : 250, v == 5);
			bool good = ngot == 1 && got[0].set == SELCALL_DTMF && strcmp(got[0].text, strings[i]) == 0;
			CHECK(good, "DTMF %s variant %d: got %d (%s)", strings[i], v, ngot, ngot ? got[0].text : "");
			n++;
			if (good) {
				ok++;
				// the string ends 1.5 s after its last digit was taken (34 ms into the tone)
				double after = (got_at[0] - (end - (on + off) * FS / 1000)) / FS;
				CHECK(after >= 1.45 && after <= 1.65, "DTMF %s ended %.2f s after its last digit", strings[i],
				      after);
			}
		}
	printf("DTMF: %d/%d strings (twist +4 / -6 dB, 45-120 ms tones, 40-300 ms gaps, +-1.5 %%, noise) decoded, each ~1.5 s after "
	       "its last digit\n", ok, n);
	// two strings 3 s apart are two entries; a 10 ms blip is no digit (FM caller ID's decoder takes a digit two of its
	// 17 ms blocks hold: 40 ms tones always, 20 ms ones in silence too)
	selcall_t d;
	selcall_init(&d, SELCALL_ANY, on_seq, NULL);
	reset();
	dtmf("12", 2500, 0, 70, 60);
	silence(3000);
	dtmf("#", 2500, 0, 70, 60);
	pair(697, 1209, 2500, 2500, 10);
	silence(2000);
	decode(&d, 250, false);
	CHECK(ngot == 2 && strcmp(got[0].text, "12") == 0 && strcmp(got[1].text, "#") == 0, "two strings: %d (%s %s)", ngot,
	      ngot > 0 ? got[0].text : "", ngot > 1 ? got[1].text : "");
	// DTMF makes no SelCall, SelCall no DTMF (checked above: exactly one result each)
}

// Voice-like audio, gliding whistles and noise: nothing decoded
static void quiet(void)
{
	selcall_t d;
	selcall_init(&d, SELCALL_ANY, on_seq, NULL);
	reset();
	double f0ph = 0, wph = 0;
	for (int i = 0; i < MAXS; i++) {
		double t = i / FS;
		double f0 = 110 + 50 * sin(2 * M_PI * 0.31 * t) + 20 * sin(2 * M_PI * 1.7 * t);
		f0ph += 2 * M_PI * f0 / FS;
		double v = 0;
		for (int h = 1; h <= 30; h++) {
			double f = h * f0;
			if (f > 3400)
				break;
			double g = exp(-pow((f - 700) / 250, 2)) + 0.6 * exp(-pow((f - 1200) / 300, 2)) +
				   0.3 * exp(-pow((f - 2400) / 400, 2));
			v += g * sin(h * f0ph + h * 0.9);
		}
		double env = 0.5 + 0.5 * sin(2 * M_PI * 3.1 * t);
		double s = 4000 * v * env * env;
		// every 10 s a whistle gliding 800 -> 2500 Hz over 2 s, and a held one at 1530 Hz for 1 s
		double tw = fmod(t, 10.0);
		if (tw > 5 && tw < 7) {
			double f = 800 + 850 * (tw - 5);
			wph += 2 * M_PI * f / FS;
			s += 3000 * sin(wph);
		} else if (tw > 8 && tw < 9) {
			wph += 2 * M_PI * 1530 / FS;
			s += 3000 * sin(wph);
		}
		sig[i] = s;
	}
	len = MAXS;
	decode(&d, 300, false);
	selcall_flush(&d);
	CHECK(ngot == 0, "voice / whistles decoded %d: %s %s", ngot, ngot ? selcall_set_name(got[0].set) : "",
	      ngot ? got[0].text : "");
	printf("voice-like audio with whistles, %d s: %d decoded (%u tone segments seen, %u sequences and %u DTMF digits "
	       "refused)\n", MAXS / 8000, ngot, d.segs_seen, d.seqs_rejected, d.dtmf_refused);
	selcall_init(&d, SELCALL_ANY, on_seq, NULL);
	reset();
	len = MAXS;
	decode(&d, 2000, false);
	selcall_flush(&d);
	CHECK(ngot == 0, "noise decoded %d", ngot);
	printf("noise, %d s: %d decoded\n", MAXS / 8000, ngot);
}

int main(int argc, char **argv)
{
	if (argc > 1)	// another seed for the random calls
		rng = (uint32_t)strtoul(argv[1], NULL, 0);
	CHECK(selcall_tone_hz(SELCALL_ZVEI1, 'E') == 2600 && selcall_tone_hz(SELCALL_CCIR, 'E') == 2110 &&
	      selcall_tone_hz(SELCALL_EIA, 'E') == 459 && selcall_tone_hz(SELCALL_ZVEI2, '0') == 2400, "tables");
	sets();
	neighbours();
	picked();
	dtmf_tests();
	quiet();
	printf("SelCall / DTMF: %d failures\n", fails);
	return fails != 0;
}
