/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus host tests: an LDU1 with any 9-octet link control word, written from MMDVMHost's formulation (P25Data.cpp
 * encodeLDU1 / encodeLDUHamming, Hamming.cpp encode1063, P25Utils.cpp encode), independent of src/p25/p25.c's tables:
 * the 9 octets as 12 hexbits, their RS(24,12,13) parity (p25_rs_encode, which the protocol tests check against
 * OP25's vectors), each hexbit Hamming (10,6,3) coded (d6 = d0^d1^d2^d5, d7 = d0^d1^d3^d5, d8 = d0^d2^d3^d4,
 * d9 = d1^d2^d3^d4), four codewords (40 bits) in each of the frame's bits 410-452, 600-640, 788-830, 978-1020,
 * 1168-1208 and 1356-1398 (frame bit 0 = the first sync bit), the status symbols at bits 70 + 72 k and 71 + 72 k
 * skipped. Used by tests/plus/test_p25x.c, test_p25x_worker.c and test_p25lc_ref.cpp (which also checks it against
 * MMDVMHost's own coder). */
#ifndef TEST_P25_LC_BUILD_H
#define TEST_P25_LC_BUILD_H
#include <stdint.h>
#include <string.h>
#include "p25.h"

static inline unsigned lcb_get(const uint8_t *p, unsigned i) { return (p[i >> 3] >> (7u - (i & 7u))) & 1u; }
static inline void lcb_put(uint8_t *p, unsigned i, unsigned b)
{
	uint8_t m = (uint8_t)(0x80u >> (i & 7u));
	p[i >> 3] = b ? (uint8_t)(p[i >> 3] | m) : (uint8_t)(p[i >> 3] & ~m);
}

static const uint16_t lcb_span[6][2] = { { 410, 452 }, { 600, 640 }, { 788, 830 }, { 978, 1020 }, { 1168, 1208 },
					 { 1356, 1398 } };

/* The frame bit where hexbit codeword k (0..23) starts its bit j (0..9) */
static inline unsigned lcb_position(unsigned k, unsigned j)
{
	unsigned g = k / 4u, n = 10u * (k % 4u) + j, ss0 = 70, ss1 = 71;
	while (ss0 < lcb_span[g][0]) {
		ss0 += 72;
		ss1 += 72;
	}
	for (unsigned i = lcb_span[g][0]; i < lcb_span[g][1]; i++) {
		if (i == ss0)
			ss0 += 72;
		else if (i == ss1)
			ss1 += 72;
		else if (n-- == 0)
			return i;
	}
	return 0xFFFF;
}

/* lc[9] into an LDU1 frame of P25_LDU_BYTES (its voice, NID and low speed data are kept) */
static inline void ldu1_set_lc(uint8_t *frame, const uint8_t lc[9])
{
	uint8_t sym[24];
	for (unsigned i = 0; i < 12u; i++) {
		sym[i] = 0;
		for (unsigned b = 0; b < 6u; b++)
			sym[i] = (uint8_t)((sym[i] << 1) | lcb_get(lc, 6u * i + b));
	}
	p25_rs_encode(sym, 12, 12, sym);
	for (unsigned k = 0; k < 24u; k++) {
		unsigned d[10];
		for (unsigned j = 0; j < 6u; j++)
			d[j] = (sym[k] >> (5u - j)) & 1u;
		d[6] = d[0] ^ d[1] ^ d[2] ^ d[5];
		d[7] = d[0] ^ d[1] ^ d[3] ^ d[5];
		d[8] = d[0] ^ d[2] ^ d[3] ^ d[4];
		d[9] = d[1] ^ d[2] ^ d[3] ^ d[4];
		for (unsigned j = 0; j < 10u; j++)
			lcb_put(frame, lcb_position(k, j), d[j]);
	}
}

/* Link control words (TIA-102.AABF; octets as MMDVMHost P25Data.cpp writes them) */
static inline void lc_group(uint8_t lc[9], uint8_t svc, uint16_t tg, uint32_t src)
{
	const uint8_t w[9] = { 0x00, 0x00, svc, 0x00, (uint8_t)(tg >> 8), (uint8_t)tg, (uint8_t)(src >> 16),
			       (uint8_t)(src >> 8), (uint8_t)src };
	memcpy(lc, w, 9);
}
static inline void lc_unit(uint8_t lc[9], uint8_t svc, uint32_t target, uint32_t src)
{
	const uint8_t w[9] = { 0x03, 0x00, svc, (uint8_t)(target >> 16), (uint8_t)(target >> 8), (uint8_t)target,
			       (uint8_t)(src >> 16), (uint8_t)(src >> 8), (uint8_t)src };
	memcpy(lc, w, 9);
}
#endif
