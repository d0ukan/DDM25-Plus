/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus SSTV receive: the decoder (sstv.h) */
#include "config.h"
#if CONFIG_SSTV
#include <string.h>
#include "sstv.h"

#define HZ(f)		((int32_t)(f) * 16)	/* frequencies are Hz x 16 */
#define MIX_HZ		1700
#define SYNC_BELOW	HZ(1350)		/* the 1200 Hz sync tone (video is 1500 Hz and up) */
#define Q8_US(us)	((int32_t)(((uint32_t)(us) * 256u + 62u) / 125u))	/* microseconds to samples x 256 */
#define SAMPLES_US(us)	((uint16_t)(((uint32_t)(us) * 8u + 500u) / 1000u))
#define VIS_BIT		240u	/* 30 ms */
#define VIS_EDGE_LAG	11u	/* the smoothed frequency's lag at the start bit's edge (samples) */
#define LEAD_IDLE	1200u	/* 150 ms of leader before a VIS */
#define LEAD_RX		1600u	/* 200 ms while an image comes (no line has that much 1900 Hz) */
#define FIRST_R		120	/* the first sync is looked for within 15 ms of where the VIS puts it */
#define SYNC_R		32	/* later ones within 4 ms of the model, 2 ms more each one missed (up to four) */
#define MISS_END	10u	/* syncs missed in a row that end the image */
#define MISS_DARK	3u	/* a line after this many missed syncs is not shown (the signal went) */
#define FIT_TRUSTED	6	/* syncs in the fit before one far from it is doubted */
#define FIT_FAR		(12 * 256)	/* far: 12 samples */
#define FIT_RESET	4u	/* doubted syncs in a row that start the fit again from them */

/* --- tables --- */

/* 15-tap low pass, 900 Hz at 8 kHz (Hamming), DC gain 32767 */
static const int16_t fir[SSTV_FIR] = { -118, -197, -205, 358, 1931, 4319, 6557, 7477, 6557, 4319, 1931, 358, -205,
				       -197, -118 };

/* cos(2 pi i / 80), Q15: the 1700 Hz mixer steps 17 entries a sample (17 cycles in 80 samples) */
static const int16_t cos80[80] = {
	32767, 32666, 32364, 31862, 31163, 30273, 29196, 27938, 26509, 24916, 23170, 21280, 19260, 17121, 14876, 12539,
	10126, 7649, 5126, 2571, 0, -2571, -5126, -7649, -10126, -12539, -14876, -17121, -19260, -21280, -23170, -24916,
	-26509, -27938, -29196, -30273, -31163, -31862, -32364, -32666, -32767, -32666, -32364, -31862, -31163, -30273,
	-29196, -27938, -26509, -24916, -23170, -21280, -19260, -17121, -14876, -12539, -10126, -7649, -5126, -2571, 0,
	2571, 5126, 7649, 10126, 12539, 14876, 17121, 19260, 21280, 23170, 24916, 26509, 27938, 29196, 30273, 31163,
	31862, 32364, 32666 };

/* atan(i / 32) as a frequency step at 8 kHz, Hz x 16 */
static const int16_t atan33[33] = { 0, 636, 1272, 1904, 2533, 3158, 3776, 4387, 4991, 5585, 6170, 6745, 7309, 7861,
				    8402, 8930, 9445, 9948, 10438, 10915, 11380, 11831, 12270, 12696, 13109, 13511,
				    13900, 14278, 14644, 14999, 15343, 15677, 16000 };

/* --- modes --- */

enum { K_SKIP, K_PARITY, K_C0, K_C1, K_C2, K_CR, K_CB, K_CHROMA };
typedef struct {
	uint8_t kind;
	uint32_t us;	/* 0: the mode's scan time */
} seg_t;

/* sync and porch, Y, the separator (1500 Hz: R-Y follows, 2300 Hz: B-Y), porch, the line's chroma */
static const seg_t robot36[] = { { K_SKIP, 12000 }, { K_C0, 88000 }, { K_PARITY, 4500 }, { K_SKIP, 1500 },
				 { K_CHROMA, 44000 } };
static const seg_t robot72[] = { { K_SKIP, 12000 }, { K_C0, 138000 }, { K_SKIP, 6000 }, { K_CR, 69000 },
				 { K_SKIP, 6000 }, { K_CB, 69000 } };
