/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus tone finder: CTCSS and DCS from 8 kHz flat FM receive audio (tonefind.h describes the method). Written
 * for this project; the DCS word layout is the one drivers/rf/subaudio uses for the radio's own receiver. */
#include "config.h"
#if CONFIG_TONE_FIND
#include <string.h>
#include "tonefind.h"
#include "radio/tones.h"

/* Low pass, the first half of the symmetric 80 taps (Q15, sum 32768): Blackman-windowed sinc, -6 dB at 350 Hz */
static const int16_t fir[TONEF_TAPS / 2u] = {
	0, 0, 0, -1, -1, 0, 1, 5, 9, 16,
	23, 31, 38, 42, 41, 33, 16, -11, -48, -95,
	-149, -205, -257, -298, -317, -306, -256, -160, -13, 188,
	438, 732, 1059, 1404, 1750, 2078, 2368, 2604, 2771, 2854,
};

/* 2 cos(2 pi f / 1000) in Q14 for the 50 tones of tones_ctcss */
static const int16_t gcoef[TONEF_CTCSS_N] = {
	29907, 29710, 29481, 29252, 29007, 28745, 28463, 28163, 27831, 27500,
	27125, 26821, 26510, 26080, 25612, 25130, 24607, 24055, 23460, 22833,
	22160, 21437, 20680, 19888, 19027, 18128, 17593, 17174, 16592, 16164,
	15551, 15096, 14453, 13971, 13297, 12787, 12082, 11525, 10789, 10224,
	9438, 8845, 8009, 6524, 4984, 4291, 3371, 1688, -62, -844,
};

#define E_MIN		((uint64_t)TONEF_N * 16u)	/* block power floor: an rms of 4 at 1 kHz */
#define DOMINANCE	4u			/* the named tone beats every other by 6 dB */
#define DCS_STEP	577243605u		/* 2^32 x 134.4 / 1000 */
#define DCS_BITS	23u
#define DCS_MASK	0x7FFFFFu
#define DCS_POLY	0xC75u			/* x^11 + x^10 + x^6 + x^5 + x^4 + x^2 + 1 */
#define DCS_MARKER	0x800u			/* word bits 11:9 = 100 */
#define EDGE_FLOOR	8			/* edge threshold floor (1 kHz samples' counts) */
#define EDGE_GAP	5u			/* ms after an edge before the next (a bit is 7.4 ms) */
#define NO_KEY		0xFFFFu

void tonef_init(tonef_t *d, bool invert)
{
	memset(d, 0, sizeof *d);
	d->invert = invert;
	d->c_cand = 0xFF;
	d->d_best = NO_KEY;
	d->d_cand = NO_KEY;
	d->since_dcs = TONEF_DCS_HOLD;
	d->sum_min = 0xFFFFFFFFu;
}

/* --- DCS words --- */

uint32_t tonef_dcs_word(uint16_t code)
{
	uint32_t data = DCS_MARKER | (code & 0x1FFu);
	uint32_t rem = data << 11;
	for (int bit = 22; bit >= 11; bit--)
		if (rem & (1u << bit))
			rem ^= DCS_POLY << (bit - 11);
	return (rem << 12) | data;
}

int tonef_dcs_code(uint32_t window)
{
	window &= DCS_MASK;
	if (((window >> 9) & 7u) != 4u)
		return -1;
	uint16_t code = (uint16_t)(window & 0x1FFu);
	return tonef_dcs_word(code) == window ? (int)code : -1;
}

bool tonef_dcs_standard(uint16_t code)
{
	for (unsigned i = 0; i < TONES_DCS_COUNT; i++)
		if (tones_dcs[i] == code)
			return true;
	return false;
}

static uint16_t key_of(int code, bool inv)
{
	return (uint16_t)((tonef_dcs_standard((uint16_t)code) ? 0u : 0x400u) | (inv ? 0x200u : 0u) | (unsigned)code);
}

uint16_t tonef_dcs_key(uint32_t window)
{
	uint16_t best = NO_KEY;
	int c = tonef_dcs_code(window);
	if (c >= 0)
		best = key_of(c, false);
	c = tonef_dcs_code(~window);
	if (c >= 0) {
		uint16_t k = key_of(c, true);
		if (k < best)
			best = k;
	}
	return best;
}

/* --- CTCSS --- */

static int32_t mul_q14(int32_t coef, int32_t s)
{
	int32_t hi = s >> 14, lo = s & 0x3FFF;
	return coef * hi + ((coef * lo) >> 14);
}

static void set_result(tonef_t *d, uint8_t type, uint16_t value, uint8_t quality)
{
	d->type = type;
	d->value = type == TONEF_NONE ? 0 : value;
	d->quality = type == TONEF_NONE ? 0 : quality;
}

