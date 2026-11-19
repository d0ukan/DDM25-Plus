/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus YSF Voice FR transmit framing (src/ysf/ysf_tx.c) against our own receive layer (src/ysf: the framer,
 * the FICH, the callsign data channels, the VW voice sections, P25's IMBE channel decoder). The reference cross-check
 * is tools/test_ysf_tx_ref.sh (MMDVMHost, MMDVM_CM). Host only, nothing transmits.
 *   clang -std=c11 -Wall -Wextra -Isrc -Isrc/p25 tests/plus/test_ysf_tx.c src/ysf/ysf_tx.c src/ysf/ysf_vw.c \
 *       src/ysf/ysf.c src/p25/p25.c -o test_ysf_tx */
#include <stdio.h>
#include <string.h>
#include "ysf/ysf.h"
#include "ysf/ysf_vw.h"
#include "ysf/ysf_tx.h"
#include "p25/p25.h"

static int failures;
static unsigned checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static uint32_t rng = 1919u;
static uint32_t rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }
static unsigned getb(const uint8_t *p, unsigned i) { return (p[i >> 3] >> (7u - (i & 7u))) & 1u; }
static void flip(uint8_t *p, unsigned i) { p[i >> 3] ^= (uint8_t)(0x80u >> (i & 7u)); }

static void random_u(uint16_t u[8])
{
	for (unsigned i = 0; i < 8u; i++)
		u[i] = (uint16_t)(rnd() & ((1u << (i < 4u ? 12u : i < 7u ? 11u : 7u)) - 1u));
}

/* Callsign data channels: round trips, the three places don't overlap, errors corrected, garbage refused */
static void test_csd(void)
{
	uint8_t frame[YSF_FRAME_BYTES], in[3][YSF_CSD_BYTES], out[YSF_CSD_BYTES];
	for (unsigned round = 0; round < 200u; round++) {
		memset(frame, 0, sizeof frame);
		for (unsigned w = 0; w < 3u; w++)
			for (unsigned i = 0; i < YSF_CSD_BYTES; i++)
				in[w][i] = (uint8_t)rnd();
		ysf_csd_put(in[YSF_CSD1], YSF_CSD1, frame);
		ysf_csd_put(in[YSF_CSD2], YSF_CSD2, frame);
		/* CSD1 and CSD2 fill the whole payload between them, and no byte before it */
		for (unsigned b = 0; b < 30u; b++)
			CHECK(frame[b] == 0);
		CHECK(ysf_csd_get(frame, YSF_CSD1, out) && memcmp(out, in[YSF_CSD1], YSF_CSD_BYTES) == 0);
		CHECK(ysf_csd_get(frame, YSF_CSD2, out) && memcmp(out, in[YSF_CSD2], YSF_CSD_BYTES) == 0);
		/* Four scattered errors in CSD1's coded bits (one in each of four 18-byte sections' first 9 bytes) */
		for (unsigned e = 0; e < 4u; e++)
			flip(frame, 8u * (30u + 18u * e) + rnd() % 72u);
		CHECK(ysf_csd_get(frame, YSF_CSD1, out) && memcmp(out, in[YSF_CSD1], YSF_CSD_BYTES) == 0);
		CHECK(ysf_csd_get(frame, YSF_CSD2, out) && memcmp(out, in[YSF_CSD2], YSF_CSD_BYTES) == 0);
		/* CSD3: payload bytes 0..44 only */
		memset(frame, 0, sizeof frame);
		ysf_csd_put(in[YSF_CSD3], YSF_CSD3, frame);
		for (unsigned b = 0; b < YSF_FRAME_BYTES; b++)
			if (b < 30u || b >= 75u)
				CHECK(frame[b] == 0);
		CHECK(ysf_csd_get(frame, YSF_CSD3, out) && memcmp(out, in[YSF_CSD3], YSF_CSD_BYTES) == 0);
	}
	/* Random payloads: the CRC refuses (nearly) all; never more than chance allows */
	unsigned accepted = 0;
	for (unsigned round = 0; round < 2000u; round++) {
		for (unsigned b = 0; b < YSF_FRAME_BYTES; b++)
			frame[b] = (uint8_t)rnd();
		accepted += ysf_csd_get(frame, YSF_CSD1, out);
	}
	CHECK(accepted <= 2u);
}

static void test_fich_raw(void)
{
	uint8_t raw[4];
	ysf_fich_t f;
	ysf_tx_fich_raw(YSF_FI_COMMS, 0, 1, 99, raw);
	CHECK(raw[0] == 0x60 && raw[1] == 0x01 && raw[2] == (YSF_TX_LOW_DEV ? 0x43 : 0x03) && raw[3] == 99);
	ysf_fich_fields(raw, &f);
	CHECK(f.fi == YSF_FI_COMMS && f.cm == 0 && f.bn == 0 && f.bt == 0 && f.fn == 0 && f.ft == 1);
	CHECK(f.dt == YSF_DT_VOICE_FR && !f.voip && f.mr == 0 && f.dev == (YSF_TX_LOW_DEV != 0) && f.dgid == 99);
	ysf_tx_fich_raw(YSF_FI_TERMINATOR, 7, 7, 200, raw);	/* fields masked */
	ysf_fich_fields(raw, &f);
	CHECK(f.fi == YSF_FI_TERMINATOR && f.fn == 7 && f.ft == 7 && f.dgid == (200u & 0x7Fu) && (raw[0] & 0x30u) == 0x20u);
}

