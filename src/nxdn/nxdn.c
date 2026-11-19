/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus NXDN48 frame layer, receive half (nxdn.h, docs/nxdn.md).
 *
 * Ported from DM32-nxdn-p25 src/nxdn/nxdn.c (GPL-3.0-or-later). As that file states, the convolutional code,
 * Viterbi decoder, puncturing, interleaving and CRC generators follow MMDVMHost (Copyright (C) 2018,2023,2025 by
 * Jonathan Naylor G4KLX, GPL-2.0-or-later; NXDNConvolution.cpp, NXDNCRC.cpp, NXDNSACCH.cpp, NXDNFACCH1.cpp,
 * NXDNLICH.cpp), rewritten in C. The AMBE+2 field decode (nxdn_ambe_params, the Golay decoders) is ours, written to
 * the layout in MMDVMHost NXDNAudio.cpp and RadioFoundry ambeFec.h. */
#include "nxdn.h"
#include <string.h>

/* --- LICH --- */
bool nxdn_lich_unpack(uint8_t raw, nxdn_lich_t *l)
{
	l->rfct = (raw >> 6) & 3u;
	l->fct = (raw >> 4) & 3u;
	l->option = (raw >> 2) & 3u;
	l->direction = (raw >> 1) & 1u;
	unsigned p = ((raw >> 7) ^ (raw >> 6) ^ (raw >> 5) ^ (raw >> 4)) & 1u;
	return (raw & 1u) == p;
}

uint8_t nxdn_lich_get(const uint8_t f[NXDN_FRAME_BYTES], unsigned *off)
{
	uint8_t raw = 0;
	unsigned o = 0;
	for (unsigned i = 0; i < 8u; i++) {
		raw = (uint8_t)(raw << 1 | nxdn_bit(f, NXDN_FSW_BITS + 2u * i));
		o += !nxdn_bit(f, NXDN_FSW_BITS + 2u * i + 1u);
	}
	if (off)
		*off = o;
	return raw;
}

/* --- Scrambler: PN9 x^9 + x^4 + 1, seed 0xE4, one bit per symbol on the symbol's sign (the dibit's first bit), from
 * the symbol after the FSW. A constant (MMDVMHost NXDNControl.cpp SCRAMBLER; test_nxdn.c regenerates it from the
 * polynomial), so it costs no RAM --- */
const uint8_t nxdn_scrambler[NXDN_FRAME_BYTES] = {
	0x00, 0x00, 0x00, 0x82, 0xA0, 0x88, 0x8A, 0x00, 0xA2, 0xA8, 0x82, 0x8A, 0x82, 0x02, 0x20, 0x08,
	0x8A, 0x20, 0xAA, 0xA2, 0x82, 0x08, 0x22, 0x8A, 0xAA, 0x08, 0x28, 0x88, 0x28, 0x28, 0x00, 0x0A,
	0x02, 0x82, 0x20, 0x28, 0x82, 0x2A, 0xAA, 0x20, 0x22, 0x80, 0xA8, 0x8A, 0x08, 0xA0, 0xAA, 0x02 };

void nxdn_scramble(uint8_t f[NXDN_FRAME_BYTES])
{
	for (unsigned i = 0; i < NXDN_FRAME_BYTES; i++)
		f[i] ^= nxdn_scrambler[i];
}

/* --- CRCs (initial value all ones, MSB first) --- */
static unsigned crc(const uint8_t *b, unsigned bits, unsigned width, unsigned poly)
{
	unsigned top = 1u << (width - 1u), all = (1u << width) - 1u, c = all;
	for (unsigned i = 0; i < bits; i++) {
		unsigned in = nxdn_bit(b, i), hi = (c & top) != 0;
		c = (c << 1) & all;
		if (in ^ hi)
			c ^= poly;
	}
	return c & all;
}

uint8_t nxdn_crc6(const uint8_t *b, unsigned bits) { return (uint8_t)crc(b, bits, 6, 0x27); }
uint16_t nxdn_crc12(const uint8_t *b, unsigned bits) { return (uint16_t)crc(b, bits, 12, 0x80F); }

/* --- Coded bits read in place: depuncture and deinterleave on the fly, 2 = erased (punctured) --- */
enum { CODE_SACCH, CODE_FACCH1 };

static uint8_t coded(const uint8_t *f, unsigned kind, unsigned base, unsigned i)
{
	unsigned n;
	if (kind == CODE_SACCH) {
		/* 72 coded bits, every sixth (5, 11, ..., 71) punctured, then 12 x 5 interleave */
		if (i % 6u == 5u)
			return 2;
		n = i - i / 6u;
		return (uint8_t)nxdn_bit(f, base + (n % 12u) * 5u + n / 12u);
	}
	/* FACCH1: 192 coded bits, 1, 5, ..., 189 punctured, then 16 x 9 interleave */
	if (i % 4u == 1u)
		return 2;
	n = i - (i + 2u) / 4u;
	return (uint8_t)nxdn_bit(f, base + (n % 16u) * 9u + n / 16u);
}