static void ctcss_block(tonef_t *d)
{
	uint64_t best = 0, second = 0;
	uint8_t bi = 0;
	for (uint8_t k = 0; k < TONEF_CTCSS_N; k++) {
		int64_t a = d->s1[k], b = d->s2[k];
		int64_t p = a * a + b * b - (((int64_t)gcoef[k] * a) >> 14) * b;
		uint64_t up = p > 0 ? (uint64_t)p : 0u;
		if (up > best) {
			second = best;
			best = up;
			bi = k;
		} else if (up > second) {
			second = up;
		}
		d->s1[k] = d->s2[k] = 0;
	}
	uint64_t e = d->energy;
	d->energy = 0;
	d->n = 0;
	d->blocks++;
	uint32_t r8 = 0;
	if (e >= E_MIN) {
		uint64_t q = best * 512u / ((uint64_t)TONEF_N * e);
		r8 = q > 255u ? 255u : (uint32_t)q;
	}
	d->ratio = (uint8_t)r8;
	bool dcs = d->since_dcs < TONEF_DCS_HOLD || d->type == TONEF_DCS || d->type == TONEF_DCS_INV;
	int named = !dcs && r8 >= TONEF_RATIO_MIN && best >= second * DOMINANCE ? bi : -1;
	if (named >= 0 && named == d->c_cand) {
		if (d->c_agree < 255u)
			d->c_agree++;
	} else {
		d->c_cand = named >= 0 ? (uint8_t)named : 0xFFu;
		d->c_agree = named >= 0 ? 1u : 0u;
	}
	uint8_t q = (uint8_t)(r8 * 100u / 256u);
	if (d->type == TONEF_CTCSS) {
		if (named >= 0 && tones_ctcss[named] == d->value) {
			d->c_miss = 0;
			set_result(d, TONEF_CTCSS, d->value, (uint8_t)((d->quality + q + 1u) / 2u));
		} else if (named >= 0 && d->c_agree >= 2u) {
			d->c_miss = 0;
			set_result(d, TONEF_CTCSS, tones_ctcss[named], q);	/* another tone, twice */
		} else if (dcs || ++d->c_miss >= 2u) {
			d->c_miss = 0;
			set_result(d, TONEF_NONE, 0, 0);
		}
	} else if (!dcs && named >= 0 && d->c_agree >= 2u) {
		d->c_miss = 0;
		set_result(d, TONEF_CTCSS, tones_ctcss[named], q);
	}
	if (d->type == TONEF_NONE && named < 0) {
		if (d->idle < 255u)
			d->idle++;
	} else {
		d->idle = 0;
	}
}

static void ctcss_step(tonef_t *d, int32_t x)
{
	for (uint8_t k = 0; k < TONEF_CTCSS_N; k++) {
		int32_t s = x + mul_q14(gcoef[k], d->s1[k]) - d->s2[k];
		d->s2[k] = d->s1[k];
		d->s1[k] = s;
	}
	d->energy += (uint64_t)((int64_t)x * x);
	if (++d->n >= TONEF_N)
		ctcss_block(d);
}

/* --- DCS --- */

static void dcs_period(tonef_t *d)
{
	uint16_t k = d->d_best;
	d->d_best = NO_KEY;
	if (k != NO_KEY && k == d->d_cand) {
		if (d->d_agree < 255u)
			d->d_agree++;
	} else {
		d->d_cand = k;
		d->d_agree = k != NO_KEY ? 1u : 0u;
	}
	bool on = d->type == TONEF_DCS || d->type == TONEF_DCS_INV;
	uint16_t now = (uint16_t)((d->type == TONEF_DCS_INV ? 0x200u : 0u) | d->value);
	uint8_t q = (uint8_t)(d->d_agree >= 4u ? 100u : d->d_agree * 25u);
	if (on && k != NO_KEY && (k & 0x3FFu) == now) {
		d->d_miss = 0;
		set_result(d, d->type, d->value, q);
	} else if (k != NO_KEY && d->d_agree >= 2u) {
		d->d_miss = 0;
		d->c_miss = 0;
		set_result(d, (k & 0x200u) ? TONEF_DCS_INV : TONEF_DCS, (uint16_t)(k & 0x1FFu), q);	/* over CTCSS too */
	} else if (on && ++d->d_miss >= 3u) {
		d->d_miss = 0;
		set_result(d, TONEF_NONE, 0, 0);
	}
	if (d->type != TONEF_NONE)
		d->idle = 0;
}

/* Each bit goes into its slot of the 23-bit period, a leaky sum (3/4 of the old plus the new bit as +-16): a bit read
 * wrong in one or two periods doesn't flip its slot, a code that changes flips it in three. At the end of each period
 * the slots' signs are one rotation of the word: every rotation is tried */
static void slot_put(int8_t *v, bool bit)
{
	*v = (int8_t)(*v - (*v >> 2) + (bit ? 16 : -16));
}

/* The best candidate among the 23 rotations of the word the slots' signs hold, and its complement */
static uint16_t slots_try(const int8_t *acc)
{
	uint32_t w = 0;
	uint16_t best = NO_KEY;
	for (uint8_t i = 0; i < DCS_BITS; i++)
		if (acc[i] > 0)
			w |= 1u << i;
	for (uint8_t r = 0; r < DCS_BITS; r++) {
		uint16_t k = tonef_dcs_key(w);
		if (k < best)
			best = k;
		w = ((w >> 1) | (w << (DCS_BITS - 1u))) & DCS_MASK;
	}
	return best;
}

