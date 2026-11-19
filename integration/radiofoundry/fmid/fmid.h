/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus FM caller ID decoders (CONFIG_FM_ID, docs/plus.md): MDC1200, FleetSync I and DTMF on 8 kHz FM receive
 * audio. Installed as core/radio/fmid.{c,h}. Pure integer DSP (the CK803S has no FPU), no RTOS, no board: the radio
 * task glue is radio_fmid.c, the host test tests/plus/test_fmid.c.
 *
 * Front end (MDC1200 and FleetSync share it: both are 1200 baud FFSK, 1200 / 1800 Hz): a one-pole high pass, a band
 * pass at 1470 Hz (Q 1: the same gain at both tones), a delay-line discriminator x[n] x[n-4] (at 8 kHz four samples
 * are half a millisecond: cos(2 pi f 0.5 ms) is -0.81 for 1200 Hz and +0.81 for 1800 Hz, symmetric about zero at
 * 1500 Hz, so the decision needs no level and does not care about the audio's sign), a three-sample smoother, and a
 * bit clock (a phase accumulator at 1200 Hz that each zero crossing pulls halfway toward a bit boundary; after ten
 * bits without one, as in MDC's steady leader tone, the next crossing sets it outright). Each bit is the sign of the
 * smoothed discriminator summed over the bit (integrate and dump). Bit "t" is 1 for 1800 Hz. Host test (white noise
 * over 0-4 kHz): 100 % of bursts at 16 dB SNR, about 90-95 % at 12 dB.
 *
 * MDC1200 (Motorola), from public descriptions of the format (as documented for the open MDC decoders, e.g. the
 * GopherTrunk project's notes "mdc1200: decode the XOR-precoded MSK line code", PR #1224, 2026): 1200 Hz means the
 * data bit equals the previous one, 1800 Hz that it changed, so the data is the running XOR of t (its start state is
 * unknown: the sync is also accepted complemented). A leader of 1800 Hz, the 40-bit sync 0x07092A446F, then a 112-bit
 * codeword interleaved 7 x 16 (codeword bit i, LSB first in each byte, is sent at (i % 7) * 16 + i / 7). The
 * codeword: op, arg, unit ID high, unit ID low, CRC low, CRC high, a status byte, then 7 bytes of rate 1/2
 * convolutional parity (parity bit n = d[n] ^ d[n-2] ^ d[n-5] ^ d[n-6] over the 56 data bits, same byte and bit
 * position 7 bytes on). The CRC is CRC-16 (polynomial 0x1021, reflected, start 0, final XOR 0xFFFF) over the first
 * 4 bytes. Op 0x01 is the PTT ID (arg 0x80 at key-up, 0x00 at release), op 0x00 an emergency. The code is
 * self-orthogonal, so each data bit is checked by four syndromes and is flipped when three of them fail (majority
 * logic with feedback); the CRC decides.
 *
 * FleetSync I (Kenwood), after the layout described in the GopherTrunk project's article "Beyond Voice, Part 4: The
 * FleetSync WAV That Lied" (2026; its decoder follows multimon-ng's): a 24-bit alternating preamble, the 16-bit sync
 * 0xA23E (or its complement: which tone is 1 is not assumed), 4 bits, then two 32-bit words sent MSB first. Fleet =
 * word1 bits 15-8 + 99, unit = (word1 bits 7-0 << 4 | word2 bits 31-28) + 999. The low 16 bits of word2 check the 48
 * bits before them: a 15-bit CRC (shift register polynomial 0x6815, start 0, MSB first) with its lowest bit inverted,
 * then a bit that makes the 64-bit block's parity even (checked here against the article's two on-air blocks
 * FE80083053059B6C and FC80083053057E59, fleet 107 unit 1772). FleetSync II (2400 baud, its own FEC) is not decoded.
 *
 * DTMF: Goertzel filters at the 8 tone frequencies on 136-sample blocks (17 ms: wide enough for a tone 1.5 % off).
 * A block holds a digit when the strongest row and column tones carry at least 45 % of the block's energy (10 % each),
 * each beats the rest of its group by 6 dB, and the twist is within 8 dB; a digit is taken when two blocks in a row
 * hold it (tones of 40 ms or more; 20 ms is too short), again only after a block without it. */
#ifndef FMID_H
#define FMID_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FMID_FS		8000u	/* the audio's sample rate */
#define FMID_DTMF_N	136u	/* Goertzel block length (17 ms) */

enum {
	FMID_MDC = 1,		/* MDC1200 PTT ID: unit */
	FMID_MDC_EMERG,		/* MDC1200 emergency: unit */
	FMID_FLEETSYNC,		/* FleetSync I: fleet, unit */
	FMID_DTMF,		/* one DTMF digit: digit */
};

typedef struct {
	uint8_t kind;		/* FMID_* */
	uint8_t op, arg;	/* MDC: the packet's op and arg */
	uint16_t unit;		/* MDC: the unit ID (shown in hex); FleetSync: 1000-4999 */
	uint16_t fleet;		/* FleetSync: 100-349 */
	char digit;		/* DTMF: '0'-'9', '*', '#', 'A'-'D' */
} fmid_event_t;

typedef void (*fmid_fn)(void *ctx, const fmid_event_t *e);

typedef struct {
	/* front end */
	int32_t hp_x, hp_y;
	int32_t bp_x1, bp_x2, bp_y1, bp_y2;
	int16_t dl[4];
	uint8_t dl_pos;
	int32_t d1, d2;		/* the smoother's last two discriminator samples */
	int32_t acc;		/* this bit's sum */
	int32_t level;		/* mean |smoothed|: the edge hysteresis */
	uint32_t ph;		/* bit clock, 2^32 = one bit */
	uint8_t quiet;		/* bits since the last zero crossing */
	bool hi;		/* the smoothed discriminator's side: 1800 Hz */
	/* MDC1200 */
	uint64_t mdc_sr;	/* the last 40 data bits */
	bool mdc_d;		/* the running XOR */
	bool mdc_inv;
	uint8_t mdc_n;		/* codeword bits taken after a sync, 0 = hunting */
	uint8_t mdc_cw[14];
	/* FleetSync */
	uint64_t fs_sr;
	bool fs_inv;
	uint8_t fs_n;
	uint64_t fs_block;
	/* DTMF */
	int32_t g1[8], g2[8];
	uint64_t g_energy;
	uint16_t g_n;
	char dtmf_blk, dtmf_prev;
	bool dtmf_taken;
	/* counters (diagnostics, host test) */
	uint32_t bits, mdc_syncs, mdc_crc_bad, fs_syncs, fs_crc_bad;
	fmid_fn fn;
	void *ctx;
} fmid_t;

void fmid_init(fmid_t *f, fmid_fn fn, void *ctx);
/* n samples of 8 kHz FM receive audio; events come through fn as they are decoded */
void fmid_audio(fmid_t *f, const int16_t *s, size_t n);

/* The checks, exposed for the host test (and its encoders) */
uint16_t fmid_mdc_crc(const uint8_t *p, size_t n);
/* parity byte i of the MDC codeword's 56 data bits (bytes 0-6) */
void fmid_mdc_parity(const uint8_t data[7], uint8_t parity[7]);
/* FleetSync I: the 16 check bits for the first 48 bits of a block (bits 63-16 of b) */
uint16_t fmid_fs_check(uint64_t b);

#endif
