// SPDX-License-Identifier: GPL-3.0-or-later
// DM32 Plus YSF layer (src/ysf) against the reference implementations, compiled on the host only:
//  - MMDVMHost (G4KLX, GPL-2.0-or-later): CYSFFICH, CGolay24128, CCRC, the YSF convolutional code
//  - MMDVM_CM YSF2DMR (EA7EE, GPL-2.0-or-later): how a V/D mode 2 voice channel becomes the AMBE+2 a/b/c words
// The reference sources are not in this tree: tools/test_classic.py compiles them from research/ref when present.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "YSFFICH.h"
#include "Golay24128.h"
#include "YSFConvolution.h"
#include "CRC.h"
extern "C" {
#include "ysf/ysf.h"
}

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint32_t rng = 12345u;
static uint32_t rnd() { rng = rng * 1103515245u + 12345u; return rng >> 8; }

static void flip(uint8_t *p, unsigned bit) { p[bit >> 3] ^= (uint8_t)(0x80u >> (bit & 7u)); }
static unsigned getb(const uint8_t *p, unsigned i) { return (p[i >> 3] >> (7u - (i & 7u))) & 1u; }

// MMDVM_CM CModeConv::putYSF's extraction of one voice channel (the oracle), from the frame as received
static void oracle_vd2(const uint8_t *frame, unsigned j, unsigned &a, unsigned &b, unsigned &c)
{
	static const unsigned IL[104] = {
		0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 60, 64, 68, 72, 76, 80, 84, 88, 92, 96, 100,
		1, 5, 9, 13, 17, 21, 25, 29, 33, 37, 41, 45, 49, 53, 57, 61, 65, 69, 73, 77, 81, 85, 89, 93, 97, 101,
		2, 6, 10, 14, 18, 22, 26, 30, 34, 38, 42, 46, 50, 54, 58, 62, 66, 70, 74, 78, 82, 86, 90, 94, 98, 102,
		3, 7, 11, 15, 19, 23, 27, 31, 35, 39, 43, 47, 51, 55, 59, 63, 67, 71, 75, 79, 83, 87, 91, 95, 99, 103};
	static const uint8_t W[13] = {0x93, 0xD7, 0x51, 0x21, 0x9C, 0x2F, 0x6C, 0xD0, 0xEF, 0x0F, 0xF8, 0x3D, 0xF1};
	const uint8_t *data = frame + 5 + 25;
	unsigned offset = 40u + 144u * j;
	uint8_t vch[13] = {0};
	for (unsigned i = 0; i < 104u; i++)
		if (getb(data, offset + IL[i]))
			vch[i >> 3] |= (uint8_t)(0x80u >> (i & 7u));
	for (unsigned i = 0; i < 13u; i++)
		vch[i] ^= W[i];
	a = b = c = 0;
	for (unsigned i = 0; i < 12u; i++) a = (a << 1) | getb(vch, 3u * i + 1u);
	for (unsigned i = 0; i < 12u; i++) b = (b << 1) | getb(vch, 3u * (i + 12u) + 1u);
	for (unsigned i = 0; i < 3u; i++) c = (c << 1) | getb(vch, 3u * (i + 24u) + 1u);
	for (unsigned i = 0; i < 22u; i++) c = (c << 1) | getb(vch, i + 81u);
}

// CYSFPayload::processVDMode2Data's decode (the oracle): the 10 DCH bytes, false on a CRC failure
static bool oracle_dch(const uint8_t *frame, uint8_t out10[10])
{
	static const uint8_t W[10] = {0x93, 0xD7, 0x51, 0x21, 0x9C, 0x2F, 0x6C, 0xD0, 0xEF, 0x0F};
	const uint8_t *data = frame + 5 + 25;
	uint8_t dch[25];
	for (unsigned i = 0; i < 5u; i++)
		memcpy(dch + 5 * i, data + 18 * i, 5);
	CYSFConvolution conv;
	conv.start();
	for (unsigned i = 0; i < 100u; i++) {
		unsigned n = (i / 5u) * 2u + (i % 5u) * 40u;	// INTERLEAVE_TABLE_5_20
		conv.decode((uint8_t)getb(dch, n), (uint8_t)getb(dch, n + 1u));
	}
	unsigned char output[13];
	conv.chainback(output, 96U);
	if (!CCRC::checkCCITT162(output, 12U))
		return false;
	for (unsigned i = 0; i < 10u; i++)
		out10[i] = output[i] ^ W[i];
	return true;
}

