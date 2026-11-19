// SPDX-License-Identifier: GPL-3.0-or-later
// DM32 Plus P25 extras against the reference: MMDVMHost's own LDU1 link control coder (CP25Data, GPL-2.0-or-later,
// compiled from the reference tree by tools/test_p25lc_ref.sh, never copied into this tree; p25lc_ref_wrap.cpp) and
// ours (src/p25/p25.c with CONFIG_P25_EXTRAS=1, tests/plus/p25_lc_build.h) on the same frames, both ways:
//  - frames from our encoder (group voice, the emergency bit) and our builder (unit to unit) decode in MMDVMHost to
//    the same LCF, emergency flag, destination (talkgroup / called unit) and source;
//  - MMDVMHost's encodeLDU1 written over our frames (group with and without emergency, unit to unit) decodes in ours
//    to the same fields, with no correction needed.
#include <cstdio>
#include <cstring>
#include <cstdint>
#include "p25.h"
#include "p25_lc_build.h"
#include "p25lc_ref_wrap.h"

static unsigned failures;
#define CHECK(c) do { if (!(c)) { failures++; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint32_t rng = 0x1234567u;
static uint32_t rnd() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static void our_ldu1(uint8_t f[P25_LDU_BYTES], uint8_t svc, uint16_t tg, uint32_t src)
{
	p25_call call;
	std::memset(&call, 0, sizeof call);
	call.nac = 0x293;
	call.talkgroup = tg;
	call.source_id = src;
	call.service_options = svc;
	uint16_t imbe[P25_IMBE_FRAMES][8];
	const uint8_t lsd[2] = { 0, 0 };
	for (unsigned i = 0; i < P25_IMBE_FRAMES; i++)
		for (unsigned j = 0; j < 8u; j++)
			imbe[i][j] = (uint16_t)(rnd() & ((1u << (j < 4 ? 12 : j < 7 ? 11 : 7)) - 1u));
	CHECK(!p25_encode_ldu(&call, P25_DUID_LDU1, imbe, lsd, f));
}

int main()
{
	unsigned rounds = 500, ours_to_ref = 0, ref_to_ours = 0;
	for (unsigned r = 0; r < rounds; r++) {
		uint8_t f[P25_LDU_BYTES], lc[9];
		bool unit = r % 3u == 2u, emergency = r % 2u == 1u;
		uint16_t tg = (uint16_t)(1u + rnd() % 65535u);
		uint32_t target = 1u + rnd() % 0xFFFFFEu, src = 1u + rnd() % 0xFFFFFEu;
		// ours -> MMDVMHost (it reads no emergency bit in a unit-to-unit LC)
		our_ldu1(f, emergency ? 0x80 : 0, tg, src);
		if (unit) {
			lc_unit(lc, 0, target, src);
			ldu1_set_lc(f, lc);
		}
		ref_lc ref;
		bool ok = ref_decode_ldu1(f, &ref) && ref.lcf == (unit ? ref_lcf_private() : ref_lcf_group()) &&
			  ref.src == src && ref.dst == (unit ? target : tg) && ref.mfid == 0 &&
			  ref.emergency == (!unit && emergency);
		CHECK(ok);
		ours_to_ref += ok;
		// MMDVMHost -> ours: its link control over one of our frames (voice, NID kept)
		our_ldu1(f, 0, 1, 1);
		ref.lcf = unit ? ref_lcf_private() : ref_lcf_group();
		ref.mfid = 0;
		ref.emergency = !unit && emergency;
		ref.src = src;
		ref.dst = unit ? target : tg;
		ref_encode_ldu1(f, &ref);
		p25_decoded d;
		ok = !p25_decode_frame(f, P25_LDU_BYTES, &d) && d.metadata_valid &&
		     d.link_control_format == (unit ? 3 : 0) && d.source_id == src &&
		     (unit ? d.target_id == target && d.talkgroup == 0 : d.talkgroup == tg && d.target_id == 0) &&
		     ((d.service_options & 0x80) != 0) == (!unit && emergency) && d.metadata_corrected_symbols == 0 &&
		     d.metadata_corrected_bits == 0;
		CHECK(ok);
		ref_to_ours += ok;
	}
	std::printf("%s P25 LC against MMDVMHost CP25Data: ours -> reference %u/%u, reference -> ours %u/%u (group, "
		    "emergency, unit to unit)\n", failures ? "FAIL" : "ok  ", ours_to_ref, rounds, ref_to_ours, rounds);
	return failures ? 1 : 0;
}
