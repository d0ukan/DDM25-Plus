// SPDX-License-Identifier: GPL-3.0-or-later
// DM32 Plus tone finder (integration/radiofoundry/fmdec/tonefind.c): synthetic flat FM receive audio at 8 kHz with
// every CTCSS tone and every DCS code (normal and inverted) under voice and noise, encoder frequency and bit-rate
// errors, the receiver's AC coupling, levels from weak to near full scale and an inverted audio sign; neighbouring
// tones must never be named, voice and noise alone must name nothing, and a DCS signal (its turn-off tail included)
// must not read as CTCSS. Prints the detection times.
//
// Levels: 1 kHz of deviation is taken as 2000 counts (the HR_C7000's receive RAM scale is not measured yet; the
// decoder only uses ratios, and the level tests cover 20x either way). CTCSS and DCS at 0.5 kHz (the usual 0.35-0.75
// kHz), voice peaks near 2.5 kHz behind the transmitter's 300 Hz high pass and pre-emphasis (the flat path leaves the
// pre-emphasis in), white noise.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tonefind.h"
#include "radio/tones.h"

#define FS 8000.0
#define DEV_KHZ 2000.0		// counts per kHz of deviation

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

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
	return (s - 3.0) * sqrt(2.0);
}

// --- a second-order section (RBJ cookbook), double ---
typedef struct { double b0, b1, b2, a1, a2, x1, x2, y1, y2; } biquad_t;
static void bq_set(biquad_t *q, bool high, double f, double qf)
{
	double w = 2 * M_PI * f / FS, c = cos(w), al = sin(w) / (2 * qf), a0 = 1 + al;
	memset(q, 0, sizeof *q);
	if (high) {
		q->b0 = (1 + c) / 2 / a0; q->b1 = -(1 + c) / a0; q->b2 = (1 + c) / 2 / a0;
	} else {
		q->b0 = (1 - c) / 2 / a0; q->b1 = (1 - c) / a0; q->b2 = (1 - c) / 2 / a0;
	}
	q->a1 = -2 * c / a0; q->a2 = (1 - al) / a0;
}
static double bq(biquad_t *q, double x)
{
	double y = q->b0 * x + q->b1 * q->x1 + q->b2 * q->x2 - q->a1 * q->y1 - q->a2 * q->y2;
	q->x2 = q->x1; q->x1 = x; q->y2 = q->y1; q->y1 = y;
	return y;
}

// --- the received audio ---
typedef struct {
	// what is sent
	int kind;		// 0 none, 1 CTCSS, 2 DCS
	double hz;		// CTCSS
	uint32_t word;		// DCS: the 23 bits sent (already inverted for an inverted code)
	double baud;
	double sub_khz;		// sub-audio deviation
	double voice_khz;	// voice peak deviation, 0 none
	double noise;		// noise rms, kHz of deviation
	double level;		// overall scale (1 = DEV_KHZ counts per kHz)
	bool negate;		// the receive path inverts the sign
	double ac_hz;		// the receive path's coupling corner (0: 20 Hz)
	// state
	double t, ph, f0ph, bitpos;
	int bit;
	biquad_t vhp[2], dlp, ac;
	double pre_x;
} rx_t;

static void rx_init(rx_t *r)
{
	r->t = 0; r->ph = urand() * 2 * M_PI; r->f0ph = 0; r->bitpos = urand() * 23; r->bit = (int)r->bitpos;
	r->pre_x = 0;
	bq_set(&r->vhp[0], true, 300, 0.541);	// 4th-order Butterworth high pass at 300 Hz: the transmitter's voice filter
	bq_set(&r->vhp[1], true, 300, 1.307);
	bq_set(&r->dlp, false, 300, 0.707);	// the DCS encoder's shaping low pass
	bq_set(&r->ac, true, r->ac_hz > 0 ? r->ac_hz : 20, 0.707);	// coupling capacitors on the receive side
}

