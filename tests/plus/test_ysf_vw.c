/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus YSF Voice FR mode self tests (src/ysf/ysf_vw.c; the cross-check against the reference programs is
 * tools/test_ysf_vw_ref.sh): which sections are voice, the interleave is a permutation that stays inside its
 * section, round trips through every section, and the whole receive path (ysf_vw_voice -> p25_imbe_decode)
 * correcting channel errors.
 *   gcc -std=gnu99 -Wall -Wextra -Isrc -Isrc/p25 tests/plus/test_ysf_vw.c src/ysf/ysf_vw.c src/ysf/ysf.c \
 *       src/p25/p25.c -o test_ysf_vw */
#include <stdio.h>
#include <string.h>
#include "ysf/ysf.h"
#include "ysf/ysf_vw.h"
#include "p25/p25.h"

static int failures;
static unsigned checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static uint32_t rng = 4242u;
static uint32_t rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }
static unsigned getb(const uint8_t *p, unsigned i) { return (p[i >> 3] >> (7u - (i & 7u))) & 1u; }
static void flip(uint8_t *p, unsigned i) { p[i >> 3] ^= (uint8_t)(0x80u >> (i & 7u)); }

static ysf_fich_t fich(unsigned fi, unsigned fn, unsigned ft, unsigned dt)
{
	uint8_t raw[4] = { (uint8_t)(fi << 6), (uint8_t)((fn & 7u) << 3 | (ft & 7u)), (uint8_t)dt, 0 };
	ysf_fich_t f;
	ysf_fich_fields(raw, &f);
	return f;
}

static void random_u(uint16_t u[8])
{
	for (unsigned i = 0; i < 8u; i++)
		u[i] = (uint16_t)(rnd() & ((1u << (i < 4u ? 12u : i < 7u ? 11u : 7u)) - 1u));
}

