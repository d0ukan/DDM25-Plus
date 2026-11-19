/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus dPMR receive: the channel coding (dpmr.h). Ours, written from ETSI TS 102 490 V1.2.1 clauses 6, 7 and 9
 * and TS 102 658 V2.3.1 clauses 6, 7 and 11; the scrambler's register order follows dsdcc's DSDdPMR (GPL-3.0, F4EXB),
 * whose description of it was read, not its code copied. */
#include "dpmr.h"
#include <string.h>

/* x^9 + x^5 + 1 from all ones, as a 9-stage register whose output o[n] obeys o[n + 9] = o[n] ^ o[n + 4] (the order
 * dsdcc uses on real signals; tests/plus/test_dpmr.c regenerates it). Unverified on our radio: docs/dpmr.md */
const uint8_t dpmr_scrambler[15] = {
	0xFF, 0x83, 0xDF, 0x17, 0x32, 0x09, 0x4E, 0xD1, 0xE7, 0xCD, 0x8A, 0x91, 0xC6, 0xD5, 0xC4
};

/* TS 102 658 V2.3.1 Table 6.1, code numbers 0..63 (each 12-bit code sent as +3 / -3 symbols: 0 -> 01, 1 -> 11) */
const uint32_t dpmr_cc_codes[DPMR_CC_COUNT] = {
	0x575F77, 0x577577, 0x57DD75, 0x57F775, 0x55577D, 0x557D7D, 0x55D57F, 0x55FF7F,
	0x5F555F, 0x5F7F5F, 0x5FD75D, 0x5FFD5D, 0x5D5D55, 0x5D7755, 0x5DDF57, 0x5DF557,
	0x775DD7, 0x7777D7, 0x77DFD5, 0x77F5D5, 0x7555DD, 0x757FDD, 0x75D7DF, 0x75FDDF,
	0x7F57FF, 0x7F7DFF, 0x7FD5FD, 0x7FFFFD, 0x7D5FF5, 0x7D75F5, 0x7DDDF7, 0x7DF7F7,
	0xD755F7, 0xD77FF7, 0xD7D7F5, 0xD7FDF5, 0xD55DFD, 0xD577FD, 0xD5DFFF, 0xD5F5FF,
	0xDF5FDF, 0xDF75DF, 0xDFDDDD, 0xDFF7DD, 0xDD57D5, 0xDD7DD5, 0xDDD5D7, 0xDDFFD7,
	0xF75757, 0xF77D57, 0xF7D555, 0xF7FF55, 0xF55F5D, 0xF5755D, 0xF5DD5F, 0xF5F75F,
	0xFF5D7F, 0xFF777F, 0xFFDF7D, 0xFFF57D, 0xFD5575, 0xFD7F75, 0xFDD777, 0xFDFD77,
};

static uint8_t crc_bits(const uint8_t *buf, unsigned bits, unsigned width, uint8_t poly)
{
	unsigned crc = 0, top = 1u << (width - 1u), mask = (1u << width) - 1u;
	for (unsigned i = 0; i < bits; i++) {
		unsigned fb = ((crc & top) != 0) ^ dpmr_bit(buf, i);
		crc = (crc << 1) & mask;
		if (fb)
			crc ^= poly;
	}
	return (uint8_t)crc;
}

uint8_t dpmr_crc7(const uint8_t *buf, unsigned bits) { return crc_bits(buf, bits, 7, 0x09); }
uint8_t dpmr_crc8(const uint8_t *buf, unsigned bits) { return crc_bits(buf, bits, 8, 0x07); }

/* TS 102 490 Table 7.3: the parity of each data bit, X0 ("1") .. X7 (x^(k + 4) mod x^4 + x + 1) */
static const uint8_t hamming_rows[8] = { 0x3, 0x6, 0xC, 0xB, 0x5, 0xA, 0x7, 0xE };

uint8_t dpmr_hamming_parity(uint8_t data)
{
	uint8_t p = 0;
	for (unsigned k = 0; k < 8u; k++)
		if ((data >> k) & 1u)
			p ^= hamming_rows[k];
	return p;
}