/* sync 20 ms and porch 2.08 ms, Y (even line), R-Y, B-Y, Y (odd line) */
static const seg_t pd[] = { { K_SKIP, 22080 }, { K_C0, 0 }, { K_CR, 0 }, { K_CB, 0 }, { K_C1, 0 } };
/* sync 4.862 ms and porch, G, separator, B, separator, R, separator */
static const seg_t martin[] = { { K_SKIP, 5434 }, { K_C0, 0 }, { K_SKIP, 572 }, { K_C1, 0 }, { K_SKIP, 572 },
				{ K_C2, 0 }, { K_SKIP, 572 } };
/* separator, G, separator, B, sync 9 ms and porch, R (the sync in the middle of the line) */
static const seg_t scottie[] = { { K_SKIP, 1500 }, { K_C0, 0 }, { K_SKIP, 1500 }, { K_C1, 0 }, { K_SKIP, 10500 },
				 { K_C2, 0 } };

enum { F_ROBOT36, F_ROBOT72, F_PD, F_MARTIN, F_SCOTTIE };
static const struct {
	const seg_t *segs;
	uint8_t nseg, rgb;
	uint16_t sync_us;
} families[] = {
	[F_ROBOT36] = { robot36, 5, 0, 9000 },
	[F_ROBOT72] = { robot72, 6, 0, 9000 },
	[F_PD] = { pd, 5, 0, 20000 },
	[F_MARTIN] = { martin, 7, 1, 4862 },
	[F_SCOTTIE] = { scottie, 6, 1, 9000 },
};

typedef struct {
	char name[11];
	uint8_t vis, family;
	uint16_t lines;		/* transmitted lines (PD: two picture lines each) */
	uint16_t rows;		/* display rows: picture lines x SSTV_W / picture width */
	uint32_t scan_us;
} mode_def_t;

static const mode_def_t modes[SSTV_MODES] = {
	[SSTV_ROBOT36] = { "Robot 36", 8, F_ROBOT36, 240, 180, 0 },
	[SSTV_ROBOT72] = { "Robot 72", 12, F_ROBOT72, 240, 180, 0 },
	[SSTV_PD50] = { "PD50", 93, F_PD, 128, 192, 91520 },
	[SSTV_PD90] = { "PD90", 99, F_PD, 128, 192, 170240 },
	[SSTV_PD120] = { "PD120", 95, F_PD, 248, 186, 121600 },
	[SSTV_PD180] = { "PD180", 96, F_PD, 248, 186, 183040 },
	[SSTV_PD240] = { "PD240", 97, F_PD, 248, 186, 244480 },
	[SSTV_MARTIN1] = { "Martin M1", 44, F_MARTIN, 256, 192, 146432 },
	[SSTV_MARTIN2] = { "Martin M2", 40, F_MARTIN, 256, 192, 73216 },
	[SSTV_SCOTTIE1] = { "Scottie S1", 60, F_SCOTTIE, 256, 192, 138240 },
	[SSTV_SCOTTIE2] = { "Scottie S2", 56, F_SCOTTIE, 256, 192, 88064 },
	[SSTV_SCOTTIEDX] = { "Scottie DX", 76, F_SCOTTIE, 256, 192, 345600 },
};

static uint32_t seg_us(const mode_def_t *m, const seg_t *s)
{
	return s->us != 0 ? s->us : m->scan_us;
}

static int32_t line_q8(const mode_def_t *m)
{
	uint32_t us = 0;
	for (uint8_t i = 0; i < families[m->family].nseg; i++)
		us += seg_us(m, &families[m->family].segs[i]);
	return Q8_US(us);
}

/* the sync's start within the line, samples x 256 (Scottie: after G and B) */
static int32_t sync_at_q8(const mode_def_t *m)
{
	return m->family == F_SCOTTIE ? Q8_US(3000u + 2u * m->scan_us) : 0;
}

const char *sstv_mode_name(uint8_t mode)
{
	return mode < SSTV_MODES ? modes[mode].name : "";
}

uint16_t sstv_mode_lines(uint8_t mode)
{
	return mode < SSTV_MODES ? (uint16_t)(modes[mode].lines * (modes[mode].family == F_PD ? 2u : 1u)) : 0;
}

