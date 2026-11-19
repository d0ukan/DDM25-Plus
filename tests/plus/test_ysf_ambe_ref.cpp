// SPDX-License-Identifier: GPL-3.0-or-later
// DM32 Plus YSF DN -> DMR vocoder path against MMDVM_CM YSF2DMR (EA7EE, GPL-2.0-or-later), host only: a V/D mode 2
// voice channel read by src/ysf and framed by RadioFoundry's AMBE FEC (ambeFecEncode) must give exactly the 72-bit
// DMR AMBE frame YSF2DMR's putAMBE2DMR builds from the same channel. The reference tables come from ModeConv.cpp at
// test time (ysf_ref_tables.h, generated; not in this tree).
#include <cstdio>
#include <cstring>
#include "Golay24128.h"
#include "ysf_ref_tables.h"	// PRNG_TABLE, DMR_A_TABLE, DMR_B_TABLE, DMR_C_TABLE
extern "C" {
#include "ysf/ysf.h"
#include "dmr/ambe/ambeFec.h"
}

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static uint32_t rng = 777u;
static uint32_t rnd() { rng = rng * 1103515245u + 12345u; return rng >> 8; }
static void wbit(uint8_t *p, unsigned i, bool b)
{
	uint8_t m = (uint8_t)(0x80u >> (i & 7u));
	p[i >> 3] = b ? (uint8_t)(p[i >> 3] | m) : (uint8_t)(p[i >> 3] & ~m);
}

// CModeConv::putAMBE2DMR (the oracle)
static void oracle_dmr(unsigned dat_a, unsigned dat_b, unsigned dat_c, uint8_t v_dmr[9])
{
	memset(v_dmr, 0, 9);
	unsigned a = CGolay24128::encode24128(dat_a);
	unsigned p = PRNG_TABLE[dat_a] >> 1;
	unsigned b = CGolay24128::encode23127(dat_b) >> 1;
	b ^= p;
	unsigned mask = 0x800000u;
	for (unsigned i = 0; i < 24u; i++, mask >>= 1) wbit(v_dmr, DMR_A_TABLE[i], a & mask);
	mask = 0x400000u;
	for (unsigned i = 0; i < 23u; i++, mask >>= 1) wbit(v_dmr, DMR_B_TABLE[i], b & mask);
	mask = 0x1000000u;
	for (unsigned i = 0; i < 25u; i++, mask >>= 1) wbit(v_dmr, DMR_C_TABLE[i], dat_c & mask);
}

int main()
{
	for (unsigned k = 0; k < 2000u; k++) {
		uint8_t params[49], frame[120] = {0}, got[49];
		for (unsigned i = 0; i < 49u; i++) params[i] = (uint8_t)(rnd() & 1u);
		unsigned j = k % 5u;
		ysf_vd2_voice_put(params, j, frame);
		CHECK(ysf_vd2_voice(frame, j, got) == 0);
		unsigned a = 0, b = 0, c = 0;
		for (unsigned i = 0; i < 12u; i++) a = (a << 1) | got[i];
		for (unsigned i = 0; i < 12u; i++) b = (b << 1) | got[12 + i];
		for (unsigned i = 0; i < 25u; i++) c = (c << 1) | got[24 + i];
		uint8_t ref[9], ours[9];
		oracle_dmr(a, b, c, ref);
		ambeFecEncode(got, ours);
		CHECK(memcmp(ref, ours, 9) == 0);
	}
	std::printf("Plus YSF DN -> DMR AMBE frame vs YSF2DMR: %d failures\n", failures);
	return failures ? 1 : 0;
}
