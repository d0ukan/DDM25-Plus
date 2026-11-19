/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus tone finder (CONFIG_TONE_FIND, docs/plus.md): which CTCSS tone or DCS code a received FM signal carries,
 * from its 8 kHz receive audio. Installed as core/radio/tonefind.{c,h}. Pure integer DSP (the CK803S has no FPU), no
 * RTOS, no board (the standard lists are radio/tones.c's): the radio task glue is radio_fmdec.c, the host test
 * tests/plus/test_tonefind.c.
 *
 * The audio must be the flat receive path's (radio_msg.c turns it on for this screen, board_nxdn_flat): the FD6818's
 * normal FM path has a 300 Hz high pass whose job is to take the sub-audible tone out of the speaker audio.
 *
 * Front end: an 80-tap low pass (Blackman, -6 dB at 350 Hz; -1.5 dB at 254 Hz, -60 dB from 600 Hz, -69 dB at 750 Hz)
 * computed only for every 8th sample, so the tones are looked for at 1 kHz with everything that would fold onto them
 * 70 dB down. Then a slow DC tracker (under 1 Hz).
 *
 * CTCSS: a Goertzel filter on each of the 50 standard tones over 0.5 s blocks (500 samples). A block names a tone when
 * that tone holds at least TONEF_RATIO_MIN of the block's AC power (a clean tone holds all of it; voice or noise alone
 * never gave over 5 % in the host test) and beats every other tone by 6 dB. Over 0.5 s a filter is down 13.5 dB or
 * more 2.3-3 Hz away (the closest standard tones: 67.0 / 69.3, 196.6 / 199.5), so a neighbour never wins. Two blocks
 * in a row with the same tone lock it (1.0-1.5 s); two blocks without it let it go; another tone named twice takes
 * over.
 *
 * DCS (134.4 bit/s NRZ, a 23-bit Golay(23,12) word sent over and over, bit 0 first, 1 = positive deviation): a bit
 * clock pulled by the edges, and each bit read two ways: an edge slicer (the difference over 2 ms against 0.56 of its
 * recent peak flips the level, with 5 ms before the next edge, so the receive path's coupling capacitors bending a run
 * of equal bits back toward zero do not matter) and the sign of the audio summed over the bit (better in noise when
 * that bend is slight). Each reading goes into its bit's slot of the 23-bit period, a leaky sum that one or two wrong
 * readings don't flip. At the end of each period the slots' signs are one rotation of the word: all 23 rotations are
 * tried, as they are and inverted; a valid word (marker 100, Golay check bits right) names a code, normal or inverted.
 * The edge slicer's word goes first; the summed one counts only when no bit of the period summed to under a quarter of
 * the strongest (a sagged run could make another valid word). Some rotations of one code are another code's word, and
 * every inverted standard code is a normal standard one turned (023N is 047I: the well-known equivalent pairs, the
 * same signal on the air), so the name kept is a standard code before a non-standard one, normal before inverted, then
 * the lowest code. The same name in two periods in a row locks it (about 0.5 s); three periods without it let it go.
 * A DCS word in the last 2 s keeps CTCSS out (the code's own spectral lines and its 134.4 Hz turn-off tail would
 * otherwise read as a tone near 67 Hz).
 *
 * Which way the receive audio's sign runs is not known on every radio: tonef_init's invert flips the DCS polarity
 * (CTCSS doesn't care). */
#ifndef TONEFIND_H
#define TONEFIND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TONEF_FS		8000u	/* the audio's sample rate */
#define TONEF_DEC		8u	/* decimated to 1 kHz */
#define TONEF_TAPS		80u
#define TONEF_N			500u	/* CTCSS block: 0.5 s at 1 kHz */
#define TONEF_CTCSS_N		50u
/* Q8: a named tone holds at least 15 % of the block's AC power (noise or voice alone: 5 % at most) */
#define TONEF_RATIO_MIN		38u
#define TONEF_DCS_HOLD		2000u	/* 1 kHz samples: a DCS match keeps CTCSS out this long */

/* tonef_t.type: the same values as hal_radio.h's tone_type_t */
#define TONEF_NONE		0u
#define TONEF_CTCSS		1u
#define TONEF_DCS		2u
#define TONEF_DCS_INV		3u

typedef struct {
	/* front end */
	int16_t hist[TONEF_TAPS];
	uint8_t hpos, phase;
	int32_t dc;			/* Q8 */
	/* CTCSS */
	int32_t s1[TONEF_CTCSS_N], s2[TONEF_CTCSS_N];
	uint64_t energy;
	uint16_t n;
	uint8_t c_cand, c_agree;	/* the tone the last blocks named (index), how many in a row */
	uint8_t c_miss;			/* blocks in a row without the locked tone */
	/* DCS */
	int16_t dh[2];
	uint8_t dpos;
	uint8_t quiet;			/* ms since the last edge (to 255) */
	bool level;
	int32_t env;			/* peak of the edge detector, Q4 */
	bool mid_level;			/* the edge slicer's level at the middle of this bit */
	uint32_t ph;			/* bit clock, 2^32 = one bit (an edge at 0) */
	int32_t base;			/* slow mean, Q8 */
	int32_t bit_sum;		/* the audio less its mean over this bit */
	int8_t acc[23];			/* each bit slot of the 23-bit period: a leaky sum, its sign the bit (edge slicer) */
	int8_t acc_l[23];		/* the same from the summed level */
	uint32_t sum_min, sum_max;	/* this period's weakest and strongest bit sum */
	uint8_t word_bits;		/* bits into this word period: the slot */
	uint8_t bits_seen;		/* bits since the start, to 255 */
	uint16_t d_best;		/* this period's best candidate key, 0xFFFF none */
	uint16_t d_cand;		/* the key the last periods named */
	uint8_t d_agree, d_miss;
	uint32_t since_dcs;		/* 1 kHz samples since the last DCS match */
	bool invert;
	/* result */
	uint8_t type;			/* TONEF_* */
	uint16_t value;			/* CTCSS: Hz x 10; DCS: the code (as written, octal digits) */
	uint8_t quality;		/* 0-100: how firmly (the tone's share of the power; DCS: words in a row) */
	uint8_t idle;			/* blocks in a row with nothing named (to 255) */
	/* counters (host test, diagnostics) */
	uint32_t t;			/* 1 kHz samples so far */
	uint32_t blocks, dcs_matches;
	uint8_t ratio;			/* the last block's strongest tone's share of the power, Q8 */
} tonef_t;

void tonef_init(tonef_t *d, bool invert);
/* n samples of 8 kHz flat FM receive audio. True when the result (type, value or quality) changed */
bool tonef_audio(tonef_t *d, const int16_t *s, size_t n);

/* Whether a DCS code is on the standard list (radio/tones.h tones_dcs) */
bool tonef_dcs_standard(uint16_t code);
/* The 23-bit word of a 9-bit DCS code (bit 0 sent first): Golay check bits 22:12, marker 100 in 11:9, the code (the
 * same word as drivers/rf/subaudio's subaudio_dcs_word) */
uint32_t tonef_dcs_word(uint16_t code);
/* A 23-bit window read as an aligned word: its code (0-0777) when it is a valid one, else -1 */
int tonef_dcs_code(uint32_t window);
/* The candidate a window names, as tonef_t.d_best keys it (non-standard << 10 | inverted << 9 | code; the lowest is
 * preferred), 0xFFFF for none */
uint16_t tonef_dcs_key(uint32_t window);

#endif