uint16_t sstv_mode_rows(uint8_t mode)
{
	return mode < SSTV_MODES ? modes[mode].rows : 0;
}

uint8_t sstv_mode_vis(uint8_t mode)
{
	return mode < SSTV_MODES ? modes[mode].vis : 0;
}

/* --- demodulator --- */

static int32_t demod(sstv_t *d, int16_t x)
{
	/* DC block (about 20 Hz) */
	int32_t y = (int32_t)x - d->dc_x1 + d->dc_y1 - d->dc_y1 / 64;
	d->dc_x1 = x;
	if (y > 32767)
		y = 32767;
	else if (y < -32768)
		y = -32768;
	d->dc_y1 = y;
	/* mix down: I = y cos, Q = -y sin (sin is cos a quarter turn, 20 entries, back) */
	uint8_t ph = d->nco;
	int16_t i = (int16_t)((y * cos80[ph]) / 32768);
	int16_t q = (int16_t)(-(y * cos80[ph >= 20u ? ph - 20u : ph + 60u]) / 32768);
	d->nco = (uint8_t)(ph >= 63u ? ph - 63u : ph + 17u);
	uint8_t at = d->hpos;
	d->hi[at] = d->hi[at + SSTV_FIR] = i;
	d->hq[at] = d->hq[at + SSTV_FIR] = q;
	d->hpos = (uint8_t)(at + 1u == SSTV_FIR ? 0 : at + 1u);
	/* low pass: the taps are symmetric, the window hi[hpos .. hpos + 14] the oldest first */
	const int16_t *wi = &d->hi[d->hpos], *wq = &d->hq[d->hpos];
	int32_t si = fir[7] * wi[7], sq = fir[7] * wq[7];
	for (unsigned k = 0; k < 7u; k++) {
		si += fir[k] * (wi[k] + wi[14u - k]);
		sq += fir[k] * (wq[k] + wq[14u - k]);
	}
	si /= 65536;
	sq /= 65536;
	/* phase step: z[n] conj z[n-1] */
	int32_t cross = sq * d->pi - si * d->pq, dot = si * d->pi + sq * d->pq;
	d->pi = si;
	d->pq = sq;
	int32_t df;
	int32_t ac = cross < 0 ? -cross : cross;
	if (dot <= 0) {
		if (cross == 0)
			return d->f;	/* no signal at all: the last reading */
		df = 16000;		/* a quarter turn or more: beyond the band */
	} else if (ac >= dot) {
		df = 16000;
	} else {
		if (dot >= (1 << 19)) {	/* both down to 19 bits, so ac << 12 fits */
			int sh = 13 - __builtin_clz((uint32_t)dot);
			dot >>= sh;
			ac >>= sh;
		}
		int32_t r = (ac << 12) / dot;	/* 0 .. 4095: tan of the step, Q12 */
		int32_t k = r >> 7, fr = r & 127;
		df = atan33[k] + (((atan33[k + 1] - atan33[k]) * fr) >> 7);
	}
	return HZ(MIX_HZ) + (cross < 0 ? -df : df);
}

/* --- sync score --- */

static inline unsigned bit_at(const sstv_t *d, uint32_t n)
{
	return (d->hist[(n >> 3) & 31u] >> (n & 7u)) & 1u;
}

/* the sums from the history, through sample n */
static void score_reset(sstv_t *d, uint32_t n)
{
	d->s1 = d->s2 = 0;
	for (uint32_t i = 0; i < d->la; i++)
		d->s2 = (uint16_t)(d->s2 + bit_at(d, n - i));
	for (uint32_t i = 0; i < d->ls; i++)
		d->s1 = (uint16_t)(d->s1 + bit_at(d, n - d->la - i));
}

/* --- the line model --- */

static int32_t model(const sstv_t *d, int32_t j)
{
	return d->alpha + j * d->tnom + (int32_t)(((int64_t)d->beta * j) / 65536);
}

/* nominal offset within the line to the receiver's (the fit's line time) */
static int32_t scaled(const sstv_t *d, int32_t off)
{
	return off + (int32_t)(((int64_t)off * d->beta) / ((int64_t)d->tnom * 65536));
}

