// SPDX-License-Identifier: GPL-3.0-or-later
// DM32 Plus YSF Voice FR mode (src/ysf/ysf_vw.c, with src/p25/p25.c's IMBE channel code) against the reference
// implementations, compiled on the host only (tools/test_ysf_vw_ref.sh):
//  - MMDVMHost (G4KLX, GPL-2.0-or-later): CYSFPayload::processVoiceFRModeAudio5/Audio2 and through them
//    CAMBEFEC::regenerateIMBE (IMBE_INTERLEAVE, Golay (23,12), Hamming (15,11), the PN whitening); CYSFControl's
//    choice between Audio2 and Audio5 (copied below as the oracle)
//  - MMDVM_CM YSF2P25 (EA7EE, GPL-2.0-or-later): CModeConv::decode, a VW section to the 88 IMBE parameter bits
//    (copied below as the oracle)
// The reference sources are not in this tree: the runner compiles them from research/ref.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "YSFPayload.h"
#include "vw_ref_tables.h"	// IMBE_INTERLEAVE_HOST, IMBE_INTERLEAVE_CM: extracted by the runner
extern "C" {
#include "ysf/ysf.h"
#include "ysf/ysf_vw.h"
#include "p25/p25.h"
}

static int failures;
static unsigned checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint32_t rng = 777u;
static uint32_t rnd() { rng = rng * 1103515245u + 12345u; return rng >> 8; }
static unsigned getb(const uint8_t *p, unsigned i) { return (p[i >> 3] >> (7u - (i & 7u))) & 1u; }
static void flip(uint8_t *p, unsigned i) { p[i >> 3] ^= (uint8_t)(0x80u >> (i & 7u)); }

// CYSFControl::writeModem / writeNetwork, case YSF_DT_VOICE_FR_MODE (the oracle): Audio2 for FN 0 / FT 1, else Audio5
static unsigned oracle_count(unsigned fn, unsigned ft) { return (fn == 0U && ft == 1U) ? 2U : 5U; }

// MMDVM_CM YSF2P25 CModeConv::decode (the oracle): one 18-byte VW section to the 88-bit IMBE frame, no FEC
static void oracle_cm_decode(const uint8_t *data, uint8_t *imbe)
{
	bool bit[144U];
	for (unsigned i = 0U; i < 144U; i++)
		bit[i] = getb(data, IMBE_INTERLEAVE_CM[i]) != 0U;
	unsigned c0data = 0U;
	for (unsigned i = 0U; i < 12U; i++)
		c0data = (c0data << 1) | (bit[i] ? 1U : 0U);
	bool prn[114U];
	unsigned p = 16U * c0data;
	for (unsigned i = 0U; i < 114U; i++) {
		p = (173U * p + 13849U) % 65536U;
		prn[i] = p >= 32768U;
	}
	for (unsigned i = 0U; i < 114U; i++)
		bit[i + 23U] ^= prn[i];
	std::memset(imbe, 0, 11);
	unsigned offset = 0U;
	static const unsigned from[8] = {0U, 23U, 46U, 69U, 92U, 107U, 122U, 137U}, n[8] = {12, 12, 12, 12, 11, 11, 11, 7};
	for (unsigned w = 0U; w < 8U; w++)
		for (unsigned i = 0U; i < n[w]; i++, offset++)
			if (bit[from[w] + i])
				imbe[offset >> 3] |= (uint8_t)(0x80U >> (offset & 7U));
}

// dm32_imbe_pack's layout (dm32_imbe.h): u0..u7 MSB first, widths {12,12,12,12,11,11,11,7}
static void pack88(const uint16_t u[8], uint8_t out[11])
{
	std::memset(out, 0, 11);
	unsigned o = 0U;
	for (unsigned w = 0U; w < 8U; w++) {
		unsigned n = w < 4U ? 12U : w < 7U ? 11U : 7U;
		for (unsigned i = 0U; i < n; i++, o++)
			if ((u[w] >> (n - 1U - i)) & 1U)
				out[o >> 3] |= (uint8_t)(0x80U >> (o & 7U));
	}
}

static void random_u(uint16_t u[8])
{
	for (unsigned i = 0U; i < 8U; i++)
		u[i] = (uint16_t)(rnd() & ((1U << (i < 4U ? 12U : i < 7U ? 11U : 7U)) - 1U));
}

static ysf_fich_t fich(unsigned fn, unsigned ft)
{
	uint8_t raw[4] = {(uint8_t)(YSF_FI_COMMS << 6), (uint8_t)((fn & 7U) << 3 | (ft & 7U)), YSF_DT_VOICE_FR, 0};
	ysf_fich_t f;
	ysf_fich_fields(raw, &f);
	return f;
}

// The codeword words: c0..c3 Golay (23,12), c4..c6 Hamming (15,11), c7 plain
static const unsigned W_START[8] = {0, 23, 46, 69, 92, 107, 122, 137}, W_LEN[8] = {23, 23, 23, 23, 15, 15, 15, 7};