/* Rate 1/2, K=5 (G1 = 1 + D3 + D4, G2 = 1 + D + D2 + D4), 16-state hard-decision Viterbi. A state is the last four
 * input bits, newest in bit 3. Branch metric: Hamming distance on the two coded bits, erasures free. The encoder
 * flushed with four zeros: the path ends in state 0. Returns the path metric */
static unsigned viterbi(const uint8_t *f, unsigned kind, unsigned base, unsigned n, uint8_t *out, uint16_t *surv)
{
	unsigned metric[16], next[16];
	for (unsigned s = 0; s < 16u; s++)
		metric[s] = s ? 1000u : 0u;
	for (unsigned t = 0; t < n; t++) {
		uint8_t c0 = coded(f, kind, base, 2u * t), c1 = coded(f, kind, base, 2u * t + 1u);
		uint16_t bits = 0;
		for (unsigned s = 0; s < 16u; s++) {
			unsigned d = (s >> 3) & 1u, d1 = (s >> 2) & 1u, d2 = (s >> 1) & 1u, d3 = s & 1u;
			unsigned best = ~0u, choice = 0;
			for (unsigned x = 0; x < 2u; x++) {
				unsigned p = ((s << 1) & 0xEu) | x;	/* predecessor: d1 d2 d3 x */
				unsigned g1 = (d + d3 + x) & 1u, g2 = (d + d1 + d2 + x) & 1u;
				unsigned m = metric[p] + (c0 < 2u && c0 != g1) + (c1 < 2u && c1 != g2);
				if (m < best) {
					best = m;
					choice = x;
				}
			}
			next[s] = best;
			if (choice)
				bits |= (uint16_t)(1u << s);
		}
		surv[t] = bits;
		memcpy(metric, next, sizeof metric);
	}
	unsigned s = 0;
	for (unsigned t = n; t-- > 0;) {
		nxdn_set_bit(out, t, (s >> 3) & 1u);
		s = ((s << 1) & 0xEu) | ((surv[t] >> s) & 1u);
	}
	return metric[0];
}

/* --- SACCH: 26 + CRC6 + 4 tail = 36 -> 72 coded, 12 punctured -> 60 --- */
bool nxdn_sacch_decode(const uint8_t f[NXDN_FRAME_BYTES], nxdn_sacch_t *s, unsigned *corrected, uint16_t *surv)
{
	uint8_t out[5] = { 0 };
	unsigned cost = viterbi(f, CODE_SACCH, NXDN_SACCH_OFFSET, 36u, out, surv);
	if (corrected)
		*corrected = cost;
	unsigned got = 0;
	for (unsigned i = 0; i < 6u; i++)
		got = got << 1 | nxdn_bit(out, 26u + i);
	s->ran = out[0] & 0x3Fu;
	s->structure = out[0] >> 6;
	s->data = 0;
	for (unsigned i = 0; i < 18u; i++)
		s->data = s->data << 1 | nxdn_bit(out, 8u + i);
	return got == nxdn_crc6(out, 26u);
}

/* --- FACCH1: 80 + CRC12 + 4 tail = 96 -> 192 coded, 48 punctured -> 144 --- */
bool nxdn_facch1_decode(const uint8_t f[NXDN_FRAME_BYTES], unsigned which, uint8_t data[NXDN_LAYER3_BYTES],
			unsigned *corrected, uint16_t *surv)
{
	uint8_t out[12] = { 0 };
	unsigned cost = viterbi(f, CODE_FACCH1, NXDN_PAYLOAD_OFFSET + which * NXDN_FACCH1_BITS, 96u, out, surv);
	if (corrected)
		*corrected = cost;
	unsigned got = 0;
	for (unsigned i = 0; i < 12u; i++)
		got = got << 1 | nxdn_bit(out, 80u + i);
	memcpy(data, out, NXDN_LAYER3_BYTES);
	return got == nxdn_crc12(out, 80u);
}

/* --- Layer 3 (TS 1-C VCALL / TX_REL; MMDVMHost NXDNLayer3.cpp field positions) --- */
bool nxdn_layer3_decode(const uint8_t d[NXDN_LAYER3_BYTES], nxdn_call_msg_t *m)
{
	m->type = d[0] & 0x3Fu;
	m->cc_option = d[1];
	m->call_type = d[2] >> 5;
	m->voice_option = d[2] & 0x1Fu;
	m->source = (uint16_t)(d[3] << 8 | d[4]);
	m->destination = (uint16_t)(d[5] << 8 | d[6]);
	m->cipher_type = d[7] >> 6;
	m->key_id = d[7] & 0x3Fu;
	return m->type == NXDN_MSG_VCALL || m->type == NXDN_MSG_TX_REL;
}