int main()
{
	// V/D mode 2 data channel: callsigns both ways, and with errors
	for (unsigned k = 0; k < 300u; k++) {
		uint8_t frame[120] = {0}, csd[10], got[10], ref[10];
		for (unsigned i = 0; i < 10u; i++) csd[i] = (uint8_t)('A' + rnd() % 26u);
		ysf_vd2_data_put(csd, frame);
		CHECK(oracle_dch(frame, ref) && memcmp(ref, csd, 10) == 0);
		CHECK(ysf_vd2_data(frame, got) && memcmp(got, csd, 10) == 0);
		for (unsigned e = 0; e < 3u; e++) {
			unsigned piece = rnd() % 5u, bit = rnd() % 40u;
			flip(frame, 240u + piece * 144u + bit);
		}
		CHECK(ysf_vd2_data(frame, got) && memcmp(got, csd, 10) == 0);	// the reference's Viterbi may give up on some
	}

	// Golay: every data word against the reference table, then 3 random errors corrected
	for (unsigned d = 0; d < 4096u; d++) {
		CHECK(ysf_golay24_encode((uint16_t)d) == CGolay24128::encode24128(d));
		uint32_t w = ysf_golay24_encode((uint16_t)d);
		for (unsigned e = 0; e < 3u; e++)
			w ^= 1u << (rnd() % 24u);
		uint16_t got = 0;
		CHECK(ysf_golay24_decode(w, &got) >= 0 && got == d);
	}

	// FICH both ways, clean and with channel bit errors the code corrects
	for (unsigned k = 0; k < 300u; k++) {
		uint8_t info[4] = {(uint8_t)rnd(), (uint8_t)rnd(), (uint8_t)rnd(), (uint8_t)rnd()};
		// reference encodes, we decode
		CYSFFICH ref;
		ref.setFI(info[0] >> 6); ref.setBN(info[0] & 3); ref.setBT(info[1] >> 6); ref.setFN((info[1] >> 3) & 7);
		ref.setFT(info[1] & 7); ref.setMR((info[2] >> 3) & 3); ref.setDGId(info[3] & 0x7F);
		ref.setDev((info[2] & 0x40) != 0); ref.setVoIP((info[2] & 0x04) != 0);
		uint8_t frame[120] = {0xD4, 0x71, 0xC9, 0x63, 0x4D};
		ref.encode(frame);
		ysf_fich_t f;
		CHECK(ysf_fich_decode(frame, &f));
		CHECK(f.fi == ref.getFI() && f.bn == ref.getBN() && f.bt == ref.getBT() && f.fn == ref.getFN() &&
		      f.ft == ref.getFT() && f.mr == ref.getMR() && f.dgid == ref.getDGId() && f.dev == ref.getDev() &&
		      f.dt == ref.getDT() && f.cm == ref.getCM());
		// we encode, the reference decodes to the same 4 bytes
		uint8_t mine[120] = {0};
		uint8_t raw[4];
		memcpy(raw, f.raw, 4);
		ysf_fich_encode(raw, mine);
		CHECK(memcmp(mine, frame, 30) == 0);	// bit for bit the same sync and FICH
		CYSFFICH back;
		CHECK(back.decode(mine) && back.getFN() == f.fn && back.getDGId() == f.dgid);
		// a few errors in the FICH (the conv + Golay code takes them)
		for (unsigned e = 0; e < 4u; e++)
			flip(mine, 40u + rnd() % 200u);
		ysf_fich_t g;
		CHECK(ysf_fich_decode(mine, &g) && memcmp(g.raw, f.raw, 4) == 0);
	}

	// V/D mode 2 voice: our 49 parameter bits equal the converter's a, b, c words; one error per triplet is voted out
	for (unsigned k = 0; k < 200u; k++) {
		uint8_t frame[120] = {0};
		uint8_t params[5][49];
		for (unsigned j = 0; j < 5u; j++) {
			for (unsigned i = 0; i < 49u; i++)
				params[j][i] = (uint8_t)(rnd() & 1u);
			ysf_vd2_voice_put(params[j], j, frame);
		}
		for (unsigned j = 0; j < 5u; j++) {
			unsigned a, b, c;
			oracle_vd2(frame, j, a, b, c);
			unsigned pa = 0, pb = 0, pc = 0;
			for (unsigned i = 0; i < 12u; i++) pa = (pa << 1) | params[j][i];
			for (unsigned i = 0; i < 12u; i++) pb = (pb << 1) | params[j][12 + i];
			for (unsigned i = 0; i < 25u; i++) pc = (pc << 1) | params[j][24 + i];
			CHECK(a == pa && b == pb && c == pc);
			uint8_t got[49];
			CHECK(ysf_vd2_voice(frame, j, got) == 0 && memcmp(got, params[j], 49) == 0);
		}
	}
	printf("Plus YSF vs MMDVMHost/MMDVM_CM: Golay table, FICH both ways and with errors, V/D2 voice: %d failures\n",
	       failures);
	return failures ? 1 : 0;
}