// Flip e distinct bits of word w of cw, chosen from [lo, lo + n) of that word
static void flip_word(uint8_t *cw, unsigned w, unsigned e, unsigned lo, unsigned n)
{
	uint8_t mark[23] = {0};
	for (unsigned done = 0U; done < e;) {
		unsigned b = lo + rnd() % n;
		if (mark[b])
			continue;
		mark[b] = 1;
		flip(cw, W_START[w] + b);
		done++;
	}
}

// Run the reference over a frame (a copy), Audio2 or Audio5 as CYSFControl picks
static unsigned ref_regenerate(uint8_t frame[120], unsigned fn, unsigned ft)
{
	CYSFPayload payload;
	return oracle_count(fn, ft) == 2U ? payload.processVoiceFRModeAudio2(frame) : payload.processVoiceFRModeAudio5(frame);
}

// Ours: every voice frame through p25_imbe_decode and back through p25_imbe_encode (what regenerateIMBE does)
static int our_regenerate(uint8_t frame[120], const ysf_fich_t &f, uint16_t u[5][8], unsigned corrected[5])
{
	for (unsigned k = 0U; k < ysf_vw_voice_count(&f); k++) {
		uint8_t cw[18], e0;
		if (!ysf_vw_voice(frame, &f, k, cw))
			return -1;
		int n = p25_imbe_decode(cw, u[k], &e0);
		if (n < 0)
			return -1;
		corrected[k] = (unsigned)n;
		if (p25_imbe_encode(u[k], cw) != 0 || !ysf_vw_voice_put(cw, &f, k, frame))
			return -1;
	}
	return 0;
}

