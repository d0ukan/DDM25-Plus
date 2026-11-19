/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus SSTV receive (CONFIG_SSTV, docs/plus.md): a slow-scan TV decoder for 8 kHz FM receive audio. Installed
 * as core/radio/sstv.{c,h}. Pure integer DSP, no RTOS, no board: the radio task glue is radio_sstv.c, the screen
 * ui_sstv.c, the host test tests/plus/test_sstv.c. Written for this project from the published mode timings (JL
 * Barber, "Proposal for SSTV mode specifications", Dayton 2000); no code from other decoders.
 *
 * Modes (VIS code): Robot 36 (8) and 72 (12), PD50 (93), PD90 (99), PD120 (95), PD180 (96), PD240 (97), Martin M1 (44)
 * and M2 (40), Scottie S1 (60), S2 (56) and DX (76). Video is 1500 Hz (black) .. 2300 Hz (white), sync 1200 Hz.
 *
 * Demodulator: a DC block, a mix down by 1700 Hz (the middle of 1100..2300; an 80-entry table: 17 cycles in 80
 * samples), a 15-tap low pass on I and Q (900 Hz), and the phase step between samples (z[n] conj z[n-1]: atan of
 * cross / dot from a 33-entry table) as the instantaneous frequency, 1/16 Hz resolution. It does not depend on the
 * audio's level or its de-emphasis.
 *
 * VIS: a 1900 Hz leader of at least 150 ms (a smoothed frequency within 150 Hz of it), then the start bit's edge
 * (1200 Hz, 30 ms); each of the 10 bits (start, 7 data LSB first, even parity, stop) is the mean frequency over the
 * middle 20 ms of its 30 (1100 Hz = 1, 1300 Hz = 0). Without a VIS (tuned in late) an image starts on its line syncs:
 * four 1200 Hz pulses in a row of a mode's sync length, spaced by its line time (0.7 %), with no other sync tone
 * between them.
 *
 * Line timing: each line's sync pulse is found near where the model puts it (a score: the sync tone over the pulse's
 * length followed by none over half of it peaks at the pulse's end; the middle of the highest run of the score), and
 * the model is a least squares line through every sync found (line start = a + k * T): T is the receiving clock's
 * view of the line time, so a sample clock off by a few tenths of a percent gives a straight picture (no slant), and
 * one noisy sync moves nothing much. Ten syncs missed in a row end the image.
 *
 * Pixels: each colour scan is sampled straight to the display's width (SSTV_W pixels, chroma SSTV_W / 2), each pixel
 * the mean frequency over its share of the scan. Only the lines in progress are kept (3 x SSTV_W bytes); a line is
 * handed to the caller when it ends (PD: two picture lines). YCrCb (Robot, PD) to RGB as ITU-R BT.601, full range.
 *
 * RAM: sstv_t, about 1 KB (the caller allocates it). CPU per sample: about 30 multiplies and one divide. */
#ifndef SSTV_H
#define SSTV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SSTV_FS		8000u	/* the audio's sample rate */
#define SSTV_W		240u	/* pixels a line is sampled to (the DM-32's display width) */
#define SSTV_ROWS_MAX	192u	/* display rows of the tallest picture (320 x 256 shown 240 x 192) */
#define SSTV_FIR	15u

enum {
	SSTV_ROBOT36, SSTV_ROBOT72, SSTV_PD50, SSTV_PD90, SSTV_PD120, SSTV_PD180, SSTV_PD240,
	SSTV_MARTIN1, SSTV_MARTIN2, SSTV_SCOTTIE1, SSTV_SCOTTIE2, SSTV_SCOTTIEDX, SSTV_MODES
};
#define SSTV_NONE	0xFFu

/* sstv_t.state */
enum {
	SSTV_IDLE,	/* listening for a VIS or line syncs; nothing received yet */
	SSTV_RX,	/* an image coming */
	SSTV_DONE,	/* the last image ended: all its lines received (listening for the next) */
	SSTV_LOST,	/* the last image ended early: its syncs stopped (listening for the next) */
};

