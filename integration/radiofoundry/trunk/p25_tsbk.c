/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus P25 TSBK block decoder (p25_tsbk.h), written from the structure of TIA-102.BAAA (data block interleave,
 * 1/2-rate trellis code) and TIA-102.AABB (TSBK). The trellis table is the standard's state transition table with its
 * constellation-to-dibit mapping applied (the same table OP25 and other decoders print). */
#include "config.h"
#if CONFIG_P25_TRUNK
#include <string.h>
#include "p25_tsbk.h"

/* [previous input dibit][input dibit] -> the four bits (two dibits) on air */
static const uint8_t trellis12[4][4] = {
	{ 0x2, 0xC, 0x1, 0xF },
	{ 0xE, 0x0, 0xD, 0x3 },
	{ 0x9, 0x7, 0xA, 0x4 },
	{ 0x5, 0xB, 0x6, 0x8 },
};
static const uint8_t ones4[16] = { 0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4 };

uint8_t p25_tsbk_trellis(unsigned state, unsigned in)
{
	return trellis12[state & 3u][in & 3u];
}

/* The interleave writes the 49 symbols in four rows (13, 12, 12, 12 symbols): symbol s is in row s % 4, column
 * s / 4, and the rows are sent one after the other */
unsigned p25_tsbk_symbol_bit(unsigned s)
{
	static const uint8_t row_bit[4] = { 0, 52, 100, 148 };
	return row_bit[s & 3u] + 4u * (s >> 2);
}

uint16_t p25_tsbk_crc(const uint8_t b[10])
{
	uint16_t crc = 0;
	for (unsigned i = 0; i < 10u; i++) {
		crc ^= (uint16_t)(b[i] << 8);
		for (unsigned k = 0; k < 8u; k++)
			crc = (uint16_t)((crc & 0x8000u) ? ((unsigned)crc << 1) ^ 0x1021u : (unsigned)crc << 1);
	}
	return (uint16_t)(crc ^ 0xFFFFu);
}

/* A frame bit by its logical position: a status symbol follows every 70 bits from the sync on */
static unsigned frame_bit(const uint8_t *frame, unsigned logical)
{
	unsigned p = logical + 2u * (logical / 70u);
	return (frame[p >> 3] >> (7u - (p & 7u))) & 1u;
}

int p25_tsbk_decode(const uint8_t *frame, unsigned block, uint8_t out[P25_TSBK_BYTES], uint8_t *bits)
{
	enum { SYMBOLS = 49, FAR = 0x3FFF };
	unsigned base = 112u + P25_TSBK_BITS * block;
	uint16_t metric[4] = { 0, FAR, FAR, FAR };	/* the encoder starts in state 0 */
	uint8_t from[SYMBOLS];				/* per symbol: each state's best predecessor, 2 bits apiece */
	for (unsigned s = 0; s < SYMBOLS; s++) {
		unsigned at = base + p25_tsbk_symbol_bit(s);
		unsigned rx = frame_bit(frame, at) << 3 | frame_bit(frame, at + 1u) << 2 | frame_bit(frame, at + 2u) << 1 |
			      frame_bit(frame, at + 3u);
		uint16_t next[4];
		uint8_t pred = 0;
		for (unsigned in = 0; in < 4u; in++) {	/* the new state is the input dibit */
			uint16_t best = 0xFFFFu;
			unsigned best_from = 0;
			for (unsigned st = 0; st < 4u; st++) {
				uint16_t m = (uint16_t)(metric[st] + ones4[rx ^ trellis12[st][in]]);
				if (m < best) {
					best = m;
					best_from = st;
				}
			}
			next[in] = best;
			pred = (uint8_t)(pred | best_from << (2u * in));
		}
		from[s] = pred;
		memcpy(metric, next, sizeof metric);
	}
	if (bits)
		*bits = (uint8_t)(metric[0] > 255u ? 255u : metric[0]);
	/* The flush dibit is 0: trace back from state 0. The state after symbol s is data dibit s */
	memset(out, 0, P25_TSBK_BYTES);
	unsigned state = 0;
	for (int s = SYMBOLS - 1; s >= 0; s--) {
		if (s < SYMBOLS - 1)
			out[s >> 2] = (uint8_t)(out[s >> 2] | state << (6u - 2u * ((unsigned)s & 3u)));
		state = (from[s] >> (2u * state)) & 3u;
	}
	return p25_tsbk_crc(out) == (uint16_t)(out[10] << 8 | out[11]);
}
#endif
