/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus DTMF / SelCall decoder (CONFIG_SELCALL, docs/plus.md): 5-tone selective calling (ZVEI1, ZVEI2, CCIR, EIA)
 * and DTMF strings from an FM channel's 8 kHz receive audio. Installed as core/radio/selcall.{c,h}. Pure integer DSP,
 * no RTOS, no board: the radio task glue is radio_fmdec.c, the host test tests/plus/test_selcall.c.
 *
 * DTMF: FM caller ID's decoder (fmid.h: Goertzel at the 8 tones on 17 ms blocks, 45 % of the power in the pair, each
 * tone 6 dB over the rest of its group, twist within 8 dB, a digit after two blocks: 34 ms or more) runs in here as
 * its own instance, fed sample by sample so that the block a digit ends in is still here: the digit counts only if
 * the pair holds 60 % of that block's power (talk-off: a vowel with harmonics on a DTMF pair has more besides). The
 * digits make a string that ends after SELCALL_DTMF_END ms without one.
 *
 * SelCall sends one tone at a time, back to back (no gaps): 70 ms each in ZVEI, 100 ms in CCIR, 33 ms in EIA; a
 * digit the same as the tone before is sent as the repeat tone ('E'). The tone sets overlap within a few Hz (ZVEI 3
 * 1270 Hz, CCIR 3 1275 Hz; CCIR 5 and EIA 6 are both 1446 Hz), so a single tone does not say which set it is: the
 * whole sequence, frequencies and lengths, does.
 *  - Tone: a high pass (about 160 Hz) and the positive zero crossings, with hysteresis at a quarter of the recent
 *    peak and interpolated to 1/256 sample. Each period between crossings lands in a 5 ms frame. A frame is "pure"
 *    when its periods' spread is under 1/8 of their mean (plus 0.2 sample): one tone, not voice, noise or a DTMF pair.
 *  - Segments: pure frames in a row whose mean frequency stays within 3 % of the segment's own make one tone (one
 *    frame off it, then back, is still the tone: a 459 Hz frame holds only two periods); its frequency is the mean
 *    over all its periods (to a few Hz), its length the frames' span. A segment of 20 ms or more counts; one within
 *    2 % of the one before and less than 10 ms after it is the same tone.
 *  - Sequence: 4 to SELCALL_SEGS tones, ended by 150 ms without one; tones at its ends 5 % or more from every set's
 *    tones are left out (a whistle or vowel right before or after the call). A gap of more than 20 ms between two
 *    tones, or a tone too short to count (450-2900 Hz) right before or after them, means one was lost to noise: the
 *    sequence is refused rather than shown short. Each set (the one picked, or all of them) maps every tone to its
 *    nearest tone within 2 %; the tones between the first and last must last 0.8-1.3 times the set's length (EIA
 *    0.6-1.6: its 33 ms is measured to the 5 ms frame), the first and last at least the minimum (ZVEI's 70 ms is 0.7
 *    of CCIR's 100 ms, CCIR's 1.43 of ZVEI's: their calls are told apart by length where their tones are within 2 %,
 *    as ZVEI 3 1270 Hz and CCIR 3 1275 Hz are). A digit sent twice without the repeat tone (not standard) is not read.
 *    Of the sets that fit, the one with the smallest frequency and length error wins, a tone read as a letter (A-D,
 *    F: rare in calls) counting as 1 % more. ZVEI1 and ZVEI2 have the same digits and lengths and differ only in A-F
 *    (970 Hz is ZVEI1's C and ZVEI2's repeat, 2600 Hz the other way round): a sequence both read the same is reported
 *    as "ZVEI".
 * Tone tables: the common ones (as Wikipedia's "Selective calling" and multimon-ng list them); 'E' is the repeat
 * tone in each. */
#ifndef SELCALL_H
#define SELCALL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "fmid.h"

#define SELCALL_FS		8000u
#define SELCALL_FRAME		40u	/* 5 ms */
#define SELCALL_SEGS		12u	/* tones kept for one sequence */
#define SELCALL_TEXT		16u	/* characters of a decoded sequence */
#define SELCALL_DTMF_END	1500u	/* ms without a digit ends a DTMF string */

/* selcall_seq_t.set and the set picked (selcall_init's only; SELCALL_ANY: all) */
enum {
	SELCALL_ANY = 0,
	SELCALL_ZVEI1,
	SELCALL_ZVEI2,
	SELCALL_CCIR,
	SELCALL_EIA,
	SELCALL_SETS,		/* the sets above */
	SELCALL_ZVEI = SELCALL_SETS,	/* ZVEI1 or ZVEI2 (the same digits) */
	SELCALL_DTMF,
};

typedef struct {
	uint8_t set;			/* SELCALL_ZVEI1 .. SELCALL_DTMF */
	char text[SELCALL_TEXT + 1];
} selcall_seq_t;

typedef void (*selcall_fn)(void *ctx, const selcall_seq_t *s);

typedef struct {
	uint16_t f4;			/* Hz x 4 */
	uint16_t ms;
	uint32_t end;			/* sample number of its end */
} selcall_seg_t;

typedef struct {
	fmid_t dtmf;			/* FM caller ID's decoders: only its DTMF events are used */
	char digits[SELCALL_TEXT + 1];
	uint32_t digit_at;		/* sample number of the last digit */
	/* tone front end */
	int32_t hp;			/* high pass state, Q4 */
	int32_t prev;			/* last sample after the high pass */
	int32_t env;			/* peak of |x|, Q4 */
	bool armed;			/* below -threshold since the last crossing */
	uint32_t n;			/* samples so far */
	uint32_t last_x;		/* the last crossing, 1/256 sample */
	bool have_x;
	/* this frame's periods */
	uint16_t f_cnt;			/* periods in 1/256 sample (to 255 samples) */
	uint32_t f_sum;
	uint64_t f_sq;
	uint8_t f_pos;
	/* the segment being built */
	uint32_t s_sum;			/* its periods, 1/256 sample */
	uint16_t s_cnt;
	uint16_t s_frames;
	uint32_t s_start, s_end;	/* sample numbers */
	uint32_t o_sum;			/* a frame off the segment's tone, kept aside: its periods */
	uint16_t o_cnt;			/* (0: none) */
	/* the sequence */
	selcall_seg_t seg[SELCALL_SEGS];
	uint8_t nseg;
	bool broken;			/* a tone of this sequence was lost: refused */
	uint32_t short_end;		/* the end of the last tone too short to count (sample number) */
	uint8_t only;			/* SELCALL_ANY or one set */
	selcall_fn fn;
	void *ctx;
	/* counters (host test) */
	int16_t hist[FMID_DTMF_N];	/* the last DTMF block's audio, oldest at hpos */
	uint8_t hpos;
	uint32_t segs_seen, seqs_rejected, dtmf_refused;
} selcall_t;

void selcall_init(selcall_t *d, uint8_t only, selcall_fn fn, void *ctx);
/* The set looked for from now on (SELCALL_ANY: all) */
void selcall_only(selcall_t *d, uint8_t only);
void selcall_audio(selcall_t *d, const int16_t *s, size_t n);
/* A sequence or string being received is ended now (the screen closes, the channel changes) */
void selcall_flush(selcall_t *d);

/* "ZVEI1", "ZVEI2", "CCIR", "EIA", "ZVEI", "DTMF" */
const char *selcall_set_name(uint8_t set);
/* A set's tone for character c ('0'-'9', 'A'-'F'), Hz; 0 if none (host test encoders) */
uint16_t selcall_tone_hz(uint8_t set, char c);
/* A set's tone length, ms */
uint16_t selcall_tone_ms(uint8_t set);

#endif