int dpmr_hamming_decode(uint16_t word, uint8_t *data)
{
	uint8_t d = (uint8_t)(word >> 4);
	unsigned s = dpmr_hamming_parity(d) ^ (word & 0xFu);
	*data = d;
	if (s == 0)
		return 0;
	for (unsigned k = 0; k < 8u; k++)
		if (hamming_rows[k] == s) {
			*data = (uint8_t)(d ^ (1u << k));
			return 1;
		}
	return (s & (s - 1u)) == 0 ? 1 : -1;	/* a parity bit itself, or not a single error */
}

int dpmr_cc_match(uint32_t field, unsigned max_errors, unsigned *errors)
{
	int best = -1;
	unsigned best_e = 13;
	for (unsigned k = 0; k < DPMR_CC_COUNT; k++) {
		unsigned e = (unsigned)__builtin_popcount((field ^ dpmr_cc_codes[k]) & DPMR_SYMBOL_INVERT24);
		if (e < best_e) {
			best_e = e;
			best = (int)k;
		}
	}
	if (errors)
		*errors = best_e;
	return best_e <= max_errors ? best : -1;
}

/* Coded word w of a block of C words, descrambled and deinterleaved: coded bit 12 w + b went out as air bit b C + w
 * (written into a 12 x C matrix by columns, read by rows) */
static uint16_t coded_word(const uint8_t *air, unsigned cols, unsigned w)
{
	uint16_t word = 0;
	for (unsigned b = 0; b < 12u; b++) {
		unsigned i = b * cols + w;
		word = (uint16_t)(word << 1 | (dpmr_bit(air, i) ^ dpmr_bit(dpmr_scrambler, i)));
	}
	return word;
}

/* Hamming-decode the C words of a block into C data bytes */
static bool block_decode(const uint8_t *air, unsigned cols, uint8_t *data, unsigned *corrected)
{
	unsigned fixed = 0;
	bool ok = true;
	for (unsigned w = 0; w < cols; w++) {
		int r = dpmr_hamming_decode(coded_word(air, cols, w), &data[w]);
		if (r < 0)
			ok = false;
		else
			fixed += (unsigned)r;
	}
	if (corrected)
		*corrected = fixed;
	return ok;
}

static uint32_t field(const uint8_t *b, unsigned first, unsigned bits)
{
	uint32_t v = 0;
	for (unsigned i = 0; i < bits; i++)
		v = v << 1 | dpmr_bit(b, first + i);
	return v;
}

static void hi_fields(const uint8_t d[10], dpmr_hi_t *h)
{
	h->type = (uint8_t)field(d, 0, 4);
	h->called = field(d, 4, 24);
	h->own = field(d, 28, 24);
	h->mode = (uint8_t)field(d, 52, 3);
	h->format = (uint8_t)field(d, 55, 4);
	h->flags = (uint8_t)field(d, 59, 2);
	h->ci = (uint16_t)field(d, 61, 11);
}

bool dpmr_hi_decode(const uint8_t air[DPMR_HI_BITS / 8u], dpmr_hi_t *h, unsigned *corrected)
{
	uint8_t d[10];
	if (!block_decode(air, 10, d, corrected) || dpmr_crc8(d, 72) != d[9])
		return false;
	hi_fields(d, h);
	return true;
}

/* One word from both copies: the copy that needs no correction, else the one with a single error (the first copy
 * when both have one); when neither decodes, the bits they agree on are kept and the 2^k ways of the k (<= 4) they
 * don't are tried for a code word (the first found). -1 when nothing fits */
static int word_pair(uint16_t c0, uint16_t c1, uint8_t *data)
{
	uint8_t d0, d1;
	int r0 = dpmr_hamming_decode(c0, &d0), r1 = dpmr_hamming_decode(c1, &d1);
	if (r0 == 0 || (r0 > 0 && r1 != 0)) {
		*data = d0;
		return r0;
	}
	if (r1 >= 0) {
		*data = d1;
		return r1;
	}
	uint16_t diff = (uint16_t)(c0 ^ c1);
	if (diff == 0 || __builtin_popcount(diff) > 4)
		return -1;
	for (uint16_t sub = diff;; sub = (uint16_t)((sub - 1u) & diff)) {
		uint16_t c = (uint16_t)(c0 ^ sub);
		if ((dpmr_hamming_parity((uint8_t)(c >> 4)) ^ (c & 0xFu)) == 0) {
			*data = (uint8_t)(c >> 4);
			return 2;
		}
		if (sub == 0)
			break;
	}
	return -1;
}

