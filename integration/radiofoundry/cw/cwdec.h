/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus CW decoder (CONFIG_CW_RX, docs/plus.md "CW decoder and practice"): Morse from the 8 kHz receive audio,
 * integer only. Installed as core/radio/cwdec.{c,h}.
 *
 * What it hears: a keyed AUDIO tone (MCW): an FM channel carrying a tone keyed in Morse (a CW ID, a beacon, a
 * practice net) or AM. FM and AM have no BFO, so a plain unmodulated CW carrier (A1A) gives no tone to decode: on FM it
 * only quiets the noise, on AM it is silent. The tone must lie within 400..1000 Hz.
 *
 * Signal chain, per 16 ms block of 128 samples:
 *   1. Tone search: 25 Goertzel bins, 400..1000 Hz 25 Hz apart (the block's resolution is 62.5 Hz, so neighbours
 *      overlap). The strongest bin is a tone when it stands 12 times (10.8 dB) over the mean of the bins at least
 *      100 Hz away; its power shape, averaged over the blocks with a tone, gives the tone to 1-2 Hz by a parabola
 *      through the peak (drift is followed block by block). A block with a tone counts only next to another one on
 *      the same bin or a neighbour (a tone, not a voice's harmonics, holds still); then the gate stays open 3 s. Only
 *      such a pair, or a block on the bin already followed, moves the tone: a lone noise block never does.
 *   2. Envelope: the block mixed down by the tone (a 64-entry sine with interpolation), I and Q through two one-pole
 *      low passes each (2 ms each from 15 WPM up, 4 ms below, 8 ms below 8 WPM), the magnitude once a millisecond.
 *      The keying runs one block behind the search, so the gate and the tone are known before a mark is read.
 *   3. Keying: the mark level (fast up, follows the marks down, sinks towards the space level over 6 dots so a mark
 *      after a fade still keys) and the space level (followed in spaces only, never the start of a mark; the whole
 *      envelope while the gate is closed); key down above half way between them, up below 3/8, but never within 3x
 *      (down) / 2x (up) the space level, which the noise's envelope rarely reaches; a change must hold a quarter dot
 *      (3..15 ms) to count.
 *   4. Timing, 5..40 WPM: the last eight marks split in two by 2-means (when they hold both dots and dashes, else by
 *      the running averages); the dot and dash averages follow (a quarter a mark), their ratio kept within 2..4.5.
 *      The unit is half their difference, so a keyer's weight (longer marks, shorter spaces) cancels; the weight is
 *      the dot minus the unit and is added back to each space. Space under 2 units: inside a character; at 2 the
 *      character is out. A word space is decided when the space ends: 5 units, or between the two kinds of long gap
 *      when the recent ones show both, or half again their mean when they are all 4.5..6.5 units (Farnsworth).
 *      A transmission's start (the gate opening, or after a pause of 14 units) is learned: its marks and spaces are
 *      kept until the marks split and three character gaps came, then decoded with that speed, so its first
 *      characters come out right too (they appear together).
 *   5. ITU-R M.1677-1 table: letters, figures and punctuation; prosigns as <AR> <KN> <SK> <AS> <KA> <VE> <BK> <CL>
 *      <SOS> <HH> (7 dots or more); BT is shown as "="; a code not in the table as "*".
 *
 * CPU: the search's 25 x 128 multiply-adds a block and the mixer's ~35 operations a sample: about 1.5 M operations
 * a second, ~1 % of the 168 MHz core. Memory: cwdec_t (about 640 bytes) */
#ifndef PLUS_CWDEC_H
#define PLUS_CWDEC_H

#include <stdbool.h>
#include <stdint.h>

#define CWDEC_FS		8000u
#define CWDEC_N			128u	/* samples a block: 16 ms */
#define CWDEC_BLOCK_MS		16u
#define CWDEC_BINS		25u
#define CWDEC_TONE_MIN		400u
#define CWDEC_TONE_STEP		25u
#define CWDEC_TONE_MAX		(CWDEC_TONE_MIN + (CWDEC_BINS - 1u) * CWDEC_TONE_STEP)
#define CWDEC_HIST		8u	/* marks (and long spaces) the 2-means looks at */
#define CWDEC_LEARN		40u	/* marks and spaces kept while a transmission's speed is learned */
#define CWDEC_HOLD_MS		3000u	/* the gate stays open this long after the last tone block */
#define CWDEC_WPM_MIN		5u
#define CWDEC_WPM_MAX		40u

/* A character, a prosign ("<AR>"), "*" (not in the table) or " " (a word space) */
typedef void (*cwdec_text_fn)(void *ctx, const char *s);

typedef struct {
	cwdec_text_fn out;
	void *ctx;
	/* the block being filled, then searched and mixed */
	int16_t blk[CWDEC_N];
	uint8_t blk_n;
	/* tone search */
	uint32_t pw[CWDEC_BINS];	/* this block's bin powers */
	uint16_t pavg[CWDEC_BINS];	/* the power shape over the tone blocks, Q16 of the peak */
	uint8_t kh[3];			/* the strongest bin of the last three blocks (0xFF: no tone), [2] newest */
	uint8_t track;			/* the bin followed (0xFF: none yet) */
	uint16_t tone16;		/* the tone followed, Hz x 16 (0: none yet) */
	uint32_t step;			/* the mixer's phase step */
	uint32_t phase;
	/* envelope */
	int32_t li1, lq1, li2, lq2;	/* the low passes, Q4 */
	uint16_t env[CWDEC_BLOCK_MS];	/* the last block's envelope, a sample a millisecond */
	bool env_ok;			/* env holds a block not yet keyed */
	/* keying */
	int32_t peak, floor;		/* mark and space levels, envelope x 16 */
	uint32_t ms;			/* milliseconds keyed so far */
	uint32_t hold_until;		/* ms: the gate open until */
	uint32_t edge;			/* ms of the last key change */
	uint8_t run;			/* ms the level has been on the other side */
	bool key;
	bool gate;			/* the gate was open the last millisecond */
	/* timing */
	uint16_t hist[CWDEC_HIST];	/* the last marks, ms */
	uint8_t hist_n, hist_i;
	uint16_t gaps[CWDEC_HIST];	/* the last spaces of 2 units or more, ms (weight added back) */
	uint8_t gaps_n, gaps_i;
	uint16_t dot16, dash16;		/* average dot and dash, ms x 16 */
	uint16_t code;			/* elements after a leading 1 (1: none), 0xFFFF: more than 12 */
	bool spaced;			/* nothing after the last word space yet */
	/* a transmission's start: marks (bit 15) and spaces kept, until the marks show the speed */
	bool learn;
	uint8_t lb_n;
	uint16_t lb[CWDEC_LEARN];
	uint32_t chars;			/* characters given out */
} cwdec_t;

void cwdec_init(cwdec_t *d, cwdec_text_fn out, void *ctx);
void cwdec_audio(cwdec_t *d, const int16_t *s, uint16_t n);
/* The character being received, given out now (the audio stops) */
void cwdec_flush(cwdec_t *d);

/* The speed found (PARIS: 1200 / unit ms), 0 before the first character */
uint8_t cwdec_wpm(const cwdec_t *d);
/* The tone followed, Hz, 0 while the gate is closed */
uint16_t cwdec_tone(const cwdec_t *d);
/* Key down now (a mark) */
bool cwdec_key(const cwdec_t *d);

/* Elements, MSB first after a leading 1 (0 dot, 1 dash), to text; NULL: not in the table */
const char *cwdec_lookup(uint16_t code);

#endif