// A voice-like sound: a gliding pitch, 30 harmonics through three formants, syllables
static double voice(rx_t *r)
{
	double t = r->t;
	double f0 = 120 + 40 * sin(2 * M_PI * 0.37 * t) + 15 * sin(2 * M_PI * 2.1 * t);
	r->f0ph += 2 * M_PI * f0 / FS;
	double s = 0;
	for (int h = 1; h <= 30; h++) {
		double f = h * f0;
		if (f > 3400)
			break;
		double g = exp(-pow((f - 650) / 250, 2)) + 0.6 * exp(-pow((f - 1250) / 300, 2)) +
			   0.3 * exp(-pow((f - 2500) / 400, 2)) + 0.4 * exp(-f / 600);
		s += g * sin(h * r->f0ph + h * h * 0.7);
	}
	double env = 0.5 + 0.5 * sin(2 * M_PI * 3.3 * t);
	return s * env * env * 0.45;
}

static int16_t rx_sample(rx_t *r)
{
	double sub = 0;
	if (r->kind == 1) {
		sub = sin(r->ph);
		r->ph += 2 * M_PI * r->hz / FS;
	} else if (r->kind == 2) {
		double lvl = (r->word >> r->bit) & 1u ? 1.0 : -1.0;
		sub = bq(&r->dlp, lvl);
		r->bitpos += r->baud / FS;
		if (r->bitpos >= 23)
			r->bitpos -= 23;
		r->bit = (int)r->bitpos;
	}
	double v = 0;
	if (r->voice_khz > 0) {
		v = voice(r);
		v = bq(&r->vhp[1], bq(&r->vhp[0], v));
		double pre = v - 0.85 * r->pre_x;	// pre-emphasis (rising 6 dB / octave above a few hundred Hz)
		r->pre_x = v;
		v = pre * r->voice_khz;
	}
	double x = (sub * r->sub_khz + v + r->noise * grand()) * DEV_KHZ * r->level;
	x = bq(&r->ac, x);
	if (r->negate)
		x = -x;
	r->t += 1 / FS;
	if (x > 32767) x = 32767;
	if (x < -32768) x = -32768;
	return (int16_t)lrint(x);
}

// Run seconds of audio through d; first: when the expected result first held (s), -1 never. wrong: another result
// was shown at some point (type / value written to *wtype / *wvalue)
typedef struct { double first; int wrong; uint8_t wtype; uint16_t wvalue; } run_t;
static run_t run(tonef_t *d, rx_t *r, double seconds, uint8_t type, uint16_t value)
{
	run_t o = { -1, 0, 0, 0 };
	int16_t buf[200];
	int n = (int)(seconds * FS);
	for (int i = 0; i < n; i += 200) {
		for (int k = 0; k < 200; k++)
			buf[k] = rx_sample(r);
		tonef_audio(d, buf, 200);
		if (d->type != TONEF_NONE) {
			if (d->type == type && d->value == value) {
				if (o.first < 0)
					o.first = (i + 200) / FS;
			} else if (!o.wrong) {
				o.wrong = 1;
				o.wtype = d->type;
				o.wvalue = d->value;
			}
		}
	}
	return o;
}

static void ctcss_all(const char *what, double sub_khz, double voice_khz, double noise, double off, double level,
		      double limit)
{
	double sum = 0, worst = 0;
	int found = 0;
	for (unsigned i = 0; i < TONES_CTCSS_COUNT; i++) {
		tonef_t d;
		tonef_init(&d, false);
		rx_t r = { .kind = 1, .hz = tones_ctcss[i] / 10.0 * (1 + off), .sub_khz = sub_khz, .voice_khz = voice_khz,
			   .noise = noise, .level = level };
		rx_init(&r);
		run_t o = run(&d, &r, 4.0, TONEF_CTCSS, tones_ctcss[i]);
		if (limit > 0)	/* (limit 0: only never another tone) */
			CHECK(o.first >= 0 && o.first <= limit, "%s: CTCSS %u.%u found at %.2f s", what,
			      tones_ctcss[i] / 10, tones_ctcss[i] % 10, o.first);
		CHECK(!o.wrong, "%s: CTCSS %u.%u read as type %u value %u", what, tones_ctcss[i] / 10, tones_ctcss[i] % 10,
		      o.wtype, o.wvalue);
		if (o.first >= 0) {
			found++;
			sum += o.first;
			if (o.first > worst)
				worst = o.first;
		}
	}
	printf("CTCSS %-46s %2d/50 found, mean %.2f s, worst %.2f s\n", what, found, found ? sum / found : 0, worst);
}

