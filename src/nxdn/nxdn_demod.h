/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus NXDN48 4FSK demodulator in software, on 8 kHz FM discriminator audio: 2400 symbols/s, +-1050 / +-350 Hz
 * (docs/nxdn.md). Receive only.
 *
 * 8000 / 2400 = 10/3 samples a symbol, so the symbol instants are fractional:
 *   int16 samples [-> inverses of the receive audio path's de-emphasis and high-pass, off unless measured] ->
 *   root raised cosine (0.2) x x/sin(x) matched filter evaluated at interpolated instants (64 phases) ->
 *   Gardner timing on the symbol pairs that cross the offset -> offset (carrier error) and level tracking ->
 *   hard dibits (01 +3, 00 +1, 10 -1, 11 -3)
 *
 * Ported from DM32-nxdn-p25 src/nxdn/nxdn_modem.{c,h} (GPL-3.0-or-later, "lyra XE1POC"), receive half only; the filter
 * structure is MMDVM's NXDN receiver's (RRC 0.2 and ISINC, G4KLX, GPL-2.0-or-later). Integer arithmetic only (the
 * CK803S has no FPU). No allocation, no RTOS, no hardware.
 *
 * RAM: sizeof(nxdn_demod_t) = 216 bytes on the CK803S (the 64-sample ring is 128 of them). Flash: about 5.2 KB, the
 * 64 x 31 filter table 3,968 B of it. CPU: two 31-tap filters a symbol, about 150 k multiply-adds a second. */
#ifndef NXDN_DEMOD_H
#define NXDN_DEMOD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NXDN_FS		8000
#define NXDN_RS		2400

/* Undoing a first-order high-pass of the receive audio path (corner measured on the radio; 0 = off):
 * x[n] = leak x[n-1] + y[n] / a - y[n-1], a = 1 / (1 + 2 pi fc / fs), leak 1 - 2^-leak_shift */
typedef struct {
	int32_t inv_a_q15;	/* 0 = off */
	int32_t x_q4, y_prev;
	uint8_t leak_shift;
} nxdn_hpf_inverse_t;
#define NXDN_RX_LEAK	10u

/* Undoing a receiver de-emphasis of time constant tau (0 = off): x[n] = (y[n] - a y[n-1]) / (1 - a) */
typedef struct {
	int32_t a_q15;		/* 0 = off */
	int32_t inv_q12;	/* 1 / (1 - a), Q12 */
	int32_t prev;
} nxdn_deemph_inverse_t;

typedef struct {
	int16_t ring[64];	/* input samples, indexed by count & 63 */
	uint32_t count;		/* samples in */
	int64_t t_on;		/* next symbol instant, samples in Q16 */
	int32_t prev_on;	/* matched filter output at the last instant */
	int32_t last4[4];	/* ... at the last four (the acquisition's offset) */
	int32_t dc, outer, amp;	/* offset, outer level (3 units), inner/outer threshold (2 units) */
	uint32_t symbols;	/* since the last acquisition */
	int32_t fit;		/* mean distance of the symbols from their decided levels */
	bool onset_reacquire;	/* acquire again while the symbols don't fit (set while the frame layer hunts) */
	uint32_t onsets;	/* acquisitions started that way (diagnostic) */
	nxdn_hpf_inverse_t post;
	nxdn_deemph_inverse_t deemph;
} nxdn_demod_t;

/* hpf_hz / deemph_us: the receive audio path's measured high-pass corner and de-emphasis (0 = flat path) */
void nxdn_demod_init(nxdn_demod_t *d, uint16_t hpf_hz, uint16_t deemph_us);
/* A new transmission may start (retuned, or the frame layer lost sync): fast offset, level and timing acquisition */
void nxdn_demod_reacquire(nxdn_demod_t *d);
/* Samples in; hard dibits out (at most cap; 3 dibits per 10 samples, rounded up, always fit). Returns the count */
size_t nxdn_demod_samples(nxdn_demod_t *d, const int16_t *in, size_t n, uint8_t *dibits, size_t cap);

void nxdn_hpf_inverse_init(nxdn_hpf_inverse_t *h, uint16_t corner_hz, uint8_t leak_shift);
int16_t nxdn_hpf_inverse_step(nxdn_hpf_inverse_t *h, int16_t y);
void nxdn_deemph_inverse_init(nxdn_deemph_inverse_t *e, uint16_t tau_us);
int16_t nxdn_deemph_inverse_step(nxdn_deemph_inverse_t *e, int16_t y);

#ifdef __cplusplus
}
#endif
#endif
