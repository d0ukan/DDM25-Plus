/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus YSF receive layer (ysf.h). */
#include <string.h>
#include "ysf.h"

static inline unsigned get_bit(const uint8_t *p, unsigned i) { return (p[i >> 3] >> (7u - (i & 7u))) & 1u; }
static inline void put_bit(uint8_t *p, unsigned i, unsigned b)
{
	uint8_t m = (uint8_t)(0x80u >> (i & 7u));
	p[i >> 3] = b ? (uint8_t)(p[i >> 3] | m) : (uint8_t)(p[i >> 3] & ~m);
}

static unsigned popcount32(uint32_t v)
{
	unsigned n = 0;
	while (v) {
		v &= v - 1u;
		n++;
	}
	return n;
}

/* ---- Golay (24,12) ---- */

uint32_t ysf_golay24_encode(uint16_t data12)
{
	uint32_t r = (uint32_t)(data12 & 0xFFFu) << 11;
	for (int i = 22; i >= 11; i--)
		if (r & (1u << i))
			r ^= 0xC75u << (i - 11);
	uint32_t c23 = ((uint32_t)(data12 & 0xFFFu) << 11) | r;
	return (c23 << 1) | (popcount32(c23) & 1u);
}

int ysf_golay24_decode(uint32_t word24, uint16_t *data12)
{
	/* Nearest codeword by search: 4096 candidates, about 40 decodes a second here */
	unsigned best = 25, best_d = 0;
	uint16_t guess = (uint16_t)((word24 >> 12) & 0xFFFu);
	if (popcount32(ysf_golay24_encode(guess) ^ word24) == 0) {
		*data12 = guess;
		return 0;
	}
	for (unsigned d = 0; d < 4096u; d++) {
		unsigned dist = popcount32((ysf_golay24_encode((uint16_t)d) ^ word24) & 0xFFFFFFu);
		if (dist < best) {
			best = dist;
			best_d = d;
		}
	}
	if (best > 3u)
		return -1;
	*data12 = (uint16_t)best_d;
	return (int)best;
}

uint16_t ysf_crc_ccitt(const uint8_t *p, unsigned n)
{
	uint16_t crc = 0;
	for (unsigned i = 0; i < n; i++) {
		crc ^= (uint16_t)p[i] << 8;
		for (unsigned b = 0; b < 8u; b++)
			crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
	}
	return (uint16_t)~crc;
}

/* ---- FICH ---- */

/* K=5 rate 1/2: g1 = d + d3 + d4, g2 = d + d1 + d2 + d4 (taps on the shift register d1..d4, d1 the newest) */
static void conv_encode(const uint8_t *in, unsigned nbits, uint8_t *out)
{
	unsigned d1 = 0, d2 = 0, d3 = 0, d4 = 0, k = 0;
	for (unsigned i = 0; i < nbits; i++) {
		unsigned d = get_bit(in, i);
		put_bit(out, k++, (d + d3 + d4) & 1u);
		put_bit(out, k++, (d + d1 + d2 + d4) & 1u);
		d4 = d3;
		d3 = d2;
		d2 = d1;
		d1 = d;
	}
}

/* Hard decision Viterbi over 16 states (the register d1..d4): steps symbol pairs in coded[] (g1, g2 bit after bit,
 * de-interleaved), steps - 4 data bits out (the 4 tail bits bring the encoder back to state 0); paths[] holds one
 * word a step (which predecessor won, one bit per state). out must hold (steps - 4 + 7) / 8 bytes */
static void viterbi(const uint8_t *coded, unsigned steps, uint16_t *paths, uint8_t *out)
{
	uint16_t metric[16], next[16];
	memset(metric, 0xFF, sizeof metric);
	metric[0] = 0;
	for (unsigned t = 0; t < steps; t++) {
		unsigned s0 = get_bit(coded, 2u * t), s1 = get_bit(coded, 2u * t + 1u);
		uint16_t dec = 0;
		for (unsigned ns = 0; ns < 16u; ns++) {
			/* ns = (d, d1, d2, d3) after the step; the previous state is (d1, d2, d3, d4) */
			unsigned d = (ns >> 3) & 1u, d1 = (ns >> 2) & 1u, d2 = (ns >> 1) & 1u, d3 = ns & 1u;
			uint16_t best = 0xFFFFu;
			unsigned pick = 0;
			for (unsigned d4 = 0; d4 < 2u; d4++) {
				unsigned ps = (d1 << 3) | (d2 << 2) | (d3 << 1) | d4;
				if (metric[ps] == 0xFFFFu)
					continue;
				unsigned g1 = (d + d3 + d4) & 1u, g2 = (d + d1 + d2 + d4) & 1u;
				uint16_t m = (uint16_t)(metric[ps] + (g1 != s0) + (g2 != s1));
				if (m < best) {
					best = m;
					pick = d4;
				}
			}
			next[ns] = best;
			if (pick)
				dec |= (uint16_t)(1u << ns);
		}
		paths[t] = dec;
		memcpy(metric, next, sizeof metric);
	}
	/* The tail brings the encoder back to state 0 */
	unsigned st = 0;
	memset(out, 0, (steps - 4u + 7u) / 8u);
	for (unsigned t = steps; t-- > 0;) {
		if (t < steps - 4u)
			put_bit(out, t, (st >> 3) & 1u);
		unsigned d4 = (paths[t] >> st) & 1u;
		st = ((st << 1) & 0xEu) | d4;
	}
}

