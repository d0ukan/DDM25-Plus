/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus P25 trunk following (docs/plus.md): the TSBK decoder (trunk/p25_tsbk.c) against the test's own encoder
 * (p25_tsdu_build.h) and a second, greedy hard-decision decoder written here; TSDU framing by the pinned CAI
 * library's control channel receiver (third_party/p25cai, as src/p25/voice.c runs it); the channel plan (IDEN_UP / IDEN_UP_VU / IDEN_UP_TDMA to Hz); grant parsing; and the follower's
 * state machine (radio/p25trunk.c): grant -> retune -> voice -> end -> back, other groups, encrypted, Phase 2,
 * unknown plan, out of band, hold-offs, a lost control channel, every group, the idle stop. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "p25.h"
#include "p25_tsbk.h"
#include "radio/p25trunk.h"
#include "p25_tsdu_build.h"

static unsigned failures;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint32_t rng = 0x2468ACE1u;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

/* DM32 radio bands (VHF, UHF) for the follower */
bool p25tk_band_ok(uint32_t hz) { return (hz >= 136000000u && hz <= 174000000u) || (hz >= 400000000u && hz <= 480000000u); }

/* ---- TSBK coding ---- */

/* A second decoder for cross-checking: the CRC as a 17-bit register over all 12 octets (zero for a good block), and
 * the trellis greedily (per symbol, the input whose output is nearest), as simple decoders do */
static int crc_register_ok(const uint8_t b[12])
{
	uint32_t reg = 0;
	for (unsigned i = 0; i < 96u; i++) {
		reg = ((reg << 1) | ((b[i >> 3] >> (7u - (i & 7u))) & 1u)) & 0x1FFFFu;
		if (reg & 0x10000u)
			reg = (reg & 0xFFFFu) ^ 0x1021u;
	}
	return (reg ^ 0xFFFFu) == 0;
}
static int greedy_decode(const uint8_t air[25], uint8_t out[12])
{
	/* deinterleave: coded pair p was sent at position (p % 4 row, p / 4 column) */
	static const uint8_t row_start[4] = { 0, 13, 25, 37 };
	unsigned state = 0;
	memset(out, 0, 12);
	for (unsigned p = 0; p < 49u; p++) {
		unsigned at = 4u * (row_start[p & 3u] + p / 4u), rx = 0;
		for (unsigned j = 0; j < 4u; j++)
			rx = rx << 1 | tsdu_getb(air, at + j);
		unsigned best = 99, pick = 0, ties = 0;
		for (unsigned in = 0; in < 4u; in++) {
			const int8_t *s = tsdu_symbols[tsdu_point[state][in]];
			unsigned cw = tsdu_dibit_of(s[0]) << 2 | tsdu_dibit_of(s[1]), d = 0;
			for (unsigned x = rx ^ cw; x; x &= x - 1u)
				d++;
			if (d < best) { best = d; pick = in; ties = 0; } else if (d == best) ties++;
		}
		if (ties)
			return 0;
		state = pick;
		if (p < 48u)
			out[p >> 2] |= (uint8_t)(pick << (6u - 2u * (p & 3u)));
	}
	return crc_register_ok(out);
}

static void random_tsbk(uint8_t b[12])
{
	for (unsigned i = 0; i < 10u; i++)
		b[i] = (uint8_t)rnd();
	tsbk_seal(b);
}

static void test_crc(void)
{
	/* CRC-16/GSM catalogue value for "123456789" is CE3C; ours runs over 10 octets: "1234567890" -> 2CDE */
	const uint8_t digits[10] = { '1', '2', '3', '4', '5', '6', '7', '8', '9', '0' };
	CHECK(p25_tsbk_crc(digits) == 0x2CDEu);
	uint8_t b[12] = { 0x3D, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 };
	CHECK(p25_tsbk_crc(b) == 0xB6D4u);
	tsbk_seal(b);
	CHECK(b[10] == 0xB6 && b[11] == 0xD4 && crc_register_ok(b));
	for (unsigned t = 0; t < 200u; t++) {
		random_tsbk(b);
		CHECK(crc_register_ok(b) && p25_tsbk_crc(b) == (uint16_t)(b[10] << 8 | b[11]));
		b[rnd() % 12u] ^= (uint8_t)(1u << (rnd() % 8u));
		CHECK(!crc_register_ok(b));
	}
	printf("ok   TSBK CRC (CRC-16/GSM vectors, register form agrees, single-bit errors caught)\n");
}