static void fit_add(sstv_t *d, int32_t j, int32_t m)
{
	int64_t r = (int64_t)m - (int64_t)j * d->tnom;
	d->fn++;
	d->sj += j;
	d->sjj += (int64_t)j * j;
	d->sr += r;
	d->sjr += (int64_t)j * r;
	int64_t den = (int64_t)d->fn * d->sjj - d->sj * d->sj;
	if (d->fn >= 2 && den > 0) {
		int64_t beta = (((int64_t)d->fn * d->sjr - d->sj * d->sr) * 65536) / den;
		int64_t lim = (int64_t)d->tnom * 65536 / 125;	/* 0.8 % */
		if (beta > lim)
			beta = lim;
		else if (beta < -lim)
			beta = -lim;
		d->beta = (int32_t)beta;
	}
	d->alpha = (int32_t)((d->sr * 65536 - (int64_t)d->beta * d->sj) / ((int64_t)d->fn * 65536));
}

static void fit_clear(sstv_t *d)
{
	d->fn = 0;
	d->sj = d->sjj = d->sr = d->sjr = 0;
}

/* where the score of line j's sync should peak (samples from base), and the window around it */
static void sync_window(sstv_t *d, int32_t radius)
{
	const mode_def_t *m = &modes[d->mode];
	int32_t start = model(d, d->sync_j) + scaled(d, sync_at_q8(m));
	int32_t peak = start / 256 + d->ls - 1 + d->la;
	d->win_lo = peak - radius;
	d->win_hi = peak + radius;
	d->best = 0;
}

/* --- the sampler --- */

static void seg_begin(sstv_t *d)
{
	const mode_def_t *m = &modes[d->mode];
	const seg_t *s = &families[m->family].segs[d->seg];
	int32_t dur = Q8_US(seg_us(m, s));
	d->seg_start = d->line_t + scaled(d, d->seg_off);
	d->seg_len = d->line_t + scaled(d, d->seg_off + dur) - d->seg_start;
	d->seg_off += dur;
	d->kind = s->kind;
	d->width_shift = s->kind == K_SKIP || s->kind == K_PARITY ? 0 :
			 s->kind == K_C0 || s->kind == K_C1 || s->kind == K_C2 ? 2 : 1;
	d->x = 0;
	d->acc = 0;
	d->cnt = 0;
	uint16_t w = d->width_shift == 0 ? 1u : d->width_shift == 2 ? SSTV_W : SSTV_W / 2u;
	d->next = d->seg_start + d->seg_len / w;
}

static void line_begin(sstv_t *d)
{
	d->line_t = model(d, d->line);
	d->seg = 0;
	d->seg_off = 0;
	seg_begin(d);
}

static void image_end(sstv_t *d, uint8_t state)
{
	d->state = state;
	d->run = d->gap = d->dirt = 0;
	d->pn = 0;
}

static void line_end(sstv_t *d)
{
	const mode_def_t *m = &modes[d->mode];
	if (d->missed < MISS_DARK && d->row != NULL) {
		if (m->family == F_PD) {
			d->row(d->ctx, d, (uint16_t)(2u * d->line), 0);
			d->row(d->ctx, d, (uint16_t)(2u * d->line + 1u), 1);
		} else {
			d->row(d->ctx, d, d->line, 0);
		}
	}
	if (++d->line >= m->lines)
		image_end(d, SSTV_DONE);
	else
		line_begin(d);
}

/* the sample at p (samples x 256 from base) has passed the unit being sampled: finish it, start the next */
static void advance(sstv_t *d)
{
	uint8_t v = d->cnt != 0 ? (uint8_t)((d->acc + d->cnt / 2u) / d->cnt) : d->last;
	d->last = v;
	switch (d->kind) {
	case K_C0: d->ch[0][d->x] = v; break;
	case K_C1: d->ch[1][d->x] = v; break;
	case K_C2:
	case K_CR: d->ch[2][d->x] = v; break;
	case K_CB: d->ch[2][SSTV_W / 2u + d->x] = v; break;
	case K_CHROMA: d->ch[2][(d->parity ? SSTV_W / 2u : 0u) + d->x] = v; break;
	case K_PARITY: d->parity = v >= 128u; break;
	default: break;
	}
	d->x++;
	d->acc = 0;
	d->cnt = 0;
	uint16_t w = d->width_shift == 0 ? 1u : d->width_shift == 2 ? SSTV_W : SSTV_W / 2u;
	if (d->x < w) {
		d->next = d->seg_start + (int32_t)(((int64_t)d->seg_len * (d->x + 1u)) / w);
		return;
	}
	if (++d->seg >= families[modes[d->mode].family].nseg)
		line_end(d);
	else
		seg_begin(d);
}

