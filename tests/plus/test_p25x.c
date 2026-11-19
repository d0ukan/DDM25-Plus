/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus P25 extras, the link control layer (src/p25/p25.c; tools/test_classic.py builds this with
 * CONFIG_P25_EXTRAS=1 and =0). LDU1s are made by the real encoder (p25_encode_ldu: voice, NID, status symbols, group
 * LC) and their link control is rewritten by tests/plus/p25_lc_build.h, written from MMDVMHost's formulation (RS
 * parity, Hamming (10,6,3), positions): first it must reproduce the encoder's frames bit for bit for group LCs, then
 * it writes the formats the encoder can't (unit to unit, vendor LCs). Checked: the emergency bit, unit-to-unit
 * target and source, correction of a bit error in every Hamming word and of 6 RS symbols, vendor / other formats
 * (Motorola talker alias LCF 0x15 / 0x17 MFID 0x90) told apart from damage and from a protected LC. */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include "p25.h"
#if CONFIG_P25_EXTRAS
#include "p25_lcx.h"
#endif
#include "p25_lc_build.h"

static unsigned failures;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint32_t rng = 0x9E3779B9u;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

/* An LDU1 of the real encoder: random voice parameters, a group LC */
static void encoder_ldu1(uint8_t f[P25_LDU_BYTES], uint16_t nac, uint8_t svc, uint16_t tg, uint32_t src)
{
	const p25_call call = { .nac = nac, .talkgroup = tg, .source_id = src, .service_options = svc };
	uint16_t imbe[P25_IMBE_FRAMES][8];
	const uint8_t lsd[2] = { 0x12, 0x34 };
	for (unsigned i = 0; i < P25_IMBE_FRAMES; i++)
		for (unsigned j = 0; j < 8u; j++)
			imbe[i][j] = (uint16_t)(rnd() & ((1u << (j < 4 ? 12 : j < 7 ? 11 : 7)) - 1u));
	CHECK(!p25_encode_ldu(&call, P25_DUID_LDU1, imbe, lsd, f));
}

#if CONFIG_P25_EXTRAS
static p25x_lc XLC;	/* the extras of the last decode (Classic r13: the binding's p25_decoded has no field for them) */
#endif
static int decode(const uint8_t *f, p25_decoded *d)
{
	memset(d, 0xA5, sizeof *d);
#if CONFIG_P25_EXTRAS
	memset(&XLC, 0xA5, sizeof XLC);
	return p25x_decode_frame(f, P25_LDU_BYTES, d, &XLC);
#else
	return p25_decode_frame(f, P25_LDU_BYTES, d);
#endif
}

/* The builder reproduces the encoder's LDU1 exactly (it is right where they overlap) */
static void test_builder(void)
{
	static const uint8_t svcs[] = { 0x00, 0x80, 0x81, 0x07, 0x84 };
	unsigned same = 0;
	for (unsigned i = 0; i < 300u; i++) {
		uint8_t a[P25_LDU_BYTES], b[P25_LDU_BYTES], lc[9];
		uint8_t svc = svcs[i % sizeof svcs];
		uint16_t tg = (uint16_t)(1u + rnd() % 65535u);
		uint32_t src = rnd() & 0xFFFFFFu;
		encoder_ldu1(a, (uint16_t)(rnd() & 0xFFFu), svc, tg, src);
		memcpy(b, a, sizeof b);
		for (unsigned k = 0; k < 24u; k++)	/* the builder writes every LC bit: clear them first */
			for (unsigned j = 0; j < 10u; j++)
				lcb_put(b, lcb_position(k, j), rnd() & 1u);
		lc_group(lc, svc, tg, src);
		ldu1_set_lc(b, lc);
		same += memcmp(a, b, sizeof a) == 0;
	}
	CHECK(same == 300u);
	printf("%s LC builder: %u of 300 encoder LDU1s reproduced bit for bit\n", same == 300u ? "ok  " : "FAIL", same);
}

static void test_emergency(void)
{
	uint8_t f[P25_LDU_BYTES];
	p25_decoded d;
	encoder_ldu1(f, 0x293, 0x80, 101, 77285);
	CHECK(!decode(f, &d) && d.metadata_valid && d.link_control_format == 0 && d.service_options == 0x80);
	CHECK(d.talkgroup == 101 && d.source_id == 77285 && d.clear_voice && d.nid.nac == 0x293);
#if CONFIG_P25_EXTRAS
	CHECK(XLC.target_id == 0 && !XLC.lc_other);
#endif
	encoder_ldu1(f, 0x293, 0x00, 101, 77285);
	CHECK(!decode(f, &d) && d.metadata_valid && !(d.service_options & 0x80));
	printf("%s emergency bit: LCF 0x00 service options 0x80 decoded, 0x00 not an emergency\n", failures ? "FAIL" : "ok  ");
}

static void test_unit_to_unit(void)
{
	unsigned before = failures;
	uint8_t f[P25_LDU_BYTES], lc[9];
	p25_decoded d;
	static const struct { uint32_t target, src; uint8_t svc; } calls[] = {
		{ 1234, 77285, 0x00 }, { 1, 9999999, 0x00 }, { 0xFFFFFF, 1, 0x80 }, { 3101234, 3105678, 0x84 },
	};
	for (unsigned i = 0; i < sizeof calls / sizeof calls[0]; i++) {
		encoder_ldu1(f, 0x659, 0, 1, 1);
		lc_unit(lc, calls[i].svc, calls[i].target, calls[i].src);
		ldu1_set_lc(f, lc);
		CHECK(!decode(f, &d) && d.nid.duid == P25_DUID_LDU1 && d.nid.nac == 0x659);
#if CONFIG_P25_EXTRAS
		CHECK(d.metadata_valid && d.link_control_format == 3 && XLC.target_id == calls[i].target);
		CHECK(d.source_id == calls[i].src && d.talkgroup == 0 && d.service_options == calls[i].svc && d.clear_voice);
		CHECK(!XLC.lc_other && d.metadata_corrected_symbols == 0);
#else
		CHECK(!d.metadata_valid);	/* without the switch: not decoded, as before */
#endif
	}
#if CONFIG_P25_EXTRAS
	printf("%s unit to unit (LCF 0x03): target, source, service options decoded; talkgroup 0\n",
	       failures == before ? "ok  " : "FAIL");
#else
	printf("%s unit to unit (LCF 0x03) not decoded without CONFIG_P25_EXTRAS (as before)\n",
	       failures == before ? "ok  " : "FAIL");
#endif
}