/* One bit, read two ways: the edge slicer's level at the middle of the bit (right through any coupling capacitor's
 * sag) and the sign of the audio summed over the bit (better in noise, wrong where a long run sagged). The summed
 * level's word is used only when the edge slicer's isn't valid and no bit of the period summed to under a quarter
 * of the strongest (a sagged run: its bits could make another valid word) */
static void dcs_bit(tonef_t *d, bool edge_bit, int32_t sum)
{
	slot_put(&d->acc[d->word_bits], edge_bit != d->invert);
	slot_put(&d->acc_l[d->word_bits], (sum > 0) != d->invert);
	uint32_t a = (uint32_t)(sum < 0 ? -sum : sum);
	if (a > d->sum_max)
		d->sum_max = a;
	if (a < d->sum_min)
		d->sum_min = a;
	if (d->bits_seen < 255u)
		d->bits_seen++;
	if (++d->word_bits < DCS_BITS)
		return;
	d->word_bits = 0;
	if (d->bits_seen >= DCS_BITS) {
		uint16_t k = slots_try(d->acc);
		if (k == NO_KEY && d->sum_min * 4u >= d->sum_max && d->sum_max != 0)
			k = slots_try(d->acc_l);
		d->d_best = k;
		if (k != NO_KEY) {
			d->since_dcs = 0;
			d->dcs_matches++;
		}
	}
	d->sum_max = 0;
	d->sum_min = 0xFFFFFFFFu;
	dcs_period(d);
}

static void dcs_step(tonef_t *d, int32_t y)
{
	int32_t e = y - d->dh[d->dpos];	/* over 2 ms: an edge's whole rise, a sag or a coupling tail only a part */
	d->dh[d->dpos] = (int16_t)y;
	d->dpos = (uint8_t)((d->dpos + 1u) & 1u);
	int32_t a = (e < 0 ? -e : e) << 4;
	if (a > d->env)
		d->env = a;
	else
		d->env -= d->env >> 9;
	int32_t th = (d->env * 9) >> 8;	/* 0.56 of the recent peak */
	if (th < EDGE_FLOOR)
		th = EDGE_FLOOR;
	if (d->quiet < 255u)
		d->quiet++;
	bool edge = false;
	/* an edge in the other direction only after EDGE_GAP ms: the tail of a coupling capacitor's step response
	 * right after an edge is not one */
	if (!d->level && e > th && d->quiet >= EDGE_GAP) {
		d->level = true;
		edge = true;
	} else if (d->level && e < -th && d->quiet >= EDGE_GAP) {
		d->level = false;
		edge = true;
	}
	if (edge)
		d->quiet = 0;
	if (edge) {
		int32_t err = (int32_t)d->ph;	/* the clock puts an edge at phase 0 (the correction never crosses it) */
		d->ph -= (uint32_t)(err / 4);
	}
	d->base += ((y * 256) - d->base) >> 8;	/* about 0.6 Hz */
	d->bit_sum += y - (d->base >> 8);
	uint32_t prev = d->ph;
	d->ph += DCS_STEP;
	if (prev < 0x80000000u && d->ph >= 0x80000000u)
		d->mid_level = d->level;	/* the middle of the bit */
	if (d->ph < prev) {			/* its end */
		dcs_bit(d, d->mid_level, d->bit_sum);
		d->bit_sum = 0;
	}
	if (d->since_dcs < 0xFFFFFFFFu)
		d->since_dcs++;
}

/* --- front end --- */

static void sample_1k(tonef_t *d, int32_t y)
{
	d->t++;
	d->dc += ((y * 256) - d->dc) >> 8;	/* about 0.6 Hz */
	ctcss_step(d, y - (d->dc >> 8));
	dcs_step(d, y);
}

bool tonef_audio(tonef_t *d, const int16_t *s, size_t n)
{
	uint8_t type = d->type, q = d->quality;
	uint16_t value = d->value;
	for (size_t i = 0; i < n; i++) {
		d->hist[d->hpos] = s[i];
		d->hpos = (uint8_t)(d->hpos + 1u >= TONEF_TAPS ? 0u : d->hpos + 1u);
		if (++d->phase < TONEF_DEC)
			continue;
		d->phase = 0;
		/* hist[hpos] is the oldest sample: tap j pairs with tap TAPS-1-j */
		int32_t acc = 0;
		uint8_t a = d->hpos, b = (uint8_t)(d->hpos == 0 ? TONEF_TAPS - 1u : d->hpos - 1u);
		for (uint8_t j = 0; j < TONEF_TAPS / 2u; j++) {
			acc += fir[j] * ((int32_t)d->hist[a] + d->hist[b]);
			a = (uint8_t)(a + 1u >= TONEF_TAPS ? 0u : a + 1u);
			b = (uint8_t)(b == 0 ? TONEF_TAPS - 1u : b - 1u);
		}
		sample_1k(d, acc >> 15);
	}
	return d->type != type || d->value != value || d->quality != q;
}
#endif