static void sync_done(sstv_t *d)
{
	const mode_def_t *m = &modes[d->mode];
	uint16_t need = (uint16_t)((d->ls + d->la) * 3u / 4u);
	bool found = false;
	if (d->best >= need) {
		/* the peak run's middle is the pulse's end + la; its start ls - 1 before that */
		int32_t mid = (d->best_first + d->best_last) * 128;
		int32_t start = mid - (int32_t)(d->la + d->ls - 1u) * 256;
		/* sync_at on the receiver's scale, roughly (the fit's own line time) */
		int32_t meas = start - scaled(d, sync_at_q8(m));
		int32_t err = meas - model(d, d->sync_j);
		if (d->fn >= FIT_TRUSTED && (err > FIT_FAR || err < -FIT_FAR)) {
			if (++d->rejected >= FIT_RESET) {
				fit_clear(d);	/* the fit went wrong somewhere: start it again from here */
				d->rejected = 0;
				found = true;
			}
		} else {
			d->rejected = 0;
			found = true;
		}
		if (found)
			fit_add(d, d->sync_j, meas);
	}
	if (found) {
		d->missed = 0;
	} else if (++d->missed >= MISS_END) {
		image_end(d, SSTV_LOST);
		return;
	}
	d->sync_j++;
	uint8_t more = d->missed < 4u ? d->missed : 4u;
	sync_window(d, d->fn == 0 ? FIRST_R : SYNC_R + 16 * (int32_t)more);
}

static void image_start(sstv_t *d, uint8_t mode, uint32_t base, int32_t alpha)
{
	const mode_def_t *m = &modes[mode];
	d->state = SSTV_RX;
	d->mode = mode;
	d->images++;
	d->lines = m->lines;
	d->line = 0;
	d->sync_j = 0;
	d->missed = d->rejected = 0;
	d->base = base;
	d->tnom = line_q8(m);
	d->alpha = alpha;
	d->beta = 0;
	fit_clear(d);
	d->ls = SAMPLES_US(families[m->family].sync_us);
	d->la = (uint16_t)(d->ls / 2u);
	score_reset(d, d->n);
	memset(d->ch, 0, sizeof d->ch);
	if (!families[m->family].rgb)
		memset(d->ch[2], 128, sizeof d->ch[2]);
	d->parity = 0;
	d->last = 0;
	d->vis_unknown = 0;
	d->pn = 0;
	sync_window(d, FIRST_R);
	line_begin(d);
}

/* --- VIS --- */

static uint8_t mode_of_vis(uint8_t code)
{
	for (uint8_t i = 0; i < SSTV_MODES; i++)
		if (modes[i].vis == code)
			return i;
	return SSTV_NONE;
}

static void vis(sstv_t *d)
{
	if (d->vis_st == 0) {
		int32_t f = d->fsm;
		if (f > HZ(1750) && f < HZ(2050)) {
			if (d->lead < 0xFFFFu)
				d->lead++;
		} else if (f < HZ(1550) && d->lead >= (d->state == SSTV_RX ? LEAD_RX : LEAD_IDLE)) {
			d->vis_st = 1;
			d->vis_t0 = d->n - VIS_EDGE_LAG;
			d->vis_bit = 0;
			d->vis_bits = 0;
			d->vis_acc = 0;
			d->vis_cnt = 0;
		} else {
			d->lead = d->lead > 8u ? (uint16_t)(d->lead - 8u) : 0;
		}
		return;
	}
	uint32_t t = d->n - d->vis_t0;
	uint32_t in = t % VIS_BIT;
	if (in >= 40u && in < 200u) {
		d->vis_acc += d->f;
		d->vis_cnt++;
		return;
	}
	if (in != 200u || d->vis_cnt == 0)
		return;
	int32_t mean = d->vis_acc / d->vis_cnt;
	d->vis_acc = 0;
	d->vis_cnt = 0;
	uint8_t bit = d->vis_bit++;
	bool ok, one = mean < HZ(1200);
	if (bit == 0 || bit == 9)
		ok = mean >= HZ(1120) && mean <= HZ(1280);	/* start, stop: 1200 Hz */
	else
		ok = mean >= HZ(1000) && mean <= HZ(1400);
	if (ok && bit >= 1 && bit <= 7 && one)
		d->vis_bits |= (uint8_t)(1u << (bit - 1u));
	if (ok && bit == 8) {
		unsigned ones = one;
		for (uint8_t b = d->vis_bits; b != 0; b >>= 1)
			ones += b & 1u;
		ok = (ones & 1u) == 0;	/* even parity */
	}
	if (ok && bit < 9)
		return;
	d->vis_st = 0;
	d->lead = 0;
	if (!ok)
		return;
	uint8_t mode = mode_of_vis(d->vis_bits);
	if (mode == SSTV_NONE) {
		d->vis_unknown = d->vis_bits != 0 ? d->vis_bits : 0x80u;
		return;
	}
	/* the image starts as the stop bit ends: start, 7 data, parity, stop */
	uint32_t base = d->vis_t0 + 10u * VIS_BIT;
	/* Scottie sends one sync pulse before its first line */
	int32_t alpha = modes[mode].family == F_SCOTTIE ? Q8_US(9000) : 0;
	image_start(d, mode, base, alpha);
}