// The 23 bits of a code as sent (inverted codes: every bit flipped)
static uint32_t sent_word(uint16_t code, bool inv)
{
	uint32_t w = tonef_dcs_word(code);
	return inv ? ~w & 0x7FFFFFu : w;
}

// Whether two words are the same periodic stream (a rotation)
static bool same_stream(uint32_t a, uint32_t b)
{
	for (int k = 0; k < 23; k++) {
		if (a == b)
			return true;
		a = ((a >> 1) | (a << 22)) & 0x7FFFFFu;
	}
	return false;
}

static void dcs_all(const char *what, bool inv, double noise, double voice_khz, double baud_off, bool negate,
		    double ac_hz, double limit, int need)
{
	double sum = 0, worst = 0;
	int found = 0, exact = 0;
	for (unsigned i = 0; i < TONES_DCS_COUNT; i++) {
		uint16_t code = tones_dcs[i];
		tonef_t d;
		tonef_init(&d, negate);
		rx_t r = { .kind = 2, .word = sent_word(code, inv), .baud = 134.4 * (1 + baud_off), .sub_khz = 0.5,
			   .voice_khz = voice_khz, .noise = noise, .level = 1, .negate = negate, .ac_hz = ac_hz };
		rx_init(&r);
		// what the decoder must name: the preferred description of this stream
		int16_t buf[200];
		double first = -1;
		bool wrong = false, ctcss = false;
		uint8_t wt = 0;
		uint16_t wv = 0;
		for (int s = 0; s < (int)(3.0 * FS); s += 200) {
			for (int k = 0; k < 200; k++)
				buf[k] = rx_sample(&r);
			tonef_audio(&d, buf, 200);
			if (d.type == TONEF_CTCSS)
				ctcss = true;
			if (d.type == TONEF_DCS || d.type == TONEF_DCS_INV) {
				bool ok = same_stream(sent_word(d.value, d.type == TONEF_DCS_INV), r.word);
				if (ok && first < 0) {
					first = (s + 200) / FS;
					if (d.value == code && (d.type == TONEF_DCS_INV) == inv)
						exact++;
				}
				if (!ok && !wrong) {
					wrong = true;
					wt = d.type;
					wv = d.value;
				}
			}
		}
		if (need >= 104)
			CHECK(first >= 0 && first <= limit, "%s: D%03o%c found at %.2f s", what, code, inv ? 'I' : 'N', first);
		CHECK(!wrong, "%s: D%03o%c read as type %u D%03o", what, code, inv ? 'I' : 'N', wt, wv);
		CHECK(!ctcss, "%s: D%03o%c read as CTCSS", what, code, inv ? 'I' : 'N');
		if (first >= 0) {
			found++;
			sum += first;
			if (first > worst)
				worst = first;
		}
	}
	CHECK(found >= need, "%s: %d found, %d needed", what, found, need);
	printf("DCS %-50s %3d/104 found (%3d named as sent, the rest as an equivalent), mean %.2f s, worst %.2f s\n",
	       what, found, exact, found ? sum / found : 0, worst);
}

// Nothing may be named from audio without a sub-audible tone
static void quiet(const char *what, double voice_khz, double noise, double seconds)
{
	tonef_t d;
	tonef_init(&d, false);
	rx_t r = { .kind = 0, .voice_khz = voice_khz, .noise = noise, .level = 1 };
	rx_init(&r);
	run_t o = { -1, 0, 0, 0 };
	int16_t buf[200];
	uint8_t rmax = 0;
	for (int i = 0; i < (int)(seconds * FS); i += 200) {
		for (int k = 0; k < 200; k++)
			buf[k] = rx_sample(&r);
		tonef_audio(&d, buf, 200);
		if (d.ratio > rmax)
			rmax = d.ratio;
		if (d.type != TONEF_NONE && !o.wrong) {
			o.wrong = 1;
			o.wtype = d.type;
			o.wvalue = d.value;
		}
	}
	CHECK(!o.wrong, "%s: named type %u value %u", what, o.wtype, o.wvalue);
	CHECK(d.idle >= 6, "%s: idle %u", what, d.idle);
	printf("quiet %-46s nothing named in %.0f s (strongest tone's share at most %u %%)\n", what, seconds,
	       rmax * 100u / 256u);
}