/* Undo the 20 x 5 (FICH, V/D mode 2 DCH: 100 dibits) or 20 x 9 (CSD: 180 dibits) dibit interleave: dibit i of the
 * coded stream sits at bit (i / cols) * 2 + (i % cols) * 40 of in[] */
static void deinterleave(const uint8_t *in, unsigned cols, unsigned dibits, uint8_t *coded)
{
	for (unsigned i = 0; i < dibits; i++) {
		unsigned n = (i / cols) * 2u + (i % cols) * 40u;
		put_bit(coded, 2u * i, get_bit(in, n));
		put_bit(coded, 2u * i + 1u, get_bit(in, n + 1u));
	}
}

static void interleave(const uint8_t *coded, unsigned cols, unsigned dibits, uint8_t *out)
{
	for (unsigned i = 0; i < dibits; i++) {
		unsigned n = (i / cols) * 2u + (i % cols) * 40u;
		put_bit(out, n, get_bit(coded, 2u * i));
		put_bit(out, n + 1u, get_bit(coded, 2u * i + 1u));
	}
}

void ysf_fich_fields(const uint8_t raw[4], ysf_fich_t *f)
{
	memcpy(f->raw, raw, 4);
	f->fi = (raw[0] >> 6) & 3u;
	f->cm = (raw[0] >> 2) & 3u;
	f->bn = raw[0] & 3u;
	f->bt = (raw[1] >> 6) & 3u;
	f->fn = (raw[1] >> 3) & 7u;
	f->ft = raw[1] & 7u;
	f->dt = raw[2] & 3u;
	f->voip = (raw[2] & 0x04u) != 0;
	f->mr = (raw[2] >> 3) & 3u;
	f->dev = (raw[2] & 0x40u) != 0;
	f->dgid = raw[3] & 0x7Fu;
}

bool ysf_fich_decode(const uint8_t frame[YSF_FRAME_BYTES], ysf_fich_t *out)
{
	uint8_t coded[25] = { 0 }, dec[12];
	uint16_t paths[100];
	deinterleave(frame + 5, 5u, 100u, coded);
	viterbi(coded, 100u, paths, dec);
	uint8_t raw[6];
	uint16_t w[4];
	for (unsigned k = 0; k < 4u; k++) {
		uint32_t word = ((uint32_t)dec[3 * k] << 16) | ((uint32_t)dec[3 * k + 1] << 8) | dec[3 * k + 2];
		if (ysf_golay24_decode(word, &w[k]) < 0)
			return false;
	}
	raw[0] = (uint8_t)(w[0] >> 4);
	raw[1] = (uint8_t)(((w[0] << 4) & 0xF0u) | ((w[1] >> 8) & 0x0Fu));
	raw[2] = (uint8_t)w[1];
	raw[3] = (uint8_t)(w[2] >> 4);
	raw[4] = (uint8_t)(((w[2] << 4) & 0xF0u) | ((w[3] >> 8) & 0x0Fu));
	raw[5] = (uint8_t)w[3];
	uint16_t crc = ysf_crc_ccitt(raw, 4);
	if (raw[4] != (uint8_t)(crc >> 8) || raw[5] != (uint8_t)crc)
		return false;
	ysf_fich_fields(raw, out);
	return true;
}