/* --- start on line syncs (no VIS) --- */

static void chain(sstv_t *d)
{
	for (uint8_t mode = 0; mode < SSTV_MODES; mode++) {
		const mode_def_t *m = &modes[mode];
		int32_t ls = SAMPLES_US(families[m->family].sync_us);
		int32_t tol_len = ls * 3 / 10 > 8 ? ls * 3 / 10 : 8;
		int32_t t = line_q8(m) / 256, tol = t / 150 + 8;
		bool ok = true;
		for (unsigned i = 0; i < 4u && ok; i++) {
			int32_t dl = (int32_t)d->plen[i] - ls;
			ok = dl <= tol_len && dl >= -tol_len;
			if (ok && i > 0) {
				int32_t gap = (int32_t)(d->pend[i] - d->pend[i - 1]) - t;
				ok = gap <= tol && gap >= -tol && d->pdirt[i] <= t / 16;
			}
		}
		if (!ok)
			continue;
		/* line 0 is the one after the last pulse: the four go into the fit as lines -4 .. -1 */
		uint32_t base = d->pend[3] + 1u;
		uint32_t pend[4];
		memcpy(pend, d->pend, sizeof pend);
		image_start(d, mode, base, 0);
		int32_t at = sync_at_q8(m);
		for (int32_t i = 0; i < 4; i++) {
			int32_t e = (int32_t)(pend[i] - base);
			fit_add(d, i - 4, (e - ls + 1) * 256 - at);
		}
		line_begin(d);
		sync_window(d, SYNC_R);
		return;
	}
}

static void pulse_end(sstv_t *d, uint32_t end, uint16_t len)
{
	if (len < 24u || len > 200u) {
		d->dirt = (uint16_t)(d->dirt + len < 60000u ? d->dirt + len : 60000u);
		return;
	}
	for (unsigned i = 0; i < 3u; i++) {
		d->pend[i] = d->pend[i + 1];
		d->plen[i] = d->plen[i + 1];
		d->pdirt[i] = d->pdirt[i + 1];
	}
	d->pend[3] = end;
	d->plen[3] = len;
	d->pdirt[3] = d->dirt;
	d->dirt = 0;
	if (d->pn < 4u)
		d->pn++;
	if (d->pn == 4u)
		chain(d);
}

static void pulses(sstv_t *d, bool tone)
{
	if (tone) {
		d->run = (uint16_t)(d->run + 1u + d->gap < 60000u ? d->run + 1u + d->gap : 60000u);
		d->gap = 0;
	} else if (d->run != 0 && ++d->gap > 2u) {
		pulse_end(d, d->n - d->gap, d->run);
		d->run = 0;
		d->gap = 0;
	}
}

/* --- per sample --- */