int main(void)
{
	/* Which sections are voice */
	for (unsigned fn = 0; fn < 8u; fn++)
		for (unsigned ft = 0; ft < 8u; ft++) {
			ysf_fich_t f = fich(YSF_FI_COMMS, fn, ft, YSF_DT_VOICE_FR);
			unsigned n = ysf_vw_voice_count(&f);
			CHECK(n == ((fn == 0u && ft == 1u) ? 2u : 5u));
			for (unsigned k = 0; k < n; k++)
				CHECK(ysf_vw_voice_section(&f, k) == (n == 2u ? 3u + k : k));
		}
	for (unsigned dt = 0; dt < 3u; dt++) {
		ysf_fich_t f = fich(YSF_FI_COMMS, 2, 6, dt);
		CHECK(ysf_vw_voice_count(&f) == 0u);
	}
	for (unsigned fi = 0; fi < 4u; fi++) {
		ysf_fich_t f = fich(fi, 2, 6, YSF_DT_VOICE_FR);
		CHECK(ysf_vw_voice_count(&f) == (fi == YSF_FI_COMMS ? 5u : 0u));
	}

	/* The interleave: each codeword bit lands on its own bit of its own section, nothing else moves */
	for (unsigned s = 0; s < YSF_VW_SECTIONS; s++) {
		uint8_t hit[YSF_FRAME_BITS] = { 0 };
		for (unsigned j = 0; j < YSF_VW_IMBE_BITS; j++) {
			uint8_t cw[YSF_VW_IMBE_BYTES] = { 0 }, frame[YSF_FRAME_BYTES] = { 0 };
			cw[j >> 3] = (uint8_t)(0x80u >> (j & 7u));
			ysf_vw_section_put(cw, s, frame);
			unsigned ones = 0, at = 0;
			for (unsigned i = 0; i < YSF_FRAME_BITS; i++)
				if (getb(frame, i)) {
					ones++;
					at = i;
				}
			CHECK(ones == 1u && at >= 240u + 144u * s && at < 240u + 144u * (s + 1u));
			hit[at]++;
		}
		unsigned covered = 0;
		for (unsigned i = 0; i < YSF_FRAME_BITS; i++)
			covered += hit[i] == 1u;
		CHECK(covered == YSF_VW_IMBE_BITS);
	}

	/* Round trips: random codewords through every voice slot; sync, FICH and the DCH area of a FN 0 / FT 1 frame
	 * stay as they were; a k beyond the count is refused and changes nothing */
	for (unsigned t = 0; t < 400u; t++) {
		unsigned fn = t % 8u, ft = (t / 8u) % 8u;
		ysf_fich_t f = fich(YSF_FI_COMMS, fn, ft, YSF_DT_VOICE_FR);
		uint8_t frame[YSF_FRAME_BYTES], before[YSF_FRAME_BYTES], cw[5][YSF_VW_IMBE_BYTES], got[YSF_VW_IMBE_BYTES];
		for (unsigned i = 0; i < YSF_FRAME_BYTES; i++)
			frame[i] = (uint8_t)rnd();
		memcpy(before, frame, sizeof frame);
		unsigned n = ysf_vw_voice_count(&f);
		for (unsigned k = 0; k < n; k++) {
			for (unsigned i = 0; i < YSF_VW_IMBE_BYTES; i++)
				cw[k][i] = (uint8_t)rnd();
			CHECK(ysf_vw_voice_put(cw[k], &f, k, frame));
		}
		for (unsigned k = 0; k < n; k++)
			CHECK(ysf_vw_voice(frame, &f, k, got) && memcmp(got, cw[k], sizeof got) == 0);
		unsigned untouched = n == 2u ? 30u + 54u : 30u;
		CHECK(memcmp(frame, before, untouched) == 0);
		memset(got, 0x5A, sizeof got);
		uint8_t again[YSF_FRAME_BYTES];
		memcpy(again, frame, sizeof frame);
		CHECK(!ysf_vw_voice(frame, &f, n, got) && got[0] == 0x5A && got[17] == 0x5A);
		CHECK(!ysf_vw_voice_put(cw[0], &f, n, frame) && memcmp(again, frame, sizeof frame) == 0);
	}

	/* The receive path: IMBE parameters -> p25_imbe_encode -> a VW frame -> channel errors in the air bits ->
	 * ysf_vw_voice -> p25_imbe_decode gives the parameters back. Errors: up to 3 in each Golay word (c0..c3),
	 * 1 in each Hamming word (c4..c6); c7 is unprotected and left clean */
	static const unsigned start[7] = { 0, 23, 46, 69, 92, 107, 122 }, len[7] = { 23, 23, 23, 23, 15, 15, 15 };
	static const unsigned cap[7] = { 3, 3, 3, 3, 1, 1, 1 };
	unsigned decoded = 0;
	for (unsigned t = 0; t < 2000u; t++) {
		ysf_fich_t f = fich(YSF_FI_COMMS, t % 8u, (t / 8u) % 8u, YSF_DT_VOICE_FR);
		uint8_t frame[YSF_FRAME_BYTES] = { 0 }, info[4] = { f.raw[0], f.raw[1], f.raw[2], f.raw[3] };
		ysf_fich_encode(info, frame);
		uint16_t u[5][8];
		unsigned n = ysf_vw_voice_count(&f), injected[5] = { 0 };
		for (unsigned k = 0; k < n; k++) {
			uint8_t cw[YSF_VW_IMBE_BYTES];
			random_u(u[k]);
			CHECK(p25_imbe_encode(u[k], cw) == 0);
			uint8_t mark[YSF_VW_IMBE_BITS] = { 0 };	/* distinct positions within a word */
			for (unsigned w = 0; w < 7u; w++) {
				unsigned e = rnd() % (cap[w] + 1u);
				for (unsigned done = 0; done < e;) {
					unsigned b = start[w] + rnd() % len[w];
					if (mark[b])
						continue;
					mark[b] = 1;
					flip(cw, b);
					done++;
				}
				injected[k] += e;
			}
			CHECK(ysf_vw_voice_put(cw, &f, k, frame));
		}
		ysf_fich_t rx;
		CHECK(ysf_fich_decode(frame, &rx) && ysf_vw_voice_count(&rx) == n);
		for (unsigned k = 0; k < n; k++) {
			uint8_t cw[YSF_VW_IMBE_BYTES], e0;
			uint16_t got[8];
			CHECK(ysf_vw_voice(frame, &rx, k, cw));
			int corrected = p25_imbe_decode(cw, got, &e0);
			CHECK(corrected == (int)injected[k]);
			CHECK(memcmp(got, u[k], sizeof got) == 0);
			decoded++;
		}
	}

	printf("test_ysf_vw: %u checks, %u IMBE frames decoded through errors, %d failures\n", checks, decoded,
	       failures);
	return failures != 0;
}
