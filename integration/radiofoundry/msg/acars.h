/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus ACARS receive (CONFIG_MSG_RX, docs/plus.md): an ACARS decoder for 8 kHz air band AM receive audio.
 * Installed as core/radio/acars.{c,h}. Pure integer DSP, no RTOS, no board: the radio task glue is radio_msg.c, the
 * host test tests/plus/test_acars.c. Written for this project from public descriptions of the ARINC 618 air-ground
 * character format (ARINC 618 itself is not public: the layout below is the one given in the Wikipedia article
 * "ACARS" and in the published write-ups of hobby decoders' documentation); no code from other decoders.
 *
 * The format: on an AM voice channel, 2400 bit/s MSK on audio tones of 1200 and 2400 Hz (half a cycle or a full
 * cycle per bit). A pre-key (all ones), bit sync "+*", character sync SYN SYN, SOH, then the block: mode (1 char),
 * address (7: the aircraft registration, right aligned with leading dots), technical acknowledgement (1), label (2),
 * block ID (1), STX, text (up to 220), ETX (last block) or ETB (more follow), the block check sequence (BCS, 2 bytes)
 * and DEL. Every character is 7-bit ASCII with an odd parity bit (bit 7), sent least significant bit first. Downlink
 * blocks (block ID a digit) start their text with a 4-character message sequence number and the 6-character flight
 * ID. The BCS is taken here to be CRC-16 with the CCITT polynomial in its reflected form (0x8408, start 0, no final
 * inversion) over the bytes from mode through ETX / ETB, sent low byte first, so the CRC over those bytes and the BCS
 * is 0. HARDWARE-UNVERIFIED: if a real ACARS block shows "BCS?" (parity clean, BCS different) this assumption is
 * what to check first.
 *
 * Which tone is which bit, and whether the bits are the tones or their changes, is left open: the sync ("*", SYN,
 * SYN, SOH: 32 bits, up to 2 wrong) is looked for in the tone bits, their complement, the tone changes and their
 * complement, and the block is read the way the sync was found.
 *
 * Demodulator (3.3 samples a bit leave little room: a delay-and-multiply discriminator had an eye a third of a bit
 * wide on clean MSK, this one two thirds): a one-pole high pass at 300 Hz (the AM detector's DC), a mixer to baseband
 * around 1800 Hz (a 40-entry cosine table: 1800 Hz is 9/40 of 8 kHz), a [1 1 1] then [1 1] low pass (zeros near the
 * mixing images at 3000 and 3800 Hz), and a differential detector over three samples: Im(z[n-3] conj z[n]) turns
 * +81 degrees for 1200 Hz, -81 for 2400 Hz, whatever the level. The bit clock is a phase accumulator at 2400 Hz
 * strobing (interpolated between samples) at each bit's center; its timing follows a sign-only Gardner detector (the
 * detector half way between two different bits is on the new bit's side: the strobes are late), 1/64 bit per
 * transition while looking for a block, 1/128 inside one, and the first crossing after ten steady bits (the pre-key)
 * sets it outright. A tone bit is the sign of the detector at the strobe. */
#ifndef ACARS_H
#define ACARS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ACARS_FS		8000u	/* the audio's sample rate */
#define ACARS_TEXT_MAX		80u	/* text characters kept, with the terminator */
#define ACARS_TEXT_LIMIT	220u	/* longer is not a block */

typedef struct {
	char mode;
	char reg[8];		/* the address without its leading dots: "N123AB", "TC-JFV" */
	char ack;		/* technical acknowledgement */
	char label[3];		/* "H1", "_d" (DEL shown as d) */
	char block;		/* block ID: a digit for downlinks, a letter for uplinks */
	char msn[5];		/* downlink: message sequence number "M01A", else "" */
	char flight[7];		/* downlink: flight ID "TK0001", else "" */
	bool crc_ok;		/* the BCS matched (else every character's parity did) */
	bool more;		/* ETB: the message goes on in another block */
	bool cut;		/* the text was longer than ACARS_TEXT_MAX - 1 */
	uint8_t n;		/* text characters */
	char text[ACARS_TEXT_MAX];
} acars_msg_t;

typedef void (*acars_fn)(void *ctx, const acars_msg_t *m);

typedef struct {
	/* front end */
	int32_t hp_x, hp_y;
	int32_t i1, i2, q1, q2;	/* the mixer's last two outputs (the first low pass) */
	int32_t ia, qa;		/* the first low pass's last output (the second) */
	int32_t ih[3], qh[3];	/* the last three baseband samples */
	uint8_t lo;		/* the mixer's place in its cosine table */
	int32_t sm;		/* the detector's last output */
	int32_t y_half, y_prev;	/* the detector between the last two bits and at the last bit */
	int32_t adj;		/* the timing correction waiting for the next half bit */
	int32_t level;		/* mean |smoothed|: the edge hysteresis */
	uint32_t ph;		/* bit clock, 2^32 = one bit, wrapping at the bit's center */
	uint8_t quiet;		/* bits since the last zero crossing */
	bool hi;		/* the smoothed discriminator's side: 1200 Hz */
	bool t_last;		/* the last tone bit */
	/* framing */
	uint32_t sr_tone, sr_change;	/* the last 32 tone bits and tone changes */
	int8_t map;		/* how the block is read: bit 1 changes, bit 0 complement; -1 = hunting */
	uint8_t nb;		/* bits of the byte */
	uint8_t byte;
	uint8_t phase;		/* where in the block */
	uint16_t pos;		/* characters of the field / text so far */
	uint16_t crc;
	bool parity_ok;
	acars_msg_t m;
	/* counters (diagnostics, host test) */
	uint32_t bits, syncs, crc_bad, parity_bad, msgs;
	acars_fn fn;
	void *ctx;
} acars_t;

void acars_init(acars_t *a, acars_fn fn, void *ctx);
/* n samples of 8 kHz AM receive audio; blocks come through fn as they end */
void acars_audio(acars_t *a, const int16_t *s, size_t n);

/* The BCS: CRC-16, reflected CCITT polynomial, start 0 (exposed for the host test's encoder) */
uint16_t acars_crc(uint16_t crc, uint8_t byte);

#endif
