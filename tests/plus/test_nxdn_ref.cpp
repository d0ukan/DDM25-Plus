// SPDX-License-Identifier: GPL-3.0-or-later
// DM32 Plus NXDN48 receive (src/nxdn) against the reference implementations, compiled on the host only
// (tools/test_nxdn_ref.sh):
//  - MMDVMHost (Jonathan Naylor G4KLX, GPL-2.0-or-later): CNXDNLICH, CNXDNSACCH, CNXDNFACCH1, CNXDNLayer3, CNXDNAudio,
//    the scrambler and the AMBE PRNG tables (NXDNControl.cpp, NXDNAudio.cpp)
//  - RadioFoundry ambeFec (MIT): the DMR AMBE+2 air frame the radio's vocoder path takes
// MMDVMHost encodes, we decode: every LICH, SACCH and FACCH1 with channel errors (both decoders' success counted),
// layer 3 fields, the voice fields both ways against CNXDNAudio and ambeFec, and whole MMDVMHost-built calls through
// our frame layer. The reference sources are not in this tree: the script compiles them from research/ref.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "NXDNLICH.h"
#include "NXDNSACCH.h"
#include "NXDNFACCH1.h"
#include "NXDNLayer3.h"
#include "NXDNAudio.h"
#include "NXDNDefines.h"
#include "nxdn_ref_tables.h"	// SCRAMBLER and PRNG_TABLE, extracted by the script
extern "C" {
#include "nxdn/nxdn.h"
#include "nxdn/nxdn_rx.h"
#include "dmr/ambe/ambeFec.h"
}

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint32_t rng = 0xC0FFEE11u;
static uint32_t rnd() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static void flip(uint8_t *p, unsigned bit) { p[bit >> 3] ^= (uint8_t)(0x80u >> (bit & 7u)); }
static unsigned getb(const uint8_t *p, unsigned i) { return (p[i >> 3] >> (7u - (i & 7u))) & 1u; }
static void setb(uint8_t *p, unsigned i, unsigned v)
{
	if (v)
		p[i >> 3] |= (uint8_t)(0x80u >> (i & 7u));
	else
		p[i >> 3] &= (uint8_t)~(0x80u >> (i & 7u));
}

// n distinct random bit positions in [from, from + span)
static void flips(uint8_t *f, unsigned from, unsigned span, unsigned n)
{
	uint8_t used[32] = { 0 };
	for (unsigned e = 0; e < n;) {
		unsigned b = rnd() % span;
		if (!getb(used, b)) {
			setb(used, b, 1);
			flip(f, from + b);
			e++;
		}
	}
}

static void put_fsw(uint8_t *f)
{
	for (unsigned i = 0; i < 20u; i++)
		setb(f, i, (NXDN_FSW >> (19u - i)) & 1u);
}

// --- a call built by MMDVMHost's classes, fed to our frame layer
struct got_t {
	uint8_t slots[64][9];
	unsigned n, calls, ends;
	nxdn_call_msg_t last;
};
static void on_voice(void *ctx, const uint8_t s[9])
{
	got_t *g = (got_t *)ctx;
	if (g->n < 64u)
		std::memcpy(g->slots[g->n], s, 9);
	g->n++;
}
static void on_event(void *ctx, unsigned ev, const nxdn_call_msg_t *m)
{
	got_t *g = (got_t *)ctx;
	if (ev == NXDN_EV_CALL) {
		g->calls++;
		g->last = *m;
	} else {
		g->ends++;
	}
}

static void push_frame(nxdn_rx_t *rx, uint8_t *f)
{
	for (unsigned i = 0; i < 48u; i++)
		f[i] ^= SCRAMBLER[i];	// MMDVMHost's own table, not ours
	for (unsigned i = 0; i < 192u; i++)
		nxdn_rx_push_dibit(rx, (uint8_t)((f[i / 4u] >> (6u - 2u * (i % 4u))) & 3u));
}

