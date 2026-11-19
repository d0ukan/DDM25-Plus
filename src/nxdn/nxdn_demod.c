/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus NXDN48 demodulator (nxdn_demod.h). Ported from DM32-nxdn-p25 src/nxdn/nxdn_modem.c (GPL-3.0-or-later),
 * receive half: path inverses, matched filter, Gardner timing, level tracking and onset re-acquisition unchanged in
 * behaviour; a long-run guard on the sample counter added. */
#include "nxdn_demod.h"
#include <string.h>
#include "nxdn_demod_tables.inc"

void nxdn_hpf_inverse_init(nxdn_hpf_inverse_t *h, uint16_t corner_hz, uint8_t leak_shift)
{
	memset(h, 0, sizeof *h);
	h->leak_shift = leak_shift;
	if (!corner_hz)
		return;
	/* 1/a = 1 + 2 pi fc / fs, Q15: 2 pi / 8000 = 25.736 / 32768 */
	h->inv_a_q15 = 32768 + (int32_t)((uint32_t)corner_hz * 25736u / 1000u);
}

int16_t nxdn_hpf_inverse_step(nxdn_hpf_inverse_t *h, int16_t y)
{
	if (!h->inv_a_q15)
		return y;
	int32_t gain = (int32_t)(((int64_t)y * h->inv_a_q15) >> 15);
	h->x_q4 = h->x_q4 - (h->x_q4 >> h->leak_shift) + (gain - h->y_prev) * 16;
	h->y_prev = y;
	int32_t x = h->x_q4 >> 4;
	return (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
}

void nxdn_deemph_inverse_init(nxdn_deemph_inverse_t *e, uint16_t tau_us)
{
	memset(e, 0, sizeof *e);
	if (!tau_us)
		return;
	/* 2 tau fs = 16 tau_us / 1000: a = (16 tau - 1000) / (16 tau + 1000), 1 / (1 - a) = (16 tau + 1000) / 2000 */
	int32_t k = 16 * (int32_t)tau_us;
	if (k <= 1000)
		return;		/* a corner above 2.5 kHz: nothing to undo in the symbol band */
	e->a_q15 = (int32_t)(((int64_t)(k - 1000) << 15) / (k + 1000));
	e->inv_q12 = (int32_t)(((int64_t)(k + 1000) << 12) / 2000);
}

int16_t nxdn_deemph_inverse_step(nxdn_deemph_inverse_t *e, int16_t y)
{
	if (!e->a_q15)
		return y;
	int32_t d = (int32_t)y - (int32_t)(((int64_t)e->prev * e->a_q15) >> 15);
	e->prev = y;
	int32_t x = (int32_t)(((int64_t)d * e->inv_q12) >> 12);
	return (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
}

#define SPS_Q16 ((int64_t)NXDN_FS * 65536 / NXDN_RS)	/* 10/3 samples */
/* Acquisition: the first symbols after init / reacquire (a preamble, +3 +3 -3 -3 or +3 / -3) set the offset from the
 * mean of the last four symbols and the timing with a fast loop; then the loops slow down (Gardner's self-noise on
 * data jitters a fast loop) */
enum { AMP_MIN = 64, ACQ_SYMBOLS = 32, TIMING_ACQ_SHIFT = 4, TIMING_TRACK_SHIFT = 8, ONSET_GUARD = ACQ_SYMBOLS };

void nxdn_demod_init(nxdn_demod_t *d, uint16_t hpf_hz, uint16_t deemph_us)
{
	memset(d, 0, sizeof *d);
	d->t_on = (int64_t)NXDN_RX_HALF << 16;
	nxdn_hpf_inverse_init(&d->post, hpf_hz, NXDN_RX_LEAK);
	nxdn_deemph_inverse_init(&d->deemph, deemph_us);
}

void nxdn_demod_reacquire(nxdn_demod_t *d)
{
	d->symbols = 0;
}

/* Matched filter at instant t (samples, Q16) */
static int32_t mf(const nxdn_demod_t *d, int64_t t)
{
	int32_t i = (int32_t)(t >> 16);
	unsigned ph = (unsigned)(((t & 0xFFFF) * NXDN_RX_PHASES) >> 16);
	const int16_t *h = nxdn_rx_filter[ph];
	int32_t acc = 0;
	for (int m = -NXDN_RX_HALF; m <= NXDN_RX_HALF; m++)
		acc += (int32_t)d->ring[(uint32_t)(i + m) & 63u] * h[m + NXDN_RX_HALF];
	return acc >> 14;
}

static uint8_t slice(const nxdn_demod_t *d, int32_t y)
{
	int32_t v = y - d->dc, a = d->amp;
	if (v > a)
		return 1;	/* +3 */
	if (v > 0)
		return 0;	/* +1 */
	if (v > -a)
		return 2;	/* -1 */
	return 3;		/* -3 */
}

size_t nxdn_demod_samples(nxdn_demod_t *d, const int16_t *in, size_t n, uint8_t *dibits, size_t cap)
{
	static const int8_t lv[4] = { 1, 3, -1, -3 };
	size_t w = 0;
	for (size_t s = 0; s < n; s++) {
		d->ring[d->count & 63u] = nxdn_hpf_inverse_step(&d->post, nxdn_deemph_inverse_step(&d->deemph, in[s]));
		d->count++;
		if (d->count >= 0x40000000u) {
			/* days of receive: move both clocks back by a multiple of the ring (its indexing is unchanged) */
			d->count -= 0x20000000u;
			d->t_on -= (int64_t)0x20000000 << 16;
		}
		/* every instant whose filter span is now in the ring */
		while (w < cap && (d->t_on >> 16) + NXDN_RX_HALF <= (int64_t)d->count - 1) {
			int32_t on = mf(d, d->t_on);
			int32_t mid = mf(d, d->t_on - SPS_Q16 / 2);
			/* Levels. The fourth symbol seeds the offset and the outer level from the first four; while acquiring
			 * the offset follows the mean of the last four symbols and the outer level the magnitude of the symbols
			 * above half of it; then both are decision directed. The inner/outer threshold sits at 2/3 of it */
			unsigned shift = d->symbols < ACQ_SYMBOLS ? 3u : d->symbols < 400u ? 5u : 6u;
			d->last4[d->symbols & 3u] = on;
			int32_t mean4 = (d->last4[0] + d->last4[1] + d->last4[2] + d->last4[3]) / 4;
			if (d->symbols == 3u) {
				d->dc = mean4;
				int32_t m = 0;
				for (unsigned k = 0; k < 4u; k++)
					m += d->last4[k] > mean4 ? d->last4[k] - mean4 : mean4 - d->last4[k];
				d->outer = m / 4;
			}
			int32_t dev = on - d->dc, mag = dev < 0 ? -dev : dev;
			d->amp = d->outer * 2 / 3;
			uint8_t sym = slice(d, on);
			dibits[w++] = sym;
			int32_t miss = on - (d->dc + lv[sym] * d->outer / 3);
			if (d->symbols >= 4u) {
				if (d->symbols < ACQ_SYMBOLS) {
					d->dc += (mean4 - d->dc) >> shift;
					if (mag * 2 > d->outer)
						d->outer += (mag - d->outer) >> shift;
				} else {
					d->dc += miss >> shift;
					if (sym == 1u || sym == 3u)
						d->outer += (mag - d->outer) >> shift;
				}
			}
			/* How well the symbols sit on their levels: a signal close (a few % of the outer level), noise
			 * spread evenly (a sixth on average). The radio's receive audio has no squelch edge: while the frame
			 * layer hunts and the symbols don't fit (over 1/7), acquire again, so a transmission that starts is met
			 * within 32 symbols, inside its preamble */
			d->fit += ((miss < 0 ? -miss : miss) - d->fit) >> 3;
			if (d->onset_reacquire && d->symbols >= ONSET_GUARD && d->fit * 7 > d->outer) {
				d->symbols = 0;
				d->fit = 0;
				d->onsets++;
			}
			/* Gardner, (y_k - y_k-1) * y_k-1/2 normalized by the level squared, on the pairs that cross the
			 * offset only (between two symbols of one sign the mid sample sits near a peak and cancels the rest) */
			int32_t a = d->amp > AMP_MIN ? d->amp : AMP_MIN;
			int64_t e = (on > d->dc) != (d->prev_on > d->dc) ? (int64_t)(on - d->prev_on) * (mid - d->dc) : 0;
			int64_t norm = e * 65536 / ((int64_t)a * a);
			if (norm > (4 << 16))
				norm = 4 << 16;
			if (norm < -(4 << 16))
				norm = -(4 << 16);
			d->t_on += SPS_Q16 - (norm >> (d->symbols < ACQ_SYMBOLS ? TIMING_ACQ_SHIFT : TIMING_TRACK_SHIFT));
			d->prev_on = on;
			d->symbols++;
		}
	}
	return w;
}