/* One block put in a frame buffer at block n, as the framer holds it */
static void block_in_frame(const uint8_t b[12], uint8_t frame[TSDU_MAX_BYTES], unsigned n)
{
	uint8_t air[25];
	tsbk_air(b, air);
	for (unsigned i = 0; i < 196u; i++)
		tsdu_putb(frame, tsdu_physical(112u + 196u * n + i), tsdu_getb(air, i));
}

static void test_trellis(void)
{
	uint8_t b[12], out[12], frame[TSDU_MAX_BYTES], bits;
	unsigned corrected = 0, failed_heavy = 0;
	for (unsigned t = 0; t < 300u; t++) {
		random_tsbk(b);
		unsigned n = t % 3u;
		memset(frame, 0xA5, sizeof frame);
		block_in_frame(b, frame, n);
		CHECK(p25_tsbk_decode(frame, n, out, &bits) && !memcmp(out, b, 12) && bits == 0);
		uint8_t air[25], again[12];
		tsbk_air(b, air);
		CHECK(greedy_decode(air, again) && !memcmp(again, b, 12));
		/* A few scattered bit errors (one per 4-bit symbol at most, far apart): the Viterbi path corrects them */
		unsigned errs = 1u + t % 4u;
		for (unsigned e = 0; e < errs; e++) {
			unsigned sym = (e * 12u + t) % 49u, j = rnd() % 4u;
			static const uint8_t row_start[4] = { 0, 13, 25, 37 };
			unsigned at = 4u * (row_start[sym & 3u] + sym / 4u) + j;
			unsigned p = tsdu_physical(112u + 196u * n + at);
			frame[p >> 3] ^= (uint8_t)(0x80u >> (p & 7u));
		}
		if (p25_tsbk_decode(frame, n, out, &bits) && !memcmp(out, b, 12) && bits == errs)
			corrected++;
		/* Many more errors: corrected or rejected, never a wrong block with a good CRC */
		for (unsigned i = 0; i < 25u; i++) {
			unsigned p = tsdu_physical(112u + 196u * n + rnd() % 196u);
			frame[p >> 3] ^= (uint8_t)(0x80u >> (p & 7u));
		}
		if (p25_tsbk_decode(frame, n, out, NULL))
			CHECK(!memcmp(out, b, 12));
		else
			failed_heavy++;
	}
	CHECK(corrected == 300u);
	/* Pure noise: the CRC (1 in 65536) keeps it out */
	unsigned accepted = 0;
	for (unsigned t = 0; t < 2000u; t++) {
		for (unsigned i = 0; i < sizeof frame; i++)
			frame[i] = (uint8_t)rnd();
		accepted += (unsigned)p25_tsbk_decode(frame, t % 3u, out, NULL);
	}
	CHECK(accepted == 0);
	printf("ok   TSBK trellis: 300 blocks decode in every position, 1-4 scattered errors corrected (%u), 25 errors "
	       "rejected (%u/300) or corrected, noise never accepted (2000)\n", corrected, failed_heavy);
}

/* ---- TSDU framing: the pinned CAI library's control channel receiver, as voice.c runs it (Classic r13 base) ---- */

/* The frame's dibits, then pad dibits of noise (the control channel goes on: the receiver completes a TSDU at its
 * microslot boundary). Returns the TSDUs completed; their TSBKs into got */
static unsigned push_frame(p25cai_control_receiver *rx, const uint8_t *frame, unsigned dibits, unsigned pad,
			   uint8_t got[][12], unsigned *ngot)
{
	unsigned units = 0;
	p25cai_control u;
	for (unsigned i = 0; i < dibits + pad; i++) {
		unsigned d = i < dibits ? (tsdu_getb(frame, 2u * i) << 1 | tsdu_getb(frame, 2u * i + 1u)) : (rnd() & 3u);
		if (p25cai_control_receiver_push(rx, (uint8_t)d, &u) == 1) {
			units++;
			CHECK(u.duid == P25CAI_TSDU && u.count >= 1u && u.count <= 3u && u.nac == 0x1C8);
			for (unsigned k = 0; k < u.count; k++)
				memcpy(got[(*ngot)++], u.block[k], 12);
		}
	}
	return units;
}
static void push_noise(p25cai_control_receiver *rx, unsigned n)
{
	p25cai_control u;
	while (n--)
		CHECK(p25cai_control_receiver_push(rx, (uint8_t)(rnd() & 3u), &u) != 1);
}