/* A bit error in every Hamming word, then 6 hexbits beyond Hamming (RS corrects up to 6 of 24) */
static void test_errors(void)
{
	unsigned before = failures, rounds = 200;
	for (unsigned r = 0; r < rounds; r++) {
		uint8_t f[P25_LDU_BYTES], lc[9];
		p25_decoded d;
		uint32_t target = 1u + rnd() % 0xFFFFFEu, src = 1u + rnd() % 9999999u;
		encoder_ldu1(f, 0x293, 0, 1, 1);
#if CONFIG_P25_EXTRAS
		lc_unit(lc, r & 1u ? 0x80 : 0, target, src);
#else
		lc_group(lc, r & 1u ? 0x80 : 0, (uint16_t)(1u + target % 65535u), src);	/* the old decoder's format */
#endif
		ldu1_set_lc(f, lc);
		uint8_t g[P25_LDU_BYTES];
		memcpy(g, f, sizeof g);
		for (unsigned k = 0; k < 24u; k++) {
			unsigned p = lcb_position(k, rnd() % 10u);
			g[p >> 3] ^= (uint8_t)(0x80u >> (p & 7u));
		}
		bool ok1 = !decode(g, &d) && d.metadata_valid && d.metadata_corrected_bits == 24u && d.source_id == src;
#if CONFIG_P25_EXTRAS
		ok1 = ok1 && XLC.target_id == target;
#endif
		CHECK(ok1);
		memcpy(g, f, sizeof g);
		bool hit[24] = { false };
		for (unsigned n = 0; n < 6u;) {	/* 6 different words, each hit in 3 of its 6 data bits */
			unsigned k = rnd() % 24u;
			if (hit[k])
				continue;
			hit[k] = true;
			n++;
			for (unsigned j = 0; j < 3u; j++) {
				unsigned p = lcb_position(k, j * 2u);
				g[p >> 3] ^= (uint8_t)(0x80u >> (p & 7u));
			}
		}
		bool ok2 = !decode(g, &d) && d.metadata_valid && d.metadata_corrected_symbols >= 1u && d.source_id == src &&
			   d.service_options == (r & 1u ? 0x80 : 0);
#if CONFIG_P25_EXTRAS
		ok2 = ok2 && XLC.target_id == target;
#endif
		CHECK(ok2);
		/* far beyond: every word hit; never valid metadata, never "another format" */
		for (unsigned k = 0; k < 24u; k++)
			for (unsigned j = 0; j < 4u; j++) {
				unsigned p = lcb_position(k, (j * 3u + k) % 10u);
				g[p >> 3] ^= (uint8_t)(0x80u >> (p & 7u));
			}
		if (!decode(g, &d)) {
#if CONFIG_P25_EXTRAS
			CHECK(!d.metadata_valid ? !XLC.lc_other : 1);
#endif
		}
	}
	printf("%s link control FEC: %u LCs (unit to unit with the switch) with a bit flipped in all 24 Hamming words, and "
	       "with 6 RS symbols wrong, decoded right\n", failures == before ? "ok  " : "FAIL", rounds);
}

/* Formats this firmware does not decode: told apart from damage (lc_other) and from a protected LC */
static void test_other_formats(void)
{
	unsigned before = failures;
	static const struct { uint8_t lcf, mfid; bool other; const char *what; } formats[] = {
		{ 0x15, 0x90, true, "Motorola talker alias header (OP25 terms)" },
		{ 0x17, 0x90, true, "Motorola talker alias block" },
		{ 0x00, 0x90, true, "LCF 0 with a vendor MFID" },
		{ 0x0F, 0x00, true, "call termination" },
		{ 0x09, 0x00, true, "source ID extension" },
		{ 0x80, 0x00, false, "protected group voice" },
		{ 0x83, 0x00, false, "protected unit to unit" },
	};
	for (unsigned i = 0; i < sizeof formats / sizeof formats[0]; i++) {
		uint8_t f[P25_LDU_BYTES], lc[9];
		p25_decoded d;
		encoder_ldu1(f, 0x293, 0, 101, 77285);
		for (unsigned b = 0; b < 9u; b++)
			lc[b] = (uint8_t)rnd();
		lc[0] = formats[i].lcf;
		lc[1] = formats[i].mfid;
		ldu1_set_lc(f, lc);
		CHECK(!decode(f, &d) && !d.metadata_valid && d.link_control_format == formats[i].lcf);
#if CONFIG_P25_EXTRAS
		CHECK(XLC.lc_other == formats[i].other && XLC.target_id == 0);
#endif
	}
	printf("%s other LC formats: not decoded; vendor / other formats marked as such (not damage), protected ones not\n",
	       failures == before ? "ok  " : "FAIL");
}

int main(void)
{
	test_builder();
	test_emergency();
	test_unit_to_unit();
	test_errors();
	test_other_formats();
	printf("P25 link control extras (CONFIG_P25_EXTRAS=%d): %u failures\n", CONFIG_P25_EXTRAS, failures);
	return failures ? 1 : 0;
}
