/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus host tests: a P25 TSDU encoder written from the standard's own formulation, independent of the
 * decoder's (trunk/p25_tsbk.c) tables. TIA-102.BAAA: the 1/2-rate trellis as its state transition table (state =
 * previous input dibit) giving constellation points, each point sent as two C4FM symbols (+3 = dibit 01, +1 = 00,
 * -1 = 10, -3 = 11); the 98 coded dibits sent in the interleave order (pairs 0, 4, 8 ... 48, then 1, 5 ... 45, 2, 6
 * ... 46, 3, 7 ... 47); a status symbol after every 70 bits from the frame sync on. TSBK CRC: CRC-16/GSM (poly
 * 0x1021, init 0, inverted). Used by tests/plus/test_p25trunk.c and test_trunk_worker.c. */
#ifndef TEST_P25_TSDU_BUILD_H
#define TEST_P25_TSDU_BUILD_H
#include <stdint.h>
#include <string.h>
#include "p25.h"

#define TSDU_SYNC UINT64_C(0x5575F5FF77FF)
#define TSDU_MAX_BYTES 90	/* sync, NID and three blocks with their status symbols: 720 bits */

static const uint8_t tsdu_point[4][4] = { { 0, 15, 12, 3 }, { 4, 11, 8, 7 }, { 13, 2, 1, 14 }, { 9, 6, 5, 10 } };
static const int8_t tsdu_symbols[16][2] = {
	{ +1, -1 }, { -1, -1 }, { +3, -3 }, { -3, -3 }, { -3, -1 }, { +3, -1 }, { -1, -3 }, { +1, -3 },
	{ -3, +3 }, { +3, +3 }, { -1, +1 }, { +1, +1 }, { +1, +3 }, { -1, +3 }, { +3, +1 }, { -3, +1 },
};
static unsigned tsdu_dibit_of(int symbol) { return symbol == 3 ? 1u : symbol == 1 ? 0u : symbol == -1 ? 2u : 3u; }

static unsigned tsdu_physical(unsigned logical) { return logical + 2u * (logical / 70u); }
static void tsdu_putb(uint8_t *p, unsigned i, unsigned b)
{
	uint8_t m = (uint8_t)(0x80u >> (i & 7u));
	p[i >> 3] = b ? (uint8_t)(p[i >> 3] | m) : (uint8_t)(p[i >> 3] & ~m);
}
static unsigned tsdu_getb(const uint8_t *p, unsigned i) { return (p[i >> 3] >> (7u - (i & 7u))) & 1u; }

/* Octets 10-11 from octets 0-9 */
static void tsbk_seal(uint8_t b[12])
{
	uint16_t crc = 0;
	for (unsigned i = 0; i < 10u; i++)
		for (unsigned k = 0; k < 8u; k++) {
			unsigned bit = (b[i] >> (7u - k)) & 1u;
			unsigned top = (crc >> 15) & 1u;
			crc = (uint16_t)(crc << 1);
			if (top ^ bit)
				crc ^= 0x1021u;
		}
	crc ^= 0xFFFFu;
	b[10] = (uint8_t)(crc >> 8);
	b[11] = (uint8_t)crc;
}

/* The 196 bits of one TSBK (12 octets) as sent, MSB first in out[0..24] */
static void tsbk_air(const uint8_t b[12], uint8_t out[25])
{
	uint8_t coded[98];
	unsigned state = 0;
	for (unsigned k = 0; k < 49u; k++) {
		unsigned in = k < 48u ? (b[k >> 2] >> (6u - 2u * (k & 3u))) & 3u : 0u;
		unsigned point = tsdu_point[state][in];
		state = in;
		coded[2u * k] = (uint8_t)tsdu_dibit_of(tsdu_symbols[point][0]);
		coded[2u * k + 1u] = (uint8_t)tsdu_dibit_of(tsdu_symbols[point][1]);
	}
	memset(out, 0, 25);
	unsigned sent = 0;
	for (unsigned row = 0; row < 4u; row++)
		for (unsigned pair = row; pair < 49u; pair += 4u)
			for (unsigned half = 0; half < 2u; half++, sent++) {
				unsigned d = coded[2u * pair + half];
				tsdu_putb(out, 2u * sent, d >> 1);
				tsdu_putb(out, 2u * sent + 1u, d & 1u);
			}
}

/* A whole TSDU: sync, NID (nac, DUID 7), n blocks; status symbols 01 ("idle") in place. Returns its length in
 * dibits: the frame up to the end of its last block */
static unsigned tsdu_build(uint8_t frame[TSDU_MAX_BYTES], uint16_t nac, const uint8_t blocks[][12], unsigned n)
{
	memset(frame, 0, TSDU_MAX_BYTES);
	for (unsigned s = 70u; s + 1u < TSDU_MAX_BYTES * 8u; s += 72u)
		tsdu_putb(frame, s + 1u, 1);	/* status symbol 01 */
	for (unsigned i = 0; i < 48u; i++)
		tsdu_putb(frame, tsdu_physical(i), (unsigned)((TSDU_SYNC >> (47u - i)) & 1u));
	uint64_t nid = p25_nid_encode(nac, 7);
	for (unsigned i = 0; i < 64u; i++)
		tsdu_putb(frame, tsdu_physical(48u + i), (unsigned)((nid >> (63u - i)) & 1u));
	for (unsigned k = 0; k < n; k++) {
		uint8_t air[25];
		tsbk_air(blocks[k], air);
		for (unsigned i = 0; i < 196u; i++)
			tsdu_putb(frame, tsdu_physical(112u + 196u * k + i), tsdu_getb(air, i));
	}
	return (tsdu_physical(112u + 196u * n - 1u) + 2u) / 2u;
}

#endif