static void test_framer(void)
{
	p25cai_control_receiver rx;
	uint8_t frame[TSDU_MAX_BYTES], blocks[3][12], got[16][12];
	unsigned ngot = 0;
	CHECK(p25cai_control_receiver_init(&rx, 0x1C8, 196u) == 0);
	/* One block (last-block flag set), then three, then one: back to back */
	for (unsigned k = 0; k < 3u; k++) {
		random_tsbk(blocks[k]);
		blocks[k][0] &= 0x3Fu;	/* not protected (the CAI receiver drops a protected TSBK) */;
		tsbk_seal(blocks[k]);
	}
	blocks[0][0] |= 0x80u;
	tsbk_seal(blocks[0]);
	push_noise(&rx, 101);
	unsigned len = tsdu_build(frame, 0x1C8, blocks, 1);
	CHECK(len == 158u);
	CHECK(push_frame(&rx, frame, len, 22, got, &ngot) == 1 && ngot == 1 && !memcmp(got[0], blocks[0], 12));
	blocks[0][0] &= 0x7Fu;
	tsbk_seal(blocks[0]);
	blocks[2][0] |= 0x80u;
	tsbk_seal(blocks[2]);
	len = tsdu_build(frame, 0x1C8, blocks, 3);
	CHECK(len == 359u);
	CHECK(push_frame(&rx, frame, len, 1, got, &ngot) == 1 && ngot == 4);
	CHECK(!memcmp(got[1], blocks[0], 12) && !memcmp(got[2], blocks[1], 12) && !memcmp(got[3], blocks[2], 12));
	len = tsdu_build(frame, 0x1C8, blocks + 2, 1);
	CHECK(push_frame(&rx, frame, len, 22, got, &ngot) == 1 && ngot == 5 && !memcmp(got[4], blocks[2], 12));
	/* A block without its flag: the receiver waits for a second block, takes the next TSDU's sync for it and drops
	 * both (the CRC fails); the one after is framed again */
	uint8_t one[1][12];
	random_tsbk(one[0]);
	one[0][0] &= 0x3Fu;
	tsbk_seal(one[0]);
	len = tsdu_build(frame, 0x1C8, one, 1);
	CHECK(push_frame(&rx, frame, len, 22, got, &ngot) == 0 && ngot == 5);
	len = tsdu_build(frame, 0x1C8, blocks + 2, 1);
	CHECK(push_frame(&rx, frame, len, 22, got, &ngot) == 0 && ngot == 5);
	CHECK(push_frame(&rx, frame, len, 22, got, &ngot) == 1 && ngot == 6 && !memcmp(got[5], blocks[2], 12));
	/* A damaged block: dropped, never a wrong TSBK */
	len = tsdu_build(frame, 0x1C8, blocks + 2, 1);
	for (unsigned i = 0; i < 40u; i++) {
		unsigned at = tsdu_physical(112u + rnd() % 196u);
		frame[at >> 3] ^= (uint8_t)(0x80u >> (at & 7u));
	}
	unsigned before = ngot;
	if (push_frame(&rx, frame, len, 22, got, &ngot))
		CHECK(!memcmp(got[before], blocks[2], 12));
	/* Another NAC: not framed */
	before = ngot;
	len = tsdu_build(frame, 0x293, blocks + 2, 1);
	CHECK(push_frame(&rx, frame, len, 22, got, &ngot) == 0 && ngot == before);
	/* The conventional voice framer (the binding's receiver) never takes a TSDU, and the binding's NID decoder keeps
	 * rejecting it */
	p25_rx vrx;
	p25_rx_init(&vrx, P25_NAC_ANY, 2);
	len = tsdu_build(frame, 0x1C8, blocks + 2, 1);
	for (unsigned i = 0; i < len; i++)
		CHECK(p25_rx_push_dibit(&vrx, (uint8_t)(tsdu_getb(frame, 2u * i) << 1 | tsdu_getb(frame, 2u * i + 1u))) != 1);
	CHECK(vrx.bad_nids == 1u);
	p25_nid nid;
	CHECK(p25_nid_decode(p25_nid_encode(0x1C8, 7), &nid) != 0);
	CHECK(p25_nid_decode(p25_nid_encode(0x1C8, P25_DUID_LDU1), &nid) == 0 && nid.duid == P25_DUID_LDU1);
	printf("ok   TSDU framing (CAI control receiver): 1- and 3-block TSDUs, last-block flag, damage, NAC filter, "
	       "not in the voice framer\n");
}