/* --- Voice fields --- */
void nxdn_voice_get(const uint8_t f[NXDN_FRAME_BYTES], unsigned k, uint8_t slot[NXDN_SLOT_BYTES])
{
	unsigned base = NXDN_PAYLOAD_OFFSET + k * NXDN_VOICE_BITS;
	memset(slot, 0, NXDN_SLOT_BYTES);
	for (unsigned i = 0; i < NXDN_VOICE_BITS; i++)
		nxdn_set_bit(slot, i, nxdn_bit(f, base + i));
}

/* --- AMBE+2 3600x2450 field: C0 Golay (24,12), C1 Golay (23,12) whitened from C0's data, C2 11 + C3 14 bits plain;
 * the 72-bit sequence S = C0 C1 C2 C3 sits on the air as air bit 4k + j = S(18 j + k) --- */
#define GOLAY_POLY	0xC75u

static uint32_t golay_syndrome(uint32_t cw)
{
	for (int i = 22; i >= 11; i--)
		if (cw & (1u << i))
			cw ^= GOLAY_POLY << (i - 11);
	return cw & 0x7FFu;
}

int nxdn_golay23_decode(uint32_t cw, uint16_t *data)
{
	cw &= 0x7FFFFFu;
	uint32_t s = golay_syndrome(cw), e = 0;
	int n = 0;
	if (s != 0) {
		/* the syndrome of a single error at bit i, then patterns of weight 1, 2, 3 (a perfect code: one of them) */
		uint16_t one[23];
		for (unsigned i = 0; i < 23u; i++)
			one[i] = (uint16_t)golay_syndrome(1u << i);
		for (unsigned i = 0; i < 23u && !n; i++) {
			if (one[i] == s) {
				e = 1u << i;
				n = 1;
			}
		}
		for (unsigned i = 0; i < 23u && !n; i++)
			for (unsigned j = i + 1u; j < 23u && !n; j++)
				if ((one[i] ^ one[j]) == s) {
					e = 1u << i | 1u << j;
					n = 2;
				}
		for (unsigned i = 0; i < 23u && !n; i++)
			for (unsigned j = i + 1u; j < 23u && !n; j++) {
				uint32_t t = s ^ one[i] ^ one[j];
				for (unsigned k = j + 1u; k < 23u; k++)
					if (one[k] == t) {
						e = 1u << i | 1u << j | 1u << k;
						n = 3;
						break;
					}
			}
	}
	*data = (uint16_t)((cw ^ e) >> 11);
	return n;
}

int nxdn_golay24_decode(uint32_t cw, uint16_t *data)
{
	int n = nxdn_golay23_decode(cw >> 1, data);
	/* the corrected 24-bit word must have even weight: if not, the overall parity bit was wrong too */
	uint32_t fixed = ((uint32_t)*data << 11 | golay_syndrome((uint32_t)*data << 11)) << 1 | (cw & 1u);
	unsigned w = 0;
	for (; fixed; fixed &= fixed - 1u)
		w++;
	if (w & 1u)
		n++;
	return n >= 4 ? -1 : n;
}

uint32_t nxdn_ambe_c1_mask(uint16_t c0_data)
{
	uint32_t x = 16u * c0_data, m = 0;
	for (unsigned i = 0; i < 23u; i++) {
		x = (173u * x + 13849u) & 0xFFFFu;
		m = m << 1 | (x >> 15);
	}
	return m;
}

int nxdn_ambe_params(const uint8_t slot[NXDN_SLOT_BYTES], uint8_t params[NXDN_AMBE_BITS])
{
	uint32_t c0 = 0, c1 = 0, c23 = 0;
	for (unsigned n = 0; n < NXDN_VOICE_BITS; n++) {
		unsigned b = nxdn_bit(slot, 4u * (n % 18u) + n / 18u);
		if (n < 24u)
			c0 = c0 << 1 | b;
		else if (n < 47u)
			c1 = c1 << 1 | b;
		else
			c23 = c23 << 1 | b;
	}
	uint16_t a, bd;
	int e0 = nxdn_golay24_decode(c0, &a);
	int e1 = nxdn_golay23_decode(c1 ^ nxdn_ambe_c1_mask(a), &bd);
	for (unsigned i = 0; i < 12u; i++) {
		params[i] = (uint8_t)((a >> (11u - i)) & 1u);
		params[12u + i] = (uint8_t)((bd >> (11u - i)) & 1u);
	}
	for (unsigned i = 0; i < 25u; i++)
		params[24u + i] = (uint8_t)((c23 >> (24u - i)) & 1u);
	return e0 < 0 ? -1 : e0 + e1;
}