bool dpmr_hi_decode_pair(const uint8_t *hi0, const uint8_t *hi1, dpmr_hi_t *h)
{
	if (dpmr_hi_decode(hi0, h, NULL) || dpmr_hi_decode(hi1, h, NULL))
		return true;
	uint8_t m[10];
	for (unsigned w = 0; w < 10u; w++)
		if (word_pair(coded_word(hi0, 10, w), coded_word(hi1, 10, w), &m[w]) < 0)
			return false;
	if (dpmr_crc8(m, 72) != m[9])
		return false;
	hi_fields(m, h);
	return true;
}

bool dpmr_cch_decode(const uint8_t air[DPMR_CCH_BITS / 8u], dpmr_cch_t *c, unsigned *corrected)
{
	uint8_t d[6];
	if (!block_decode(air, 6, d, corrected) || dpmr_crc7(d, 41) != (d[5] & 0x7Fu))
		return false;
	c->fn = (uint8_t)field(d, 0, 2);
	c->id = (uint16_t)field(d, 2, 12);
	c->mode = (uint8_t)field(d, 14, 3);
	c->format = (uint8_t)field(d, 17, 4);
	c->flags = (uint8_t)field(d, 21, 2);
	c->sld = field(d, 23, 18);
	return true;
}

bool dpmr_end_decode(const uint8_t *air, unsigned first, dpmr_end_t *e)
{
	uint8_t d[3];
	for (unsigned w = 0; w < 3u; w++) {
		uint16_t word = 0;
		for (unsigned b = 0; b < 12u; b++) {
			unsigned i = 12u * w + b;
			word = (uint16_t)(word << 1 | (dpmr_bit(air, first + i) ^ dpmr_bit(dpmr_scrambler, i)));
		}
		if (dpmr_hamming_decode(word, &d[w]) < 0)
			return false;
	}
	if (dpmr_crc7(d, 17) != (d[2] & 0x7Fu))
		return false;
	e->type = (uint8_t)field(d, 0, 2);
	e->arq = (uint8_t)field(d, 2, 2);
	e->wait = (uint8_t)field(d, 4, 4);
	e->status = (uint8_t)field(d, 8, 5);
	return true;
}

size_t dpmr_id_text(uint32_t id, char *out, size_t cap)
{
	char t[12];
	size_t n = 0;
	id &= 0xFFFFFFu;
	if (id == DPMR_ID_ALL) {
		memcpy(t, "ALL", 3);
		n = 3;
	} else if ((id & 0xFFFFu) == 0xFFFFu) {
		/* dPMR446 initial services: the common ID (1..254) in the top byte */
		unsigned c = id >> 16;
		memcpy(t, "CID ", 4);
		n = 4;
		if (c >= 100u)
			t[n++] = (char)('0' + c / 100u);
		if (c >= 10u)
			t[n++] = (char)('0' + c / 10u % 10u);
		t[n++] = (char)('0' + c % 10u);
	} else if (id <= 14640999u) {
		/* B2: K1 K2 K3 decimal, K4..K7 base 11 with * = 10 (999**** = 14640999) */
		uint32_t v = id;
		for (unsigned k = 7; k-- > 3u;) {
			unsigned d = v % 11u;
			t[k] = d == 10u ? '*' : (char)('0' + d);
			v /= 11u;
		}
		for (unsigned k = 3; k-- > 0;) {
			t[k] = (char)('0' + v % 10u);
			v /= 10u;
		}
		n = 7;
	} else {
		char r[8];
		unsigned m = 0;
		do {
			r[m++] = (char)('0' + id % 10u);
			id /= 10u;
		} while (id && m < sizeof r);
		while (m)
			t[n++] = r[--m];
	}
	if (cap == 0)
		return 0;
	if (n > cap - 1u)
		n = cap - 1u;
	memcpy(out, t, n);
	out[n] = '\0';
	return n;
}
