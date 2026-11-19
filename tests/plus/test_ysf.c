/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus YSF layer self tests (src/ysf; the cross-check against the reference programs is tools/test_ysf_ref.sh):
 * CRC, FICH round trip with errors, voice triplet voting, and the framer: hunting through noise, a sync with
 * errors, back-to-back frames, a lost sync. */
#include <stdio.h>
#include <string.h>
#include "ysf/ysf.h"

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static uint32_t rng = 99u;
static uint32_t rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }
static unsigned getb(const uint8_t *p, unsigned i) { return (p[i >> 3] >> (7u - (i & 7u))) & 1u; }
static void flip(uint8_t *p, unsigned i) { p[i >> 3] ^= (uint8_t)(0x80u >> (i & 7u)); }

static void make_frame(uint8_t frame[YSF_FRAME_BYTES], uint8_t fn, uint8_t params[5][49])
{
	memset(frame, 0, YSF_FRAME_BYTES);
	uint8_t info[4] = { (uint8_t)(YSF_FI_COMMS << 6), (uint8_t)((fn & 7u) << 3 | 6u), YSF_DT_VD2, 0x00 };
	ysf_fich_encode(info, frame);
	for (unsigned j = 0; j < 5u; j++) {
		for (unsigned i = 0; i < 49u; i++)
			params[j][i] = (uint8_t)(rnd() & 1u);
		ysf_vd2_voice_put(params[j], j, frame);
	}
}

int main(void)
{
	/* CRC-16/CCITT, init 0, inverted: "123456789" */
	CHECK(ysf_crc_ccitt((const uint8_t *)"123456789", 9) == (uint16_t)~0x31C3u);

	uint8_t frame[YSF_FRAME_BYTES], p[5][49], got[49];
	ysf_fich_t f;
	make_frame(frame, 3, p);
	CHECK(ysf_fich_decode(frame, &f) && f.fi == YSF_FI_COMMS && f.fn == 3 && f.ft == 6 && f.dt == YSF_DT_VD2);
	for (unsigned e = 0; e < 5u; e++)
		flip(frame, 40u + rnd() % 200u);
	CHECK(ysf_fich_decode(frame, &f) && f.fn == 3);

	/* One wrong bit in each of the 27 triplets of a voice channel: all voted back */
	make_frame(frame, 0, p);
	uint8_t copy[YSF_FRAME_BYTES];
	memcpy(copy, frame, sizeof copy);
	for (unsigned t = 0; t < 27u; t++) {
		unsigned bit = 3u * t + rnd() % 3u;	/* the bit inside the channel, before interleave */
		unsigned pos = 240u + 40u + (bit % 26u) * 4u + bit / 26u;	/* channel 0 */
		flip(frame, pos);
	}
	CHECK(ysf_vd2_voice(frame, 0, got) == 27u && memcmp(got, p[0], 49) == 0);
	CHECK(ysf_vd2_voice(copy, 1, got) == 0 && memcmp(got, p[1], 49) == 0);

	/* Framer: noise, then three frames back to back (the second's sync with 2 bit errors), noise, one more */
	ysf_framer_t fr;
	ysf_framer_init(&fr);
	uint8_t frames[4][YSF_FRAME_BYTES];
	for (unsigned k = 0; k < 4u; k++)
		make_frame(frames[k], (uint8_t)k, p);
	flip(frames[1], 7);
	flip(frames[1], 30);
	unsigned seen = 0, fn_seen[8];
	for (unsigned i = 0; i < 3000u; i++)
		if (ysf_framer_bit(&fr, (uint8_t)(rnd() & 1u))) {
		}
	ysf_framer_init(&fr);	/* random bits may hit the sync by chance; start clean for the count */
	for (unsigned k = 0; k < 3u; k++)
		for (unsigned i = 0; i < YSF_FRAME_BITS; i++)
			if (ysf_framer_bit(&fr, (uint8_t)getb(frames[k], i)) && ysf_fich_decode(fr.frame, &f) && seen < 8u)
				fn_seen[seen++] = f.fn;
	for (unsigned i = 0; i < 517u; i++)
		ysf_framer_bit(&fr, (uint8_t)(i & 1u));	/* 0101...: never the sync */
	for (unsigned i = 0; i < YSF_FRAME_BITS; i++)
		if (ysf_framer_bit(&fr, (uint8_t)getb(frames[3], i)) && ysf_fich_decode(fr.frame, &f) && seen < 8u)
			fn_seen[seen++] = f.fn;
	CHECK(seen == 4u && fn_seen[0] == 0 && fn_seen[1] == 1 && fn_seen[2] == 2 && fn_seen[3] == 3);
	printf("Plus YSF: CRC, FICH with errors, triplet votes, framer through noise and a bad sync: %d failures\n",
	       failures);
	return failures ? 1 : 0;
}