/* ---- The channel plan and grants ---- */

static uint32_t now;
static p25tk_view_t view;
static void look(void) { CHECK(p25tk_snapshot(&view)); }

static void tsbk(const uint8_t b[10]) { p25tk_tsbk(b, now); }
/* IDEN_UP (0x3D): id, BW 9 bits, TX offset 9 bits, spacing 10 bits (125 Hz), base 32 bits (5 Hz); IDEN_UP_VU
 * (0x34): id, BW 4, offset 14, spacing, base; IDEN_UP_TDMA (0x33): id, channel type 4, offset 14, spacing, base */
static void iden(uint8_t op, unsigned id, unsigned type_or_bw, uint32_t base_hz, uint32_t spacing_hz)
{
	uint8_t b[10] = { op, 0x00 };
	uint32_t base = base_hz / 5u, spacing = spacing_hz / 125u;
	if (op == 0x3D) {
		b[2] = (uint8_t)(id << 4 | (type_or_bw >> 5));
		b[3] = (uint8_t)(type_or_bw << 3);
		b[3] |= 0x04u;	/* an offset sign bit, whatever: receive only */
	} else {
		b[2] = (uint8_t)(id << 4 | (type_or_bw & 15u));
		b[3] = 0x80u;
	}
	b[4] = (uint8_t)(b[4] | ((spacing >> 8) & 3u));
	b[5] = (uint8_t)spacing;
	b[6] = (uint8_t)(base >> 24);
	b[7] = (uint8_t)(base >> 16);
	b[8] = (uint8_t)(base >> 8);
	b[9] = (uint8_t)base;
	tsbk(b);
}
static void grant(uint8_t opts, uint16_t ch, uint16_t tg, uint32_t src)
{
	const uint8_t b[10] = { 0x00, 0x00, opts, (uint8_t)(ch >> 8), (uint8_t)ch, (uint8_t)(tg >> 8), (uint8_t)tg,
				(uint8_t)(src >> 16), (uint8_t)(src >> 8), (uint8_t)src };
	tsbk(b);
}
static void grant_update(uint16_t ch1, uint16_t tg1, uint16_t ch2, uint16_t tg2)
{
	const uint8_t b[10] = { 0x02, 0x00, (uint8_t)(ch1 >> 8), (uint8_t)ch1, (uint8_t)(tg1 >> 8), (uint8_t)tg1,
				(uint8_t)(ch2 >> 8), (uint8_t)ch2, (uint8_t)(tg2 >> 8), (uint8_t)tg2 };
	tsbk(b);
}