#define MAX_IMBE 64
#define AIR_BITS (YSF_FRAME_BITS * 20u)
static uint8_t air[AIR_BITS / 8u];
static uint16_t sent_u[MAX_IMBE][8];

/* A transmission of `talk` IMBE frames from the microphone, taken in chunks of `chunk` bits (288 = one modem block),
 * closed when the microphone ends; returns the air bits */
static unsigned transmit(const char *call, uint8_t dgid, unsigned talk, unsigned chunk, unsigned *imbe_out)
{
	static uint8_t frame[YSF_FRAME_BYTES];
	ysf_tx_t t;
	unsigned bits = 0, spoken = 0, guard = 0;
	memset(air, 0, sizeof air);
	ysf_tx_begin(&t, frame, call, dgid);
	while (!ysf_tx_done(&t) && guard++ < 10000u) {
		unsigned n = ysf_tx_take(&t, air, bits, chunk);
		bits += n;
		CHECK(bits <= AIR_BITS);
		if (ysf_tx_need_voice(&t)) {
			uint16_t u[8];
			if (spoken < talk) {
				random_u(u);
				spoken++;
			} else {
				if (!t.closing) {
					ysf_tx_close(&t);
					continue;
				}
				memset(u, 0, sizeof u);	/* the worker encodes silence here; any valid frame will do */
			}
			CHECK(t.imbe < MAX_IMBE);
			memcpy(sent_u[t.imbe], u, sizeof u);
			sent_u[t.imbe][7] = (uint16_t)((u[7] & ~1u) | (t.imbe & 1u));
			ysf_tx_voice(&t, u);
		} else if (spoken >= talk && !t.closing && n == 0) {
			CHECK(0);	/* stuck: nothing to hand out and no voice wanted */
			break;
		}
	}
	CHECK(ysf_tx_done(&t));
	CHECK(t.frames * YSF_FRAME_BITS == bits);
	*imbe_out = t.imbe;
	return bits;
}

/* What our receiver makes of it: the frames found, checked one by one */
static void receive_check(unsigned bits, const char *call, uint8_t dgid, unsigned imbe, unsigned *frames_out)
{
	ysf_framer_t fr;
	ysf_framer_init(&fr);
	/* the 0x77 preamble (as the worker sends first) and filler after: no frame found in them */
	for (unsigned i = 0; i < 3u * 288u; i++)
		CHECK(!ysf_framer_bit(&fr, (uint8_t)((0x77u >> (7u - (i & 7u))) & 1u)));
	char want_src[11];
	snprintf(want_src, sizeof want_src, "%-10s", call);
	unsigned frames = 0, got_imbe = 0, comms = 0;
	bool term = false;
	for (unsigned i = 0; i < bits + 400u; i++) {
		uint8_t bit = i < bits ? (uint8_t)getb(air, i) : (uint8_t)((0x77u >> (7u - (i & 7u))) & 1u);
		if (!ysf_framer_bit(&fr, bit))
			continue;
		ysf_fich_t f;
		uint8_t csd[YSF_CSD_BYTES];
		CHECK(ysf_fich_decode(fr.frame, &f));
		CHECK(f.dt == YSF_DT_VOICE_FR && f.dgid == dgid && f.cm == 0 && f.mr == 0 && !f.voip && f.bn == 0 && f.bt == 0);
		CHECK((f.raw[0] & 0x30u) == 0x20u && f.dev == (YSF_TX_LOW_DEV != 0));
		if (frames == 0 || f.fi == YSF_FI_TERMINATOR) {
			CHECK(f.fi == (frames == 0 ? YSF_FI_HEADER : YSF_FI_TERMINATOR) && f.fn == 0 && f.ft == 1);
			CHECK(ysf_csd_get(fr.frame, YSF_CSD1, csd));
			CHECK(memcmp(csd, "**********", 10) == 0 && memcmp(csd + 10, want_src, 10) == 0);
			CHECK(ysf_csd_get(fr.frame, YSF_CSD2, csd) && memcmp(csd, "                    ", 20) == 0);
			CHECK(ysf_vw_voice_count(&f) == 0);
			term = f.fi == YSF_FI_TERMINATOR;
		} else {
			CHECK(!term && f.fi == YSF_FI_COMMS && f.fn == 0 && f.ft == (comms == 0 ? 1u : 0u));
			if (comms == 0) {
				CHECK(ysf_csd_get(fr.frame, YSF_CSD3, csd) && memcmp(csd, "               *****", 20) == 0);
				for (unsigned b = 45; b < 54u; b++)
					CHECK(fr.frame[30u + b] == 0);
			}
			unsigned n = ysf_vw_voice_count(&f);
			CHECK(n == (comms == 0 ? 2u : 5u));
			for (unsigned k = 0; k < n; k++) {
				uint8_t cw[YSF_VW_IMBE_BYTES], u0e = 9;
				uint16_t u[8];
				CHECK(ysf_vw_voice(fr.frame, &f, k, cw));
				CHECK(p25_imbe_decode(cw, u, &u0e) == 0 && u0e == 0);
				CHECK(got_imbe < imbe && memcmp(u, sent_u[got_imbe], sizeof u) == 0);
				got_imbe++;
			}
			comms++;
		}
		frames++;
	}
	CHECK(term && got_imbe == imbe && fr.lost == 0);
	*frames_out = frames;
}