static void words(void)
{
	// the same words as drivers/rf/subaudio (023 -> 0x763813, the datasheets' example)
	CHECK(tonef_dcs_word(0023) == 0x763813u, "023 word %06x", tonef_dcs_word(0023));
	for (unsigned c = 0; c < 512; c++) {
		uint32_t w = tonef_dcs_word((uint16_t)c);
		CHECK(tonef_dcs_code(w) == (int)c, "code %03o", c);
		for (int b = 0; b < 23; b++)
			CHECK(tonef_dcs_code(w ^ (1u << b)) < 0, "code %03o with bit %d wrong still valid", c, b);
	}
	// the equivalents among the standard codes: each stream's preferred name
	int alias = 0;
	for (unsigned i = 0; i < TONES_DCS_COUNT; i++) {
		for (int inv = 0; inv < 2; inv++) {
			uint32_t w = sent_word(tones_dcs[i], inv);
			uint16_t best = 0xFFFF;
			for (int k = 0; k < 23; k++) {
				uint16_t key = tonef_dcs_key(w);
				if (key < best)
					best = key;
				w = ((w >> 1) | (w << 22)) & 0x7FFFFFu;
			}
			CHECK(best != 0xFFFF && !(best & 0x400), "D%03o%c: no standard name", tones_dcs[i], inv ? 'I' : 'N');
			if ((best & 0x1FF) != tones_dcs[i] || ((best & 0x200) != 0) != inv)
				alias++;
		}
	}
	printf("DCS words: 512 codes round trip, single bit errors refused; %d of the 208 standard N / I codes are named "
	       "by an equivalent\n", alias);
}