static void test_plan(void)
{
	now = 1000;
	p25tk_start(851012500u, 0x1C8, 0, true, now);
	bool tdma;
	/* 800 MHz: base 851.00625 MHz, 6.25 kHz spacing (IDEN_UP) */
	iden(0x3D, 1, 0x64, 851006250u, 6250u);
	CHECK(p25tk_channel_hz(0x1000u | 10u, &tdma) == 851068750u && !tdma);
	CHECK(p25tk_channel_hz(0x1000u, NULL) == 851006250u);
	/* 800 MHz with 12.5 kHz, channel 0xFFF */
	iden(0x3D, 7, 0x64, 851012500u, 12500u);
	CHECK(p25tk_channel_hz(0x7FFFu, NULL) == 851012500u + 4095u * 12500u);
	/* 700 MHz (IDEN_UP_VU): base 764.00625 MHz, 6.25 kHz: channel 200 */
	iden(0x34, 2, 4, 764006250u, 6250u);
	CHECK(p25tk_channel_hz(0x2000u | 200u, &tdma) == 765256250u && !tdma);
	/* UHF: base 450.000 MHz, 12.5 kHz: channel 17 */
	iden(0x34, 3, 5, 450000000u, 12500u);
	CHECK(p25tk_channel_hz(0x3000u | 17u, NULL) == 450212500u);
	/* VHF: base 136.000 MHz, 2.5 kHz: channel 1000 */
	iden(0x34, 4, 5, 136000000u, 2500u);
	CHECK(p25tk_channel_hz(0x4000u | 1000u, NULL) == 138500000u);
	/* TDMA (IDEN_UP_TDMA, type 3: two slots a carrier): channel numbers count slots */
	iden(0x33, 5, 3, 851006250u, 12500u);
	CHECK(p25tk_channel_hz(0x5000u | 5u, &tdma) == 851031250u && tdma);
	/* Unknown identifier: 0 */
	CHECK(p25tk_channel_hz(0x6000u | 1u, &tdma) == 0 && !tdma);
	/* A later IDEN_UP for an identifier replaces it */
	iden(0x34, 3, 5, 460000000u, 6250u);
	CHECK(p25tk_channel_hz(0x3000u | 16u, NULL) == 460100000u);
	/* Vendor TSBKs (MFID 0x90) reuse opcodes: ignored; so is a protected one */
	uint8_t vendor[10] = { 0x3D, 0x90, 0x60, 0, 0, 100, 0x0A, 0, 0, 0 };
	tsbk(vendor);
	CHECK(p25tk_channel_hz(0x6000u, NULL) == 0);
	vendor[0] = 0x7D;
	vendor[1] = 0x00;
	tsbk(vendor);
	CHECK(p25tk_channel_hz(0x6000u, NULL) == 0);
	/* The system: NET_STS_BCST (WACN BEE00, system 1C8), RFSS_STS_BCST (system, RFSS 1, site 5) */
	const uint8_t net[10] = { 0x3B, 0x00, 0x01, 0xBE, 0xE0, 0x01, 0xC8, 0x10, 0x01, 0x70 };
	const uint8_t rfss[10] = { 0x3A, 0x00, 0x01, 0x11, 0xC8, 0x01, 0x05, 0x10, 0x01, 0x70 };
	tsbk(net);
	tsbk(rfss);
	look();
	CHECK(view.have_net && view.wacn == 0xBEE00u && view.sysid == 0x1C8u);
	CHECK(view.have_site && view.rfss == 1u && view.site == 5u);
	CHECK(view.state == P25TK_CONTROL && view.tsbks == 11u);
	/* Grants: GRP_V_CH_GRANT carries options, channel, group and source; an update carries two pairs */
	grant(0x40, 0x1000u | 10u, 4001, 0x123456u);	/* encrypted: noted only */
	look();
	CHECK(view.n == 1 && view.recent[0].tg == 4001 && view.recent[0].src == 0x123456u &&
	      view.recent[0].hz == 851068750u && view.recent[0].ch == (0x1000u | 10u) && view.recent[0].flags == P25TK_G_ENC);
	grant_update(0x5000u | 4u, 4002, 0x6000u | 1u, 4003);
	look();
	CHECK(view.n == 3 && view.recent[0].tg == 4003 && view.recent[0].flags == P25TK_G_NO_PLAN &&
	      view.recent[1].tg == 4002 && view.recent[1].flags == P25TK_G_TDMA && view.recent[2].tg == 4001);
	grant(0x00, 0x1000u | 12u, 4004, 77);	/* 851.08125: not on this radio */
	look();
	CHECK(view.recent[0].tg == 4004 && view.recent[0].flags == P25TK_G_BAND && view.state == P25TK_CONTROL);
	/* The same group and channel again: one entry, moved to the front, the source kept */
	grant_update(0x1000u | 10u, 4001, 0x1000u | 10u, 4001);
	look();
	CHECK(view.n == 4 && view.recent[0].tg == 4001 && view.recent[0].src == 0x123456u);
	/* The list keeps the newest P25TK_RECENT */
	for (unsigned i = 0; i < 10u; i++)
		grant_update(0x6000u | i, (uint16_t)(5000u + i), 0x6000u | i, (uint16_t)(5000u + i));
	look();
	CHECK(view.n == P25TK_RECENT && view.recent[0].tg == 5009 && view.recent[P25TK_RECENT - 1u].tg == 5004);
	p25tk_stop();
	CHECK(!p25tk_on());
	printf("ok   channel plan (800 / 700 / UHF / VHF / TDMA / unknown, vendor and protected TSBKs ignored), "
	       "system, grant parsing and list\n");
}