static void sample(sstv_t *d, int16_t x)
{
	int32_t f = demod(d, x);
	d->f = f;
	d->fsm += (f - d->fsm) / 16;
	uint32_t n = d->n;
	bool tone = f < SYNC_BELOW;
	uint8_t *h = &d->hist[(n >> 3) & 31u];
	*h = (uint8_t)(tone ? *h | (1u << (n & 7u)) : *h & ~(1u << (n & 7u)));
	if (d->state == SSTV_RX) {
		unsigned out2 = bit_at(d, n - d->la);
		d->s2 = (uint16_t)(d->s2 + tone - out2);
		d->s1 = (uint16_t)(d->s1 + out2 - bit_at(d, n - d->la - d->ls));
	}
	vis(d);
	if (d->state != SSTV_RX) {
		pulses(d, tone);
		d->n++;
		return;
	}
	int32_t rel = (int32_t)(n - d->base);
	/* the sync being looked for */
	if (d->sync_j < d->lines && rel >= d->win_lo) {
		uint16_t score = (uint16_t)(d->s1 + d->la - d->s2);
		if (score > d->best) {
			d->best = score;
			d->best_first = d->best_last = rel;
		} else if (score == d->best) {
			d->best_last = rel;
		}
		if (rel >= d->win_hi)
			sync_done(d);
	}
	/* the picture */
	if (d->state == SSTV_RX && rel >= -(1 << 22)) {
		int32_t p = rel * 256;
		while (d->state == SSTV_RX && p >= d->next)
			advance(d);
		if (d->state == SSTV_RX && d->kind != K_SKIP && p >= d->seg_start) {
			int32_t lv = (f - HZ(1500)) * 1306;	/* 1500 .. 2300 Hz: 0 .. 255 x 65536 */
			lv = lv <= 0 ? 0 : (lv + 32768) / 65536;
			d->acc += (uint32_t)(lv > 255 ? 255 : lv);
			d->cnt++;
		}
	}
	d->n++;
}

void sstv_init(sstv_t *d, sstv_row_fn row, void *ctx)
{
	memset(d, 0, sizeof *d);
	d->row = row;
	d->ctx = ctx;
	d->f = d->fsm = HZ(MIX_HZ);
	d->mode = SSTV_NONE;
}

void sstv_audio(sstv_t *d, const int16_t *s, size_t n)
{
	for (size_t i = 0; i < n; i++)
		sample(d, s[i]);
}

int32_t sstv_clock_ppm(const sstv_t *d)
{
	if (d->fn < 2 || d->tnom == 0)
		return 0;
	return (int32_t)(((int64_t)d->beta * 1000000) / ((int64_t)d->tnom * 65536));
}

/* --- colour --- */

static uint8_t clamp8(int32_t v)
{
	return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

void sstv_rgb(const sstv_t *d, uint8_t sub, uint16_t x, uint8_t rgb[3])
{
	if (x >= SSTV_W)
		x = SSTV_W - 1u;
	if (d->mode < SSTV_MODES && families[modes[d->mode].family].rgb) {
		rgb[0] = d->ch[2][x];
		rgb[1] = d->ch[0][x];
		rgb[2] = d->ch[1][x];
		return;
	}
	int32_t y = sub ? d->ch[1][x] : d->ch[0][x];
	int32_t cr = (int32_t)d->ch[2][x / 2u] - 128, cb = (int32_t)d->ch[2][SSTV_W / 2u + x / 2u] - 128;
	/* BT.601 full range: R = Y + 1.402 Cr, G = Y - 0.344 Cb - 0.714 Cr, B = Y + 1.772 Cb */
	rgb[0] = clamp8(y + (359 * cr) / 256);
	rgb[1] = clamp8(y - (88 * cb + 183 * cr) / 256);
	rgb[2] = clamp8(y + (454 * cb) / 256);
}

static const uint8_t bayer[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };

uint8_t sstv_dither(const uint8_t rgb[3], uint16_t x, uint16_t y)
{
	uint32_t t = bayer[(y & 3u) * 4u + (x & 3u)] * 255u + 128u;
	uint32_t r = (rgb[0] * 16u + t) / 4080u, g = (rgb[1] * 32u + t) / 4080u, b = (rgb[2] * 16u + t) / 4080u;
	return (uint8_t)(r * 6u + g * 2u + b);
}

void sstv_colour(uint8_t i, uint8_t rgb[3])
{
	static const uint8_t g3[3] = { 0, 128, 255 };
	i = i < SSTV_COLOURS ? i : 0;
	rgb[0] = i / 6u ? 255 : 0;
	rgb[1] = g3[(i / 2u) % 3u];
	rgb[2] = i % 2u ? 255 : 0;
}
#endif