void ysf_fich_encode(const uint8_t info[4], uint8_t frame[YSF_FRAME_BYTES])
{
	static const uint8_t sync[5] = { 0xD4, 0x71, 0xC9, 0x63, 0x4D };
	memcpy(frame, sync, 5);
	uint8_t raw[6];
	memcpy(raw, info, 4);
	uint16_t crc = ysf_crc_ccitt(raw, 4);
	raw[4] = (uint8_t)(crc >> 8);
	raw[5] = (uint8_t)crc;
	uint16_t w[4] = {
		(uint16_t)(((raw[0] << 4) & 0xFF0u) | (raw[1] >> 4)), (uint16_t)(((raw[1] << 8) & 0xF00u) | raw[2]),
		(uint16_t)(((raw[3] << 4) & 0xFF0u) | (raw[4] >> 4)), (uint16_t)(((raw[4] << 8) & 0xF00u) | raw[5]),
	};
	uint8_t conv[13] = { 0 }, coded[25] = { 0 };
	for (unsigned k = 0; k < 4u; k++) {
		uint32_t c = ysf_golay24_encode(w[k]);
		conv[3 * k] = (uint8_t)(c >> 16);
		conv[3 * k + 1] = (uint8_t)(c >> 8);
		conv[3 * k + 2] = (uint8_t)c;
	}
	conv_encode(conv, 100, coded);
	interleave(coded, 5u, 100u, frame + 5);
}

/* ---- V/D mode 2 voice ---- */

/* The PN whitening of the voice and data channels (MMDVMHost WHITENING_DATA) */
static const uint8_t whitening[YSF_CSD_BYTES] = { 0x93, 0xD7, 0x51, 0x21, 0x9C, 0x2F, 0x6C, 0xD0, 0xEF, 0x0F,
						  0xF8, 0x3D, 0xF1, 0x73, 0x20, 0x94, 0xED, 0x1E, 0x7C, 0xD8 };

/* Bit i (0..103) of a voice channel sits at bit IL26(i) of its 104: a 26 x 4 bit interleave */
static unsigned il26(unsigned i) { return (i % 26u) * 4u + i / 26u; }

/* The channel's first bit: past the sync and FICH (240 bits), then the 40-bit DCH, 144 bits a section */
static unsigned vch_base(unsigned j) { return 240u + 40u + 144u * j; }

unsigned ysf_vd2_voice(const uint8_t frame[YSF_FRAME_BYTES], unsigned j, uint8_t params[YSF_AMBE_BITS])
{
	uint8_t vch[13] = { 0 };
	unsigned base = vch_base(j), outvoted = 0;
	for (unsigned i = 0; i < 104u; i++)
		put_bit(vch, i, get_bit(frame, base + il26(i)));
	for (unsigned i = 0; i < 13u; i++)
		vch[i] ^= whitening[i];
	/* 27 bit triplets (a12, b12, then the first 3 of c), majority voted; then 22 plain bits of c */
	for (unsigned t = 0; t < 27u; t++) {
		unsigned v = get_bit(vch, 3 * t) + get_bit(vch, 3 * t + 1) + get_bit(vch, 3 * t + 2);
		if (v == 1u || v == 2u)
			outvoted++;
		params[t] = (uint8_t)(v >= 2u);
	}
	for (unsigned i = 0; i < 22u; i++)
		params[27 + i] = (uint8_t)get_bit(vch, 81u + i);
	return outvoted;
}

void ysf_vd2_voice_put(const uint8_t params[YSF_AMBE_BITS], unsigned j, uint8_t frame[YSF_FRAME_BYTES])
{
	uint8_t vch[13] = { 0 };
	for (unsigned t = 0; t < 27u; t++)
		for (unsigned r = 0; r < 3u; r++)
			put_bit(vch, 3 * t + r, params[t] & 1u);
	for (unsigned i = 0; i < 22u; i++)
		put_bit(vch, 81u + i, params[27 + i] & 1u);
	for (unsigned i = 0; i < 13u; i++)
		vch[i] ^= whitening[i];
	unsigned base = vch_base(j);
	for (unsigned i = 0; i < 104u; i++)
		put_bit(frame, base + il26(i), get_bit(vch, i));
}

/* ---- V/D mode 2 data channel ---- */

/* The DCH pieces: 40 bits at the start of each 144-bit section of the payload */
static unsigned dch_bit(unsigned i) { return 240u + (i / 40u) * 144u + i % 40u; }

bool ysf_vd2_data(const uint8_t frame[YSF_FRAME_BYTES], uint8_t out[10])
{
	uint8_t dch[25] = { 0 }, coded[25] = { 0 }, dec[12];
	uint16_t paths[100];
	for (unsigned i = 0; i < 200u; i++)
		put_bit(dch, i, get_bit(frame, dch_bit(i)));
	deinterleave(dch, 5u, 100u, coded);	/* the same 20 x 5 dibit interleave as the FICH */
	viterbi(coded, 100u, paths, dec);
	uint16_t crc = ysf_crc_ccitt(dec, 10);
	if (dec[10] != (uint8_t)(crc >> 8) || dec[11] != (uint8_t)crc)
		return false;
	for (unsigned i = 0; i < 10u; i++)
		out[i] = dec[i] ^ whitening[i];
	return true;
}