typedef struct sstv sstv_t;
/* A picture line is ready: line 0 .. sstv_mode_lines() - 1. Read its pixels with sstv_rgb(d, sub, x, ...) */
typedef void (*sstv_row_fn)(void *ctx, const sstv_t *d, uint16_t line, uint8_t sub);

struct sstv {
	sstv_row_fn row;
	void *ctx;
	/* demodulator */
	int32_t dc_x1, dc_y1;
	int16_t hi[2 * SSTV_FIR], hq[2 * SSTV_FIR];	/* each sample twice: the taps are one straight run */
	uint8_t hpos, nco;
	int32_t pi, pq;		/* the last filtered sample */
	int32_t f;		/* instantaneous frequency, Hz x 16 */
	int32_t fsm;		/* the same smoothed (2 ms): the VIS leader and edge */
	uint32_t n;		/* samples so far */
	/* VIS */
	uint8_t vis_st, vis_bit, vis_bits;
	uint16_t lead;
	uint32_t vis_t0;	/* the start bit's edge (sample) */
	int32_t vis_acc;
	uint16_t vis_cnt;
	uint8_t vis_unknown;	/* a VIS this decoder has no mode for (0: none) */
	/* sync pulses while idle (start without a VIS) */
	uint16_t run, gap, dirt;
	uint8_t pn;
	uint32_t pend[4];
	uint16_t plen[4], pdirt[4];
	/* sync score: the tone's bits, the last 256 samples */
	uint8_t hist[32];
	uint16_t ls, la, s1, s2;
	/* the image */
	uint8_t state, mode;
	uint8_t images;		/* counts the images started */
	uint16_t lines;		/* transmitted lines of the mode */
	uint16_t line;		/* the line being sampled */
	uint16_t sync_j;	/* the line whose sync is looked for next */
	uint8_t missed, rejected;
	uint32_t base;		/* sample at position 0 of the model */
	int32_t tnom;		/* the mode's line time, samples x 256 */
	int32_t alpha;		/* line 0's start, samples x 256 from base */
	int32_t beta;		/* line time - tnom, samples x 256 x 65536 */
	int32_t fn;		/* syncs in the fit */
	int64_t sj, sjj, sr, sjr;
	/* the sync being looked for */
	int32_t win_lo, win_hi;	/* samples from base: the score's peak expected in between */
	uint16_t best;
	int32_t best_first, best_last;
	/* the sampler */
	int32_t line_t;		/* the line's start, samples x 256 from base */
	int32_t seg_off;	/* the segment's nominal start within the line, samples x 256 */
	int32_t seg_start, seg_len, next;
	uint8_t seg, kind, width_shift, parity;
	uint16_t x;
	uint32_t acc;
	uint16_t cnt;
	uint8_t last;
	uint8_t ch[3][SSTV_W];	/* RGB modes: G, B, R; YCrCb modes: Y, Y2 (PD), Cr | Cb (half width each) */
};

void sstv_init(sstv_t *d, sstv_row_fn row, void *ctx);
void sstv_audio(sstv_t *d, const int16_t *s, size_t n);

/* The picture line's colour at pixel x (0 .. SSTV_W - 1); sub: PD's second line of the pair */
void sstv_rgb(const sstv_t *d, uint8_t sub, uint16_t x, uint8_t rgb[3]);

/* Mode facts: name ("PD120"), picture lines (496), display rows for SSTV_W pixels across (186) */
const char *sstv_mode_name(uint8_t mode);
uint16_t sstv_mode_lines(uint8_t mode);
uint16_t sstv_mode_rows(uint8_t mode);
uint8_t sstv_mode_vis(uint8_t mode);
/* The receiving clock against the sender's, parts per million (the fit's line time), 0 before two syncs */
int32_t sstv_clock_ppm(const sstv_t *d);

/* Display colours: an RGB pixel to one of SSTV_COLOURS (R and B 2 levels, G 3) by a 4 x 4 ordered dither at x, y;
 * index r * 6 + g * 2 + b. sstv_colour gives its 8-bit RGB */
#define SSTV_COLOURS	12u
uint8_t sstv_dither(const uint8_t rgb[3], uint16_t x, uint16_t y);
void sstv_colour(uint8_t i, uint8_t rgb[3]);

#endif