int main(void)
{
	words();
	// every tone: clean, realistic (voice + noise), off frequency both ways, weak and strong, through an inverted sign
	ctcss_all("clean 0.5 kHz", 0.5, 0, 0.02, 0, 1, 1.6);
	ctcss_all("0.5 kHz, voice 2.5 kHz, noise 0.3 kHz rms", 0.5, 2.5, 0.3, 0, 1, 2.1);
	ctcss_all("0.35 kHz, voice, noise, +0.3 % (encoder error)", 0.35, 2.5, 0.3, 0.003, 1, 3.6);
	ctcss_all("0.35 kHz, voice, noise, -0.3 %", 0.35, 2.5, 0.3, -0.003, 1, 3.6);
	ctcss_all("0.5 kHz, voice, heavy noise 0.8 kHz rms", 0.5, 2.5, 0.8, 0, 1, 2.6);
	ctcss_all("0.5 kHz, voice, noise, level x0.05", 0.5, 2.5, 0.3, 0, 0.05, 2.1);
	ctcss_all("0.75 kHz, voice 4 kHz, noise, level x4 (clipping)", 0.75, 4.0, 0.3, 0, 4, 2.1);
	// neighbours: 0.6 % off (four times a good encoder's error) is up to 40 % of the way to the closest neighbour
	// (196.6 / 199.5 are 1.5 % apart): never named as the neighbour; found where the 0.5 s filter still reaches
	ctcss_all("0.5 kHz, voice, noise, +0.6 % (not the neighbour)", 0.5, 2.5, 0.3, 0.006, 1, 0);
	ctcss_all("0.5 kHz, voice, noise, -0.6 % (not the neighbour)", 0.5, 2.5, 0.3, -0.006, 1, 0);
	// DCS: every code, both polarities
	// (coupling: 20 Hz bends every bit, 5 Hz is closer to what a DC-coupled detector sees)
	dcs_all("normal, voice, noise, 20 Hz coupling", false, 0.3, 2.5, 0, false, 20, 1.5, 104);
	dcs_all("inverted, voice, noise, 20 Hz coupling", true, 0.3, 2.5, 0, false, 20, 1.5, 104);
	dcs_all("normal, voice, noise, 5 Hz coupling", false, 0.3, 2.5, 0, false, 5, 1.5, 104);
	dcs_all("inverted, voice, noise, 5 Hz coupling", true, 0.3, 2.5, 0, false, 5, 1.5, 104);
	dcs_all("normal, +0.5 % bit rate", false, 0.3, 2.5, 0.005, false, 20, 2.0, 104);
	dcs_all("inverted, -0.5 % bit rate", true, 0.3, 2.5, -0.005, false, 20, 2.0, 104);
	dcs_all("normal, receive sign inverted (decoder told)", false, 0.3, 2.5, 0, true, 20, 1.5, 104);
	// heavy noise: what is found must be right; how much is found is printed
	dcs_all("normal, heavy noise 0.6 kHz rms, 20 Hz coupling", false, 0.6, 2.5, 0, false, 20, 3, 0);
	dcs_all("normal, heavy noise 0.6 kHz rms, 5 Hz coupling", false, 0.6, 2.5, 0, false, 5, 3, 90);
	dcs_all("inverted, heavy noise 0.6 kHz rms, 5 Hz coupling", true, 0.6, 2.5, 0, false, 5, 3, 90);
	// nothing from voice or noise alone
	quiet("voice 2.5 kHz + noise 0.3 kHz", 2.5, 0.3, 30);
	quiet("noise 1 kHz rms (no carrier)", 0, 1.0, 30);
	quiet("voice 4 kHz, no noise", 4.0, 0.0, 30);
	// a DCS transmission's 134.4 Hz turn-off tail (alternating bits, a 67.2 Hz square wave) after 2 s of code
	{
		tonef_t d;
		tonef_init(&d, false);
		rx_t r = { .kind = 2, .word = sent_word(0023, false), .baud = 134.4, .sub_khz = 0.5, .voice_khz = 2.5,
			   .noise = 0.3, .level = 1 };
		rx_init(&r);
		run_t o = run(&d, &r, 2.0, TONEF_DCS, 0023);
		CHECK(o.first >= 0, "turn-off: D023N not found");
		r.word = 0x2AAAAAu;	// alternating
		run_t t = run(&d, &r, 0.3, 0xFF, 0);
		r.kind = 0;
		run_t e = run(&d, &r, 3.0, 0xFF, 0);
		bool ctcss = (t.wrong && t.wtype == TONEF_CTCSS) || (e.wrong && e.wtype == TONEF_CTCSS);
		CHECK(!ctcss, "turn-off tail read as CTCSS");
		CHECK(d.type == TONEF_NONE, "turn-off: still showing type %u after 3 s", d.type);
		printf("DCS turn-off tail: not read as CTCSS, released\n");
	}
	// a tone that stops is let go within 1.5 s; another tone after it is found
	{
		tonef_t d;
		tonef_init(&d, false);
		rx_t r = { .kind = 1, .hz = 88.5, .sub_khz = 0.5, .voice_khz = 2.5, .noise = 0.3, .level = 1 };
		rx_init(&r);
		run(&d, &r, 3.0, TONEF_CTCSS, 885);
		CHECK(d.type == TONEF_CTCSS && d.value == 885, "88.5 not held");
		r.kind = 0;
		double gone = -1;
		int16_t buf[200];
		for (int s = 0; s < (int)(3 * FS) && gone < 0; s += 200) {
			for (int k = 0; k < 200; k++)
				buf[k] = rx_sample(&r);
			tonef_audio(&d, buf, 200);
			if (d.type == TONEF_NONE)
				gone = (s + 200) / FS;
		}
		CHECK(gone >= 0 && gone <= 1.6, "88.5 let go after %.2f s", gone);
		r.kind = 1;
		r.hz = 91.5;
		run_t o = run(&d, &r, 3.0, TONEF_CTCSS, 915);
		CHECK(o.first >= 0 && !o.wrong, "91.5 after 88.5: %.2f s, wrong %d", o.first, o.wrong);
		// and the change from one tone straight to another, no gap
		r.hz = 94.8;
		o = run(&d, &r, 3.0, TONEF_CTCSS, 948);
		CHECK(o.first >= 0 && o.first <= 2.1, "94.8 straight after 91.5: %.2f s", o.first);
		printf("CTCSS let go %.2f s after the tone stops; the next tone found\n", gone);
	}
	printf("tone finder: %d failures\n", fails);
	return fails != 0;
}