/* ---- The follower ---- */

static p25tk_rx_t rx;
/* One radio task turn 10 ms later; the screen looks */
static uint32_t turn(void)
{
	now += 10;
	look();
	return p25tk_poll(&rx, now);
}
static uint32_t run_ms(unsigned ms)
{
	uint32_t hz = 0;
	for (unsigned t = 0; t < ms; t += 10)
		hz = turn();
	return hz;
}
/* The receiver arrives where the follower asked: the counts start again */
static void arrive(uint32_t hz)
{
	rx.on_hz = hz;
	rx.frames = rx.terms = 0;
	rx.tg = 0;
	rx.src = 0;
}
static void heard(unsigned frames)
{
	rx.frames += frames;
	rx.last_ms = now;
}

#define CC 433500000u
#define VOICE_CH (0x1000u | 80u)	/* 433.000 + 80 * 12.5 kHz = 434.000 MHz */
#define VOICE_HZ 434000000u
static void control_setup(uint16_t tg, bool all)
{
	now = 50000;
	memset(&rx, 0, sizeof rx);
	p25tk_start(CC, 0x1C8, tg, all, now);
	CHECK(p25tk_on() && p25tk_hz() == CC && p25tk_nac() == 0x1C8);
	CHECK(turn() == CC);
	look();
	CHECK(view.state == P25TK_HUNT && view.cc_hz == CC && view.follow_tg == tg && view.heard_nac == 0xFFFFu);
	arrive(CC);
	rx.nac = 0x1C8;
	heard(3);
	iden(0x3D, 1, 0x64, 433000000u, 12500u);
	iden(0x33, 2, 3, 433000000u, 12500u);
	CHECK(turn() == CC);
	look();
	CHECK(view.state == P25TK_CONTROL && view.heard_nac == 0x1C8u);
}