static void mmdvm_control(uint8_t *f, unsigned ran, const uint8_t l3[10])
{
	std::memset(f, 0, 48);
	put_fsw(f);
	CNXDNLICH lich;
	lich.setRFCT(NXDN_LICH_RFCT_RDCH);
	lich.setFCT(NXDN_LICH_USC_SACCH_NS);
	lich.setOption(NXDN_LICH_STEAL_FACCH);
	lich.setDirection(NXDN_LICH_DIRECTION_INBOUND);
	lich.encode(f);
	CNXDNSACCH sacch;
	sacch.setRAN((unsigned char)ran);
	sacch.setStructure(NXDN_SR_SINGLE);
	sacch.setData(SACCH_IDLE);	// as NXDNControl.cpp
	sacch.encode(f);
	CNXDNFACCH1 facch;
	facch.setData(l3);
	facch.encode(f, NXDN_FSW_LENGTH_BITS + NXDN_LICH_LENGTH_BITS + NXDN_SACCH_LENGTH_BITS);
	facch.encode(f, NXDN_FSW_LENGTH_BITS + NXDN_LICH_LENGTH_BITS + NXDN_SACCH_LENGTH_BITS + NXDN_FACCH1_LENGTH_BITS);
}

int main()
{
	// Scrambler and the C1 whitening
	CHECK(std::memcmp(nxdn_scrambler, SCRAMBLER, 48) == 0);
	for (unsigned a = 0; a < 4096u; a++)
		CHECK(nxdn_ambe_c1_mask((uint16_t)a) == (PRNG_TABLE[a] >> 1));

	// LICH: every RDCH value MMDVMHost builds, decoded by us (MMDVMHost's parity rule and ours agree on RDCH)
	for (unsigned v = 0; v < 32u; v++) {
		uint8_t f[48] = { 0 };
		CNXDNLICH m;
		m.setRFCT(NXDN_LICH_RFCT_RDCH);
		m.setFCT((unsigned char)(v >> 3));
		m.setOption((unsigned char)((v >> 1) & 3u));
		m.setDirection((unsigned char)(v & 1u));
		m.encode(f);
		nxdn_lich_t l;
		unsigned off = 9;
		CHECK(nxdn_lich_unpack(nxdn_lich_get(f, &off), &l) && off == 0);
		CHECK(l.rfct == NXDN_RFCT_RDCH && l.fct == (v >> 3) && l.option == ((v >> 1) & 3u) && l.direction == (v & 1u));
	}

	// SACCH and FACCH1: MMDVMHost encodes, both decode through 0..4 / 0..6 channel errors
	uint16_t surv[NXDN_VIT_STEPS];
	unsigned s_ours[5] = { 0 }, s_theirs[5] = { 0 }, f_ours[7] = { 0 }, f_theirs[7] = { 0 }, ours_only = 0,
		 theirs_only = 0, wrong = 0;
	for (unsigned it = 0; it < 7000u; it++) {
		uint8_t f[48] = { 0 };
		unsigned ran = rnd() & 63u, st = rnd() & 3u, data = rnd() & 0x3FFFFu;
		CNXDNSACCH ms;
		ms.setRAN((unsigned char)ran);
		ms.setStructure((unsigned char)st);
		unsigned char d3[3] = { (unsigned char)(data >> 10), (unsigned char)(data >> 2), (unsigned char)(data << 6) };
		ms.setData(d3);
		ms.encode(f);
		uint8_t l3[10];
		for (unsigned i = 0; i < 10u; i++)
			l3[i] = (uint8_t)rnd();
		unsigned which = it & 1u;
		CNXDNFACCH1 mf;
		mf.setData(l3);
		mf.encode(f, 96u + which * 144u);
		unsigned es = it % 5u, ef = (it / 5u) % 7u;
		flips(f, 36u, 60u, es);
		flips(f, 96u + which * 144u, 144u, ef);

		nxdn_sacch_t s;
		bool ok = nxdn_sacch_decode(f, &s, NULL, surv);
		bool good = ok && s.ran == ran && s.structure == st && s.data == data;
		wrong += ok && !good;
		CNXDNSACCH md;
		bool mok = md.decode(f);
		unsigned char got3[3] = { 0, 0, 0 };
		md.getData(got3);
		bool mgood = mok && md.getRAN() == ran && md.getStructure() == st &&
			     (unsigned)(got3[0] << 10 | got3[1] << 2 | got3[2] >> 6) == data;
		s_ours[es] += good;
		s_theirs[es] += mgood;
		ours_only += good && !mgood;
		theirs_only += mgood && !good;

		uint8_t out[10];
		ok = nxdn_facch1_decode(f, which, out, NULL, surv);
		good = ok && std::memcmp(out, l3, 10) == 0;
		wrong += ok && !good;
		CNXDNFACCH1 fd;
		mok = fd.decode(f, 96u + which * 144u);
		unsigned char mout[10];
		fd.getData(mout);
		mgood = mok && std::memcmp(mout, l3, 10) == 0;
		f_ours[ef] += good;
		f_theirs[ef] += mgood;
		ours_only += good && !mgood;
		theirs_only += mgood && !good;
	}
	std::printf("SACCH by errors 0-4, ours/MMDVMHost of 1400:");
	for (unsigned e = 0; e < 5u; e++)
		std::printf(" %u/%u", s_ours[e], s_theirs[e]);
	std::printf("\nFACCH1 by errors 0-6, ours/MMDVMHost of 1000:");
	for (unsigned e = 0; e < 7u; e++)
		std::printf(" %u/%u", f_ours[e], f_theirs[e]);
	std::printf("\nonly ours decoded %u, only MMDVMHost %u, ours wrong with a good CRC %u\n", ours_only, theirs_only,
		    wrong);
	CHECK(s_ours[0] == 1400u && s_ours[1] == 1400u && f_ours[0] == 1000u && f_ours[1] == 1000u);
	// A 6-bit CRC passes 1 in 64 of the SACCHs past the code's reach: a few wrong ones are the code's, not ours
	CHECK(wrong <= 5u);
	// The same code: we must decode at least as often as the reference (ours treats punctured bits as erasures)
	CHECK(theirs_only <= ours_only);

	// Layer 3 fields
	for (unsigned it = 0; it < 2000u; it++) {
		uint8_t d[10];
		for (unsigned i = 0; i < 10u; i++)
			d[i] = (uint8_t)rnd();
		d[0] = (uint8_t)((d[0] & 0xC0u) | (it & 1u ? NXDN_MSG_VCALL : NXDN_MSG_TX_REL));
		nxdn_call_msg_t m;
		CHECK(nxdn_layer3_decode(d, &m));
		CNXDNLayer3 l3;
		l3.decode(d, 80u);
		CHECK(m.type == l3.getMessageType() && m.source == l3.getSourceUnitId() &&
		      m.destination == l3.getDestinationGroupId() && (m.call_type < 4u) == l3.getIsGroup());
	}

	// Voice: CNXDNAudio encodes two fields; our 49 bits equal its input; ambeFecEncode gives the same 72 bits (the
	// NXDN field is the DMR AMBE+2 air frame); with errors, ours and ambeFecDecode agree on the bits and the count
	CNXDNAudio audio;
	unsigned fec_cases = 0;
	for (unsigned it = 0; it < 3000u; it++) {
		uint8_t in[13] = { 0 }, out[18] = { 0 }, p[2][49];
		for (unsigned k = 0; k < 2u; k++)
			for (unsigned i = 0; i < 49u; i++) {
				p[k][i] = (uint8_t)(rnd() & 1u);
				setb(in, k * 49u + i, p[k][i]);
			}
		audio.encode(in, out);
		for (unsigned k = 0; k < 2u; k++) {
			uint8_t q[49], fec[9];
			CHECK(nxdn_ambe_params(out + 9u * k, q) == 0 && std::memcmp(q, p[k], 49) == 0);
			ambeFecEncode(p[k], fec);
			CHECK(std::memcmp(fec, out + 9u * k, 9) == 0);
			uint8_t back[13] = { 0 };
			audio.decode(out, back);
			for (unsigned i = 0; i < 49u; i++)
				CHECK(getb(back, k * 49u + i) == p[k][i]);
			// up to 3 errors in C0 and in C1 (by sequence position), any in C2/C3
			uint8_t bad[9];
			std::memcpy(bad, out + 9u * k, 9);
			unsigned e0 = rnd() % 4u, e1 = rnd() % 4u, e2 = rnd() % 3u;
			uint32_t m0 = 0, m1 = 0, m2 = 0;
			while ((unsigned)__builtin_popcount(m0) < e0) m0 |= 1u << (rnd() % 24u);
			while ((unsigned)__builtin_popcount(m1) < e1) m1 |= 1u << (rnd() % 23u);
			while ((unsigned)__builtin_popcount(m2) < e2) m2 |= 1u << (rnd() % 25u);
			for (unsigned n = 0; n < 72u; n++) {
				bool hit = n < 24u ? (m0 >> n) & 1u : n < 47u ? (m1 >> (n - 24u)) & 1u : (m2 >> (n - 47u)) & 1u;
				if (hit)
					flip(bad, ambeAirBitOfSeq((uint8_t)n));
			}
			uint8_t rq[49];
			ambe_fec_result_t res;
			int theirs = ambeFecDecode(bad, rq, &res);
			int ours = nxdn_ambe_params(bad, q);
			CHECK(ours == (int)(e0 + e1) && theirs == ours);
			CHECK(std::memcmp(q, rq, 49) == 0);
			fec_cases++;
		}
	}
	// 4 errors in C0: both call the frame lost
	for (unsigned it = 0; it < 500u; it++) {
		uint8_t p[49], fr[9], q[49];
		for (unsigned i = 0; i < 49u; i++)
			p[i] = (uint8_t)(rnd() & 1u);
		ambeFecEncode(p, fr);
		uint32_t m0 = 0;
		while (__builtin_popcount(m0) < 4)
			m0 |= 1u << (rnd() % 24u);
		for (unsigned n = 0; n < 24u; n++)
			if ((m0 >> n) & 1u)
				flip(fr, ambeAirBitOfSeq((uint8_t)n));
		ambe_fec_result_t res;
		ambeFecDecode(fr, q, &res);
		CHECK(nxdn_ambe_params(fr, q) == -1 && res.c0_failed);
	}

	// Whole calls built with MMDVMHost's classes (as NXDNControl does on RF), through our frame layer: header,
	// eight voice frames with the VCALL in the SACCH superframe, release; then a late-entry pass without the header
	for (unsigned pass = 0; pass < 2u; pass++) {
		got_t g;
		std::memset(&g, 0, sizeof g);
		nxdn_rx_cfg_t cfg = { 0, 0, 0, on_voice, on_event, &g };
		static nxdn_rx_t rx;
		nxdn_rx_init(&rx, &cfg);
		uint8_t vcall[10] = { NXDN_MESSAGE_TYPE_VCALL, 0, 0x20, 0x30, 0x39, 0x00, 0x65, 0, 0, 0 };	// 12345 > TG 101
		uint8_t rel[10];
		std::memcpy(rel, vcall, 10);
		rel[0] = NXDN_MESSAGE_TYPE_TX_REL;
		uint8_t f[48], p[64][49];
		unsigned sent = 0;
		if (pass == 0) {
			mmdvm_control(f, 12, vcall);
			push_frame(&rx, f);
		}
		for (unsigned v = 0; v < 8u; v++) {
			std::memset(f, 0, 48);
			put_fsw(f);
			CNXDNLICH lich;
			lich.setRFCT(NXDN_LICH_RFCT_RDCH);
			lich.setFCT(NXDN_LICH_USC_SACCH_SS);
			lich.setOption(NXDN_LICH_STEAL_NONE);
			lich.setDirection(NXDN_LICH_DIRECTION_INBOUND);
			lich.encode(f);
			CNXDNSACCH sacch;
			sacch.setRAN(12);
			static const unsigned char SR[4] = { NXDN_SR_1_4, NXDN_SR_2_4, NXDN_SR_3_4, NXDN_SR_4_4 };
			sacch.setStructure(SR[v & 3u]);
			CNXDNLayer3 l3;
			l3.decode(vcall, 80u);
			unsigned char part[3] = { 0, 0, 0 };
			l3.encode(part, 18u, 18u * (v & 3u));
			sacch.setData(part);
			sacch.encode(f);
			for (unsigned h = 0; h < 2u; h++) {
				uint8_t in[13] = { 0 }, out[18];
				for (unsigned k = 0; k < 2u; k++)
					for (unsigned i = 0; i < 49u; i++) {
						p[sent + k][i] = (uint8_t)(rnd() & 1u);
						setb(in, k * 49u + i, p[sent + k][i]);
					}
				audio.encode(in, out);
				for (unsigned i = 0; i < 144u; i++)
					setb(f, 96u + h * 144u + i, getb(out, i));
				sent += 2u;
			}
			push_frame(&rx, f);
		}
		mmdvm_control(f, 12, rel);
		push_frame(&rx, f);
		unsigned skip = pass == 0 ? 0u : 12u;	// late entry: the caller after the fourth voice frame, played from it
		std::printf("MMDVMHost call pass %u: fields %u, calls %u, ends %u, src %u dst %u, last RAN %u\n", pass, g.n,
			    g.calls, g.ends, g.last.source, g.last.destination, rx.last_ran);
		CHECK(g.calls == 1 && g.ends == 1 && g.last.source == 12345 && g.last.destination == 101 && rx.last_ran == 12);
		CHECK(g.n == sent - skip);
		for (unsigned i = 0; i < g.n && i < 64u; i++) {
			uint8_t q[49];
			CHECK(nxdn_ambe_params(g.slots[i], q) == 0 && std::memcmp(q, p[skip + i], 49) == 0);
		}
	}

	if (failures) {
		std::printf("test_nxdn_ref: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("test_nxdn_ref: all passed (%u FEC cases)\n", fec_cases);
	return 0;
}