static void test_transmission(void)
{
	static const struct { const char *call; uint8_t dgid; unsigned talk, chunk, imbe, frames; } cases[] = {
		{ "TA1ABC", 0, 23, 288, 27, 8 },	/* SUB 2 + 4 x 5 + 1 (+4 silence) = 27: header, sub, 5 voice, end */
		{ "TA1ABC", 12, 22, 288, 22, 7 },	/* ends exactly at a frame: no silence, the terminator next */
		{ "TA2XYZ", 127, 1, 288, 2, 3 },	/* one frame of speech: the sub-header padded, no voice frame */
		{ "TA1ABCDEFG", 64, 40, 7, 42, 11 },	/* 10-character callsign, odd chunks */
		{ "K1A", 1, 12, 960, 12, 5 },
		{ "N0CALL", 5, 2, 1, 2, 3 },
	};
	for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; c++) {
		unsigned imbe = 0, frames = 0;
		unsigned bits = transmit(cases[c].call, cases[c].dgid, cases[c].talk, cases[c].chunk, &imbe);
		CHECK(imbe == cases[c].imbe);
		CHECK(bits == cases[c].frames * YSF_FRAME_BITS);
		receive_check(bits, cases[c].call, cases[c].dgid, imbe, &frames);
		CHECK(frames == cases[c].frames);
	}
	/* Chunk size doesn't change the air bits */
	static uint8_t first[AIR_BITS / 8u];
	unsigned imbe;
	rng = 5u;
	unsigned a = transmit("TA1ABC", 3, 30, 288, &imbe);
	memcpy(first, air, sizeof first);
	rng = 5u;
	unsigned b = transmit("TA1ABC", 3, 30, 13, &imbe);
	CHECK(a == b && memcmp(first, air, sizeof first) == 0);
}

/* The interface as the worker uses it: no bits before the header is taken, none while voice is due */
static void test_interface(void)
{
	uint8_t frame[YSF_FRAME_BYTES], out[YSF_FRAME_BYTES * 2];
	ysf_tx_t t;
	ysf_tx_begin(&t, frame, "TA1ABC", 0);
	CHECK(!ysf_tx_need_voice(&t) && t.phase == YSF_TX_HEADER);
	CHECK(ysf_tx_take(&t, out, 0, 1000) == YSF_FRAME_BITS);	/* the header, then the sub-header waits for voice */
	CHECK(ysf_tx_need_voice(&t) && t.phase == YSF_TX_SUBHEADER && ysf_tx_take(&t, out, 0, 10) == 0);
	uint16_t u[8] = { 0 };
	ysf_tx_voice(&t, u);
	CHECK(ysf_tx_need_voice(&t) && ysf_tx_take(&t, out, 0, 10) == 0);
	ysf_tx_voice(&t, u);
	CHECK(!ysf_tx_need_voice(&t));
	ysf_tx_voice(&t, u);	/* not wanted: ignored */
	CHECK(t.imbe == 2);
	CHECK(ysf_tx_take(&t, out, 0, 100) == 100 && ysf_tx_take(&t, out, 0, 2000) == YSF_FRAME_BITS - 100);
	CHECK(t.phase == YSF_TX_VOICE && ysf_tx_need_voice(&t));
	/* An empty transmission: closed before any voice, the header goes, then the terminator at once */
	ysf_tx_begin(&t, frame, "TA1ABC", 0);
	ysf_tx_close(&t);
	CHECK(ysf_tx_take(&t, out, 0, 2 * YSF_FRAME_BITS) == 2 * YSF_FRAME_BITS && ysf_tx_done(&t) && t.imbe == 0);
	CHECK(ysf_tx_take(&t, out, 0, 10) == 0 && !ysf_tx_need_voice(&t));
}

int main(void)
{
	test_csd();
	test_fich_raw();
	test_transmission();
	test_interface();
	printf("YSF transmit framing: %u checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