static void test_follow(void)
{
	control_setup(101, false);
	/* Other groups, encrypted, Phase 2, an unknown plan: the receiver stays */
	grant(0x00, VOICE_CH, 202, 1);
	grant(0x40, VOICE_CH, 101, 2);
	grant(0x00, 0x2000u | 80u, 101, 3);
	grant(0x00, 0x9000u | 80u, 101, 4);
	CHECK(turn() == CC);
	look();
	CHECK(view.state == P25TK_CONTROL && view.recent[3].flags == P25TK_G_OTHER && view.recent[2].flags == P25TK_G_ENC &&
	      view.recent[1].flags == P25TK_G_TDMA && view.recent[0].flags == P25TK_G_NO_PLAN && view.calls == 0);
	/* A grant for the followed group: to the voice channel */
	grant(0x02, VOICE_CH, 101, 1234567);
	CHECK(turn() == VOICE_HZ);
	look();
	CHECK(view.state == P25TK_VOICE && view.call.tg == 101 && view.call.hz == VOICE_HZ && view.call.src == 1234567u &&
	      view.recent[0].flags == P25TK_G_FOLLOWED && view.calls == 1);
	/* While the receiver moves (no TSBKs reach the follower any more but stale ones): it waits */
	rx.on_hz = 0;
	grant(0x00, VOICE_CH, 202, 9);	/* a stale TSBK from before the move: noted, not followed */
	CHECK(run_ms(200) == VOICE_HZ);
	look();
	CHECK(view.recent[0].tg == 202 && view.recent[0].flags == P25TK_G_BUSY);
	/* There: voice frames for a while, then a terminator: back to the control channel */
	arrive(VOICE_HZ);
	for (unsigned i = 0; i < 20u; i++) {
		heard(1);
		CHECK(run_ms(180) == VOICE_HZ);
	}
	rx.terms = 1;
	CHECK(turn() == CC);
	look();
	CHECK(view.state == P25TK_CONTROL && view.call.tg == 101 && view.call.ms == now);
	/* The call's updates keep coming a little: not followed again (hold-off); a new grant (a new transmission) is */
	arrive(CC);
	grant_update(VOICE_CH, 101, VOICE_CH, 101);
	CHECK(turn() == CC);
	look();
	CHECK(view.recent[0].flags == P25TK_G_BUSY);
	grant(0x00, VOICE_CH, 101, 7654321);
	CHECK(turn() == VOICE_HZ);
	look();
	CHECK(view.call.src == 7654321u && view.calls == 2);
	/* The call fades (no terminator heard): back P25TK_FADE_MS after the last frame */
	arrive(VOICE_HZ);
	heard(4);
	uint32_t last = now;
	while (turn() == VOICE_HZ && now - last < 5000u) {}
	CHECK(now - last >= P25TK_FADE_MS && now - last < P25TK_FADE_MS + 30u);
	/* After the hold-off its updates are followed again (late entry into a call still going) */
	arrive(CC);
	now += P25TK_HOLDOFF_MS;
	heard(1);
	grant_update(VOICE_CH, 101, VOICE_CH, 101);
	CHECK(turn() == VOICE_HZ);
	/* Late entry: the source comes from the voice's link control */
	arrive(VOICE_HZ);
	look();
	CHECK(view.call.src == 0);
	heard(2);
	rx.tg = 101;
	rx.src = 3101234;
	turn();
	look();
	CHECK(view.call.src == 3101234u);
	rx.terms = 1;
	CHECK(turn() == CC);
	/* No call on the voice channel: back after P25TK_NO_CALL_MS, and no grant of it is followed for a while */
	arrive(CC);
	grant(0x00, VOICE_CH, 101, 1);
	CHECK(turn() == VOICE_HZ);
	arrive(VOICE_HZ);
	uint32_t at = now;
	while (turn() == VOICE_HZ && now - at < 5000u) {}
	CHECK(now - at >= P25TK_NO_CALL_MS && now - at < P25TK_NO_CALL_MS + 30u);
	arrive(CC);
	grant(0x00, VOICE_CH, 101, 1);
	CHECK(turn() == CC);
	now += P25TK_MISS_HOLDOFF_MS;
	at = now;
	grant(0x00, VOICE_CH, 101, 1);
	CHECK(turn() == VOICE_HZ);
	/* The receiver never gets there: back after P25TK_MOVE_MS */
	rx.on_hz = 0;
	while (turn() == VOICE_HZ && now - at < 10000u) {}
	CHECK(now - at >= P25TK_MOVE_MS && now - at < P25TK_MOVE_MS + 30u);
	/* The control channel is lost: searching after P25TK_LOST_MS without a TSBK, locked again with the next */
	arrive(CC);
	at = now;
	while (turn() == CC && now - at < 10000u) {
		look();
		if (view.state == P25TK_HUNT)
			break;
	}
	look();
	CHECK(view.state == P25TK_HUNT && view.lost == 1u && now - at >= P25TK_LOST_MS && now - at < P25TK_LOST_MS + 30u);
	grant(0x00, VOICE_CH, 202, 1);
	look();
	CHECK(view.state == P25TK_CONTROL);
	/* Every group: the other group's grant is followed too */
	p25tk_follow(101, true);
	grant(0x00, VOICE_CH, 202, 1);
	CHECK(turn() == VOICE_HZ);
	look();
	CHECK(view.all && view.call.tg == 202);
	rx.on_hz = VOICE_HZ;
	rx.terms = 1;
	CHECK(turn() == CC);
	/* Rate and bad blocks */
	arrive(CC);
	rx.bad = 7;
	for (unsigned i = 0; i < 10u; i++)
		grant(0x40, VOICE_CH, 303, 1);
	run_ms(1000);
	look();
	CHECK(view.bad == 7u && view.rate >= 10u);
	/* Nobody looks: it stops itself */
	for (unsigned i = 0; i < 600u && p25tk_on(); i++) {
		now += 10;
		p25tk_poll(&rx, now);
	}
	CHECK(!p25tk_on() && p25tk_hz() == 0);
	printf("ok   follower: grant -> voice channel -> terminator / fade / no call / no retune -> control channel; "
	       "other group, encrypted, Phase 2, unknown plan ignored; hold-offs; late-entry source; lost -> searching; "
	       "every group; idle stop\n");
}

int main(void)
{
	test_crc();
	test_trellis();
	test_framer();
	test_plan();
	test_follow();
	if (failures) {
		printf("P25 trunk: %u failures\n", failures);
		return 1;
	}
	printf("P25 trunk: all passed\n");
	return 0;
}