int main()
{
	// 1. The interleave: both references' IMBE_INTERLEAVE, one codeword bit at a time, every section
	for (unsigned j = 0U; j < 144U; j++)
		CHECK(IMBE_INTERLEAVE_HOST[j] == IMBE_INTERLEAVE_CM[j]);
	for (unsigned s = 0U; s < 5U; s++)
		for (unsigned j = 0U; j < 144U; j++) {
			uint8_t cw[18] = {0}, frame[120] = {0};
			cw[j >> 3] = (uint8_t)(0x80U >> (j & 7U));
			ysf_vw_section_put(cw, s, frame);
			unsigned want = 240U + 144U * s + IMBE_INTERLEAVE_HOST[j], ones = 0U;
			for (unsigned i = 0U; i < 960U; i++)
				ones += getb(frame, i);
			CHECK(ones == 1U && getb(frame, want) == 1U);
		}

	// 2. Which sections are voice: our count against CYSFControl's choice, every FN/FT; and the reference touches
	// exactly our voice sections (a frame of noise: every 18-byte section it rewrites is one we call voice)
	for (unsigned fn = 0U; fn < 8U; fn++)
		for (unsigned ft = 0U; ft < 8U; ft++) {
			ysf_fich_t f = fich(fn, ft);
			CHECK(ysf_vw_voice_count(&f) == oracle_count(fn, ft));
			for (unsigned t = 0U; t < 20U; t++) {
				uint8_t frame[120], ref[120];
				for (unsigned i = 0U; i < 120U; i++)
					frame[i] = (uint8_t)rnd();
				std::memcpy(ref, frame, 120);
				ref_regenerate(ref, fn, ft);
				CHECK(std::memcmp(ref, frame, 30) == 0);
				bool voice[5] = {false, false, false, false, false};
				for (unsigned k = 0U; k < ysf_vw_voice_count(&f); k++)
					voice[ysf_vw_voice_section(&f, k)] = true;
				for (unsigned s = 0U; s < 5U; s++)
					if (!voice[s])
						CHECK(std::memcmp(ref + 30 + 18 * s, frame + 30 + 18 * s, 18) == 0);
			}
		}

	// 3. Clean frames: IMBE parameters -> p25_imbe_encode -> ysf_vw_voice_put is a frame the reference finds no
	// error in and leaves bit for bit; YSF2P25's decode reads back the same 88 parameter bits
	unsigned clean = 0U;
	for (unsigned t = 0U; t < 3000U; t++) {
		unsigned fn = t % 8U, ft = (t / 8U) % 8U;
		ysf_fich_t f = fich(fn, ft);
		uint8_t frame[120], ref[120];
		for (unsigned i = 0U; i < 120U; i++)
			frame[i] = (uint8_t)rnd();	// the DCH area of a FN 0 / FT 1 frame stays noise
		uint16_t u[5][8];
		for (unsigned k = 0U; k < ysf_vw_voice_count(&f); k++) {
			uint8_t cw[18], packed[11], cm[11];
			random_u(u[k]);
			CHECK(p25_imbe_encode(u[k], cw) == 0);
			CHECK(ysf_vw_voice_put(cw, &f, k, frame));
			oracle_cm_decode(frame + 30 + 18 * ysf_vw_voice_section(&f, k), cm);
			pack88(u[k], packed);
			CHECK(std::memcmp(cm, packed, 11) == 0);
			clean++;
		}
		std::memcpy(ref, frame, 120);
		CHECK(ref_regenerate(ref, fn, ft) == 0U);
		CHECK(std::memcmp(ref, frame, 120) == 0);
	}

	// 4. Channel errors the reference corrects: both regenerate the same frame bit for bit, count the same errors,
	// and ours returns the sent parameters. The budget is the reference's: CAMBEFEC::regenerateIMBE writes each
	// corrected 23-bit Golay word back as 24 bits, zeroing the first bit of the next word before it is decoded,
	// so c1..c3 get 2 errors (not 3) and c4 none but its first bit; c0 3, c5 and c6 1, c7 (unprotected) up to 2
	unsigned with_errors = 0U, bits_fixed = 0U;
	for (unsigned t = 0U; t < 6000U; t++) {
		unsigned fn = t % 8U, ft = (t / 8U) % 8U;
		ysf_fich_t f = fich(fn, ft);
		uint8_t frame[120], ref[120];
		for (unsigned i = 0U; i < 120U; i++)
			frame[i] = (uint8_t)rnd();
		uint16_t u[5][8], got[5][8];
		unsigned n = ysf_vw_voice_count(&f), injected = 0U, c7flips[5] = {0}, corrected[5] = {0};
		for (unsigned k = 0U; k < n; k++) {
			uint8_t cw[18], before[18];
			random_u(u[k]);
			CHECK(p25_imbe_encode(u[k], cw) == 0);
			std::memcpy(before, cw, 18);
			static const unsigned cap[8] = {3, 2, 2, 2, 0, 1, 1, 2};
			for (unsigned w = 0U; w < 8U; w++)
				flip_word(cw, w, rnd() % (cap[w] + 1U), 0U, W_LEN[w]);
			if (rnd() & 1U)
				flip(cw, 92U);	// c4's first bit, the one the reference zeroes anyway
			for (unsigned i = 0U; i < 144U; i++)
				injected += getb(cw, i) != getb(before, i);
			for (unsigned i = 0U; i < 7U; i++)
				c7flips[k] |= (getb(cw, 137U + i) != getb(before, 137U + i)) << (6U - i);
			CHECK(ysf_vw_voice_put(cw, &f, k, frame));
		}
		std::memcpy(ref, frame, 120);
		unsigned ref_errors = ref_regenerate(ref, fn, ft);
		CHECK(our_regenerate(frame, f, got, corrected) == 0);
		CHECK(std::memcmp(ref, frame, 120) == 0);
		// Both count the bits they corrected; c7 errors pass through uncounted (nothing can see them)
		unsigned ours = 0U, c7 = 0U;
		for (unsigned k = 0U; k < n; k++) {
			CHECK(std::memcmp(got[k], u[k], 7 * sizeof(uint16_t)) == 0);
			CHECK(got[k][7] == (u[k][7] ^ c7flips[k]));
			for (unsigned v = c7flips[k]; v; v &= v - 1U)
				c7++;
			ours += corrected[k];
			with_errors++;
		}
		CHECK(ref_errors == ours);
		CHECK(ours + c7 == injected);
		bits_fixed += injected;
	}

	// 5. Beyond the reference's budget (information only): 3 errors in c1..c3, or one error in c4 off its first
	// bit, which p25_imbe_decode corrects; how often the reference regenerates something else
	unsigned beyond = 0U, ref_differs = 0U;
	for (unsigned t = 0U; t < 2000U; t++) {
		ysf_fich_t f = fich(2, 6);
		uint8_t frame[120] = {0}, ref[120];
		uint16_t u[5][8], got[5][8];
		unsigned corrected[5];
		for (unsigned k = 0U; k < 5U; k++) {
			uint8_t cw[18];
			random_u(u[k]);
			p25_imbe_encode(u[k], cw);
			if (t & 1U)
				flip_word(cw, 1U + rnd() % 3U, 3U, 0U, 23U);
			else
				flip_word(cw, 4U, 1U, 1U, 14U);
			ysf_vw_voice_put(cw, &f, k, frame);
		}
		std::memcpy(ref, frame, 120);
		ref_regenerate(ref, 2, 6);
		CHECK(our_regenerate(frame, f, got, corrected) == 0);
		for (unsigned k = 0U; k < 5U; k++)
			CHECK(std::memcmp(got[k], u[k], sizeof got[k]) == 0);
		ref_differs += std::memcmp(ref, frame, 120) != 0;
		beyond++;
	}

	std::printf("test_ysf_vw_ref: %u checks; %u clean IMBE frames, %u IMBE frames through %u injected bit errors, all "
		    "identical to MMDVMHost; YSF2P25 decode agrees; %d failures\n", checks, clean, with_errors, bits_fixed,
		    failures);
	std::printf("info: beyond MMDVMHost's regenerateIMBE budget, it differs from ours on %u of %u frames "
		    "(ours recovers all)\n", ref_differs, beyond);
	return failures != 0;
}