void ysf_vd2_data_put(const uint8_t in[10], uint8_t frame[YSF_FRAME_BYTES])
{
	uint8_t raw[13] = { 0 }, coded[25] = { 0 }, dch[25] = { 0 };
	for (unsigned i = 0; i < 10u; i++)
		raw[i] = in[i] ^ whitening[i];
	uint16_t crc = ysf_crc_ccitt(raw, 10);
	raw[10] = (uint8_t)(crc >> 8);
	raw[11] = (uint8_t)crc;
	conv_encode(raw, 100, coded);
	interleave(coded, 5u, 100u, dch);
	for (unsigned i = 0; i < 200u; i++)
		put_bit(frame, dch_bit(i), get_bit(dch, i));
}

/* ---- Callsign data channels (header, terminator, Voice FR sub-header) ---- */

/* Frame byte of coded byte b (0..44): CSD1 / CSD2 in 9-byte pieces at the start / middle of each 18-byte payload
 * section, CSD3 in payload bytes 0..44 */
static unsigned csd_byte(unsigned which, unsigned b)
{
	if (which == YSF_CSD3)
		return 30u + b;
	return 30u + (b / 9u) * 18u + b % 9u + (which == YSF_CSD2 ? 9u : 0u);
}

void ysf_csd_put(const uint8_t in[YSF_CSD_BYTES], unsigned which, uint8_t frame[YSF_FRAME_BYTES])
{
	uint8_t raw[23] = { 0 }, coded[45] = { 0 }, block[45] = { 0 };
	for (unsigned i = 0; i < YSF_CSD_BYTES; i++)
		raw[i] = in[i] ^ whitening[i];
	uint16_t crc = ysf_crc_ccitt(raw, YSF_CSD_BYTES);
	raw[20] = (uint8_t)(crc >> 8);
	raw[21] = (uint8_t)crc;
	conv_encode(raw, 180u, coded);	/* 176 bits and 4 zero tail bits */
	interleave(coded, 9u, 180u, block);
	for (unsigned b = 0; b < 45u; b++)
		frame[csd_byte(which, b)] = block[b];
}

bool ysf_csd_get(const uint8_t frame[YSF_FRAME_BYTES], unsigned which, uint8_t out[YSF_CSD_BYTES])
{
	uint8_t block[45], coded[45] = { 0 }, dec[22];
	uint16_t paths[180];
	for (unsigned b = 0; b < 45u; b++)
		block[b] = frame[csd_byte(which, b)];
	deinterleave(block, 9u, 180u, coded);
	viterbi(coded, 180u, paths, dec);
	uint16_t crc = ysf_crc_ccitt(dec, YSF_CSD_BYTES);
	if (dec[20] != (uint8_t)(crc >> 8) || dec[21] != (uint8_t)crc)
		return false;
	for (unsigned i = 0; i < YSF_CSD_BYTES; i++)
		out[i] = dec[i] ^ whitening[i];
	return true;
}

/* ---- Framer ---- */

void ysf_framer_init(ysf_framer_t *f)
{
	memset(f, 0, sizeof *f);
}

static unsigned sync_errors(uint64_t last40)
{
	uint64_t x = (last40 ^ YSF_SYNC) & 0xFFFFFFFFFFULL;
	return popcount32((uint32_t)x) + popcount32((uint32_t)(x >> 32));
}

bool ysf_framer_bit(ysf_framer_t *f, uint8_t bit)
{
	f->shift = ((f->shift << 1) | (bit & 1u)) & 0xFFFFFFFFFFULL;
	if (f->have == 0) {
		if (sync_errors(f->shift) > YSF_SYNC_MAX_ERRORS)
			return false;
		/* The sync is the frame's first 40 bits: write it as it should be */
		static const uint8_t sync[5] = { 0xD4, 0x71, 0xC9, 0x63, 0x4D };
		memcpy(f->frame, sync, 5);
		f->have = YSF_SYNC_BITS;
		return false;
	}
	put_bit(f->frame, f->have, bit & 1u);
	if (++f->have < YSF_FRAME_BITS)
		return false;
	f->frames++;
	f->have = 0;
	return true;
}
