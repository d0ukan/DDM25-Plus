/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus NXDN48 receive (src/nxdn, docs/nxdn.md) host tests. The cross-check against MMDVMHost and RadioFoundry's
 * ambeFec is tests/plus/test_nxdn_ref.cpp (tools/test_nxdn_ref.sh).
 *
 * The transmit side lives here only: a frame builder (convolutional code, puncturing, interleaving, CRCs, LICH,
 * layer 3, the AMBE+2 field FEC; ported from DM32-nxdn-p25 src/nxdn/nxdn.c and nxdn_call.c, GPL-3.0-or-later, which
 * follow MMDVMHost, G4KLX, GPL-2.0-or-later) and a floating point 4FSK modulator to 8 kHz discriminator samples
 * (root raised cosine 0.2 x sin(x)/x pulse, as MMDVM's NXDN transmitter) with timing phase, clock error, carrier
 * offset, gain, polarity and Gaussian noise. Checks: scrambler, CRCs, LICH parity, Golay, the AMBE+2 parameter bits,
 * SACCH / FACCH1 with channel errors, and whole transmissions through the demodulator: sync, LICH, caller IDs from the
 * header and by late entry, every voice field's 49 parameter bits, the release, RAN / group / encryption filters,
 * inverted polarity, and noise alone (no false call). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nxdn/nxdn.h"
#include "nxdn/nxdn_demod.h"
#include "nxdn/nxdn_rx.h"

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint32_t rng = 0x1234567u;
static uint32_t rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}
static double urand(void) { return ((rnd() >> 8) + 0.5) / 16777216.0; }
static double gauss(void) { return sqrt(-2.0 * log(urand())) * cos(6.283185307179586 * urand()); }
static void flip(uint8_t *b, unsigned i) { b[i >> 3] ^= (uint8_t)(0x80u >> (i & 7u)); }

/* ---------------------------------------------------------------- transmit side (test only) */

static void conv_encode(const uint8_t *in, unsigned n, uint8_t *out)
{
	unsigned d1 = 0, d2 = 0, d3 = 0, d4 = 0, k = 0;
	for (unsigned i = 0; i < n; i++) {
		unsigned d = nxdn_bit(in, i);
		nxdn_set_bit(out, k++, (d + d3 + d4) & 1u);
		nxdn_set_bit(out, k++, (d + d1 + d2 + d4) & 1u);
		d4 = d3;
		d3 = d2;
		d2 = d1;
		d1 = d;
	}
}

static uint8_t lich_pack(uint8_t rfct, uint8_t fct, uint8_t option, uint8_t dir)
{
	uint8_t v = (uint8_t)((rfct & 3u) << 6 | (fct & 3u) << 4 | (option & 3u) << 2 | (dir & 1u) << 1);
	return (uint8_t)(v | (((v >> 7) ^ (v >> 6) ^ (v >> 5) ^ (v >> 4)) & 1u));
}

static void lich_put(uint8_t *f, uint8_t raw)
{
	for (unsigned i = 0; i < 8u; i++) {
		nxdn_set_bit(f, NXDN_FSW_BITS + 2u * i, (raw >> (7u - i)) & 1u);
		nxdn_set_bit(f, NXDN_FSW_BITS + 2u * i + 1u, 1);
	}
}

static void sacch_put(uint8_t *f, uint8_t ran, uint8_t structure, uint32_t data)
{
	uint8_t in[5] = { 0 }, cod[9] = { 0 }, punct[8] = { 0 };
	in[0] = (uint8_t)((structure & 3u) << 6 | (ran & 0x3Fu));
	for (unsigned i = 0; i < 18u; i++)
		nxdn_set_bit(in, 8u + i, (data >> (17u - i)) & 1u);
	uint8_t c = nxdn_crc6(in, 26);
	for (unsigned i = 0; i < 6u; i++)
		nxdn_set_bit(in, 26u + i, (c >> (5u - i)) & 1u);
	conv_encode(in, 36, cod);
	unsigned n = 0;
	for (unsigned i = 0; i < 72u; i++)
		if (i % 6u != 5u)
			nxdn_set_bit(punct, n++, nxdn_bit(cod, i));
	for (unsigned i = 0; i < 60u; i++)
		nxdn_set_bit(f, NXDN_SACCH_OFFSET + (i % 12u) * 5u + i / 12u, nxdn_bit(punct, i));
}

static void facch1_put(uint8_t *f, unsigned which, const uint8_t data[NXDN_LAYER3_BYTES])
{
	uint8_t in[12] = { 0 }, cod[24] = { 0 }, punct[18] = { 0 };
	memcpy(in, data, NXDN_LAYER3_BYTES);
	uint16_t c = nxdn_crc12(in, 80);
	for (unsigned i = 0; i < 12u; i++)
		nxdn_set_bit(in, 80u + i, (c >> (11u - i)) & 1u);
	conv_encode(in, 96, cod);
	unsigned n = 0;
	for (unsigned i = 0; i < 192u; i++)
		if (i % 4u != 1u)
			nxdn_set_bit(punct, n++, nxdn_bit(cod, i));
	unsigned base = NXDN_PAYLOAD_OFFSET + which * NXDN_FACCH1_BITS;
	for (unsigned i = 0; i < 144u; i++)
		nxdn_set_bit(f, base + (i % 16u) * 9u + i / 16u, nxdn_bit(punct, i));
}

static void layer3_put(uint8_t d[NXDN_LAYER3_BYTES], uint8_t type, uint8_t call_type, uint16_t src, uint16_t dst,
		       uint8_t cipher)
{
	memset(d, 0, NXDN_LAYER3_BYTES);
	d[0] = type & 0x3Fu;
	d[2] = (uint8_t)((call_type & 7u) << 5);
	d[3] = (uint8_t)(src >> 8);
	d[4] = (uint8_t)src;
	d[5] = (uint8_t)(dst >> 8);
	d[6] = (uint8_t)dst;
	d[7] = (uint8_t)((cipher & 3u) << 6);
}

static uint32_t golay23_encode(uint16_t data)
{
	uint32_t cw = (uint32_t)(data & 0xFFFu) << 11, r = cw;
	for (int i = 22; i >= 11; i--)
		if (r & (1u << i))
			r ^= 0xC75u << (i - 11);
	return cw | (r & 0x7FFu);
}

static uint32_t golay24_encode(uint16_t data)
{
	uint32_t c = golay23_encode(data), w = 0;
	for (uint32_t v = c; v; v &= v - 1u)
		w++;
	return c << 1 | (w & 1u);
}

/* 49 parameter bits -> the 72-bit AMBE+2 air frame (ambeFec.h's layout) */
static void ambe_encode(const uint8_t p[NXDN_AMBE_BITS], uint8_t slot[NXDN_SLOT_BYTES])
{
	uint16_t a = 0, b = 0;
	uint32_t c = 0;
	for (unsigned i = 0; i < 12u; i++) {
		a = (uint16_t)(a << 1 | p[i]);
		b = (uint16_t)(b << 1 | p[12u + i]);
	}
	for (unsigned i = 0; i < 25u; i++)
		c = c << 1 | p[24u + i];
	uint32_t c0 = golay24_encode(a), c1 = golay23_encode(b) ^ nxdn_ambe_c1_mask(a);
	memset(slot, 0, NXDN_SLOT_BYTES);
	for (unsigned n = 0; n < 72u; n++) {
		unsigned v = n < 24u ? (c0 >> (23u - n)) & 1u : n < 47u ? (c1 >> (46u - n)) & 1u : (c >> (71u - n)) & 1u;
		nxdn_set_bit(slot, 4u * (n % 18u) + n / 18u, v);
	}
}

static void frame_start(uint8_t *f, uint8_t fct, uint8_t option)
{
	memset(f, 0, NXDN_FRAME_BYTES);
	for (unsigned i = 0; i < NXDN_FSW_BITS; i++)
		nxdn_set_bit(f, i, (NXDN_FSW >> (19u - i)) & 1u);
	lich_put(f, lich_pack(NXDN_RFCT_RDCH, fct, option, NXDN_DIR_INBOUND));
}

/* A transmission: its dibits and the voice it carries */
#define MAX_DIBITS	20000u
#define MAX_SLOTS	256u
typedef struct {
	uint8_t dib[MAX_DIBITS];
	unsigned n;
	uint8_t params[MAX_SLOTS][NXDN_AMBE_BITS];
	unsigned slots;
	unsigned first_voice_dibit;	/* where the first voice frame starts */
} tx_t;

static void put_frame(tx_t *t, uint8_t *f)
{
	nxdn_scramble(f);
	for (unsigned i = 0; i < NXDN_FRAME_SYMBOLS && t->n < MAX_DIBITS; i++)
		t->dib[t->n++] = (uint8_t)((f[i / 4u] >> (6u - 2u * (i % 4u))) & 3u);
}

static void put_control(tx_t *t, uint8_t ran, const uint8_t l3[NXDN_LAYER3_BYTES])
{
	uint8_t f[NXDN_FRAME_BYTES];
	frame_start(f, NXDN_USC_SACCH_NS, NXDN_STEAL_FACCH);
	sacch_put(f, ran, NXDN_SF_SINGLE, (uint32_t)NXDN_MSG_IDLE << 10);
	facch1_put(f, 0, l3);
	facch1_put(f, 1, l3);
	put_frame(t, f);
}

/* steal: NXDN_STEAL_NONE (four voice fields) or NXDN_STEAL_FACCH1_1 (VCALL in the first half, two voice fields) */
static void put_voice(tx_t *t, uint8_t ran, const uint8_t l3[NXDN_LAYER3_BYTES], unsigned part, uint8_t steal)
{
	uint8_t f[NXDN_FRAME_BYTES];
	frame_start(f, NXDN_USC_SACCH_SS, steal);
	uint32_t chunk = 0;
	for (unsigned i = 0; i < 18u; i++)
		chunk = chunk << 1 | nxdn_bit(l3, part * 18u + i);
	sacch_put(f, ran, (uint8_t)(3u - part), chunk);
	if (steal == NXDN_STEAL_FACCH1_1)
		facch1_put(f, 0, l3);
	for (unsigned k = steal == NXDN_STEAL_FACCH1_1 ? 2u : 0u; k < 4u; k++) {
		uint8_t slot[NXDN_SLOT_BYTES];
		uint8_t *p = t->params[t->slots < MAX_SLOTS ? t->slots : MAX_SLOTS - 1u];
		for (unsigned i = 0; i < NXDN_AMBE_BITS; i++)
			p[i] = (uint8_t)(rnd() & 1u);
		ambe_encode(p, slot);
		for (unsigned i = 0; i < 72u; i++)
			nxdn_set_bit(f, NXDN_PAYLOAD_OFFSET + k * 72u + i, nxdn_bit(slot, i));
		t->slots++;
	}
	put_frame(t, f);
}

typedef struct {
	uint8_t ran, call_type, cipher;
	uint16_t src, dst;
	unsigned voice_frames;
	bool steal_one;		/* the third voice frame carries a FACCH1 VCALL */
	bool header, release;
} call_t;

static void build(tx_t *t, const call_t *c)
{
	memset(t, 0, sizeof *t);
	/* preamble: +3 / -3 dotting, then 0x5775FD (MMDVM's NXDN preamble) */
	for (unsigned i = 0; i < 36u; i++)
		t->dib[t->n++] = i & 1u ? 3u : 1u;
	static const uint8_t pre[3] = { 0x57, 0x75, 0xFD };
	for (unsigned k = 0; k < 12u; k++)
		t->dib[t->n++] = (uint8_t)((pre[k / 4u] >> (6u - 2u * (k % 4u))) & 3u);
	uint8_t vcall[NXDN_LAYER3_BYTES], rel[NXDN_LAYER3_BYTES];
	layer3_put(vcall, NXDN_MSG_VCALL, c->call_type, c->src, c->dst, c->cipher);
	layer3_put(rel, NXDN_MSG_TX_REL, c->call_type, c->src, c->dst, c->cipher);
	if (c->header)
		put_control(t, c->ran, vcall);
	t->first_voice_dibit = t->n;
	for (unsigned v = 0; v < c->voice_frames; v++)
		put_voice(t, c->ran, vcall, v & 3u, c->steal_one && v == 2u ? NXDN_STEAL_FACCH1_1 : NXDN_STEAL_NONE);
	if (c->release)
		put_control(t, c->ran, rel);
	for (unsigned i = 0; i < 24u; i++)
		t->dib[t->n++] = i & 1u ? 3u : 1u;
}

/* ---------------------------------------------------------------- 4FSK modulator to 8 kHz samples */

#define PSPAN	8	/* symbols each side */
#define PRES	100	/* pulse table points a symbol */
static double pulse_tab[2 * PSPAN * PRES + 1];

static double rrc_spectrum(double f)
{
	f = fabs(f);
	if (f <= 0.4)
		return 1.0;
	if (f >= 0.6)
		return 0.0;
	return sqrt(0.5 * (1.0 + cos(3.141592653589793 / 0.2 * (f - 0.4))));
}

static void pulse_init(void)
{
	/* RRC (0.2) x sin(x)/x, x = pi f (symbol units), by inverse transform; peak normalized to 1 */
	const unsigned bins = 600;
	const double df = 0.6 / bins;
	for (int j = -PSPAN * PRES; j <= PSPAN * PRES; j++) {
		double t = (double)j / PRES, acc = 0;
		for (unsigned k = 0; k <= bins; k++) {
			double f = k * df, x = 3.141592653589793 * f;
			double p = k ? sin(x) / x : 1.0;
			acc += (k == 0 || k == bins ? 1.0 : 2.0) * rrc_spectrum(f) * p * cos(6.283185307179586 * f * t);
		}
		pulse_tab[j + PSPAN * PRES] = acc * df;
	}
	double peak = pulse_tab[PSPAN * PRES];
	for (unsigned i = 0; i < sizeof pulse_tab / sizeof pulse_tab[0]; i++)
		pulse_tab[i] /= peak;
}

static double pulse(double t)
{
	if (t <= -PSPAN || t >= PSPAN)
		return 0;
	double x = (t + PSPAN) * PRES;
	unsigned i = (unsigned)x;
	double fr = x - i;
	return pulse_tab[i] * (1 - fr) + pulse_tab[i + 1] * fr;
}

typedef struct {
	double lsb_per_hz;	/* discriminator scale */
	double offset_hz;	/* carrier error: a DC offset */
	double noise_hz;	/* Gaussian noise, rms in Hz of deviation */
	double phase;		/* timing phase, symbols */
	double ppm;		/* symbol clock error */
	bool invert;
} channel_t;

/* n_pre / n_post: samples of noise before and after */
static int16_t *modulate(const tx_t *t, const channel_t *ch, unsigned n_pre, unsigned n_post, size_t *len)
{
	static const double hz[4] = { 350.0, 1050.0, -350.0, -1050.0 };
	double sym_per_sample = (double)NXDN_RS / NXDN_FS * (1.0 + ch->ppm * 1e-6);
	size_t body = (size_t)((t->n + 2 * PSPAN) / sym_per_sample) + 1u;
	size_t total = n_pre + body + n_post;
	int16_t *pcm = malloc(total * sizeof *pcm);
	for (size_t s = 0; s < total; s++) {
		double v = ch->offset_hz + ch->noise_hz * gauss();
		if (s >= n_pre && s < n_pre + body) {
			double ts = (double)(s - n_pre) * sym_per_sample + ch->phase - PSPAN;	/* symbol time */
			int k0 = (int)floor(ts) - PSPAN, k1 = (int)ceil(ts) + PSPAN;
			for (int k = k0 < 0 ? 0 : k0; k <= k1 && k < (int)t->n; k++)
				v += hz[t->dib[k]] * pulse(ts - k);
		}
		v *= ch->lsb_per_hz * (ch->invert ? -1.0 : 1.0);
		pcm[s] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : lrint(v));
	}
	*len = total;
	return pcm;
}

/* ---------------------------------------------------------------- the receiver under test */

typedef struct {
	uint8_t params[MAX_SLOTS][NXDN_AMBE_BITS];
	unsigned slots, bad_fec, calls, ends;
	nxdn_call_msg_t last;
} sink_t;

static void on_voice(void *ctx, const uint8_t slot[NXDN_SLOT_BYTES])
{
	sink_t *s = ctx;
	if (s->slots < MAX_SLOTS) {
		if (nxdn_ambe_params(slot, s->params[s->slots]) < 0)
			s->bad_fec++;
	}
	s->slots++;
}

static void on_event(void *ctx, unsigned ev, const nxdn_call_msg_t *m)
{
	sink_t *s = ctx;
	if (ev == NXDN_EV_CALL) {
		s->calls++;
		s->last = *m;
	} else if (ev == NXDN_EV_END) {
		s->ends++;
	}
}

static nxdn_receiver_t rxr;	/* static: like the firmware's */

static void receive(const int16_t *pcm, size_t n, sink_t *s, uint8_t ran, uint16_t group, uint16_t unit)
{
	memset(s, 0, sizeof *s);
	nxdn_rx_cfg_t cfg = { ran, group, unit, on_voice, on_event, s };
	nxdn_receiver_init(&rxr, &cfg, 0, 0);
	/* in blocks of 256 like the HR_C7000's RX RAM halves */
	for (size_t i = 0; i < n; i += 256u)
		nxdn_receiver_audio(&rxr, pcm + i, n - i < 256u ? n - i : 256u);
}

/* Voice fields received that equal the transmitted ones, from transmitted field skip on. Each received field is
 * aligned to the closest transmitted one within the next 12 (a lost frame shifts the rest; the first anywhere): the
 * parameter bits that differ in aligned fields add up in last_biterr, fields with no close match in last_unaligned */
static unsigned last_biterr, last_unaligned;
static unsigned voice_matches(const tx_t *t, const sink_t *s, unsigned skip)
{
	unsigned ok = 0, j = skip;
	last_biterr = last_unaligned = 0;
	for (unsigned i = 0; i < s->slots && i < MAX_SLOTS; i++) {
		unsigned best = 99, at = 0;
		for (unsigned k = j; k < t->slots && (i == 0 || k < j + 12u); k++) {
			unsigned d = 0;
			for (unsigned b = 0; b < NXDN_AMBE_BITS; b++)
				d += s->params[i][b] != t->params[k][b];
			if (d < best) {
				best = d;
				at = k;
			}
		}
		if (best > 12u) {
			last_unaligned++;
			continue;
		}
		ok += best == 0;
		last_biterr += best;
		j = at + 1u;
	}
	return ok;
}

static const channel_t clean = { 6.0, 0, 0, 0, 0, false };

static void run_call(const char *name, const call_t *c, channel_t ch, uint8_t ran, uint16_t group, uint16_t unit,
		     sink_t *s, tx_t *t)
{
	size_t n;
	build(t, c);
	int16_t *pcm = modulate(t, &ch, 4000, 4000, &n);
	receive(pcm, n, s, ran, group, unit);
	free(pcm);
	unsigned m = voice_matches(t, s, 0);
	printf("  %-24s slots %3u/%3u match %3u biterr %4u  calls %u ends %u frames %2u badlich %u badsacch %u"
	       " falsesync %u\n", name, s->slots, t->slots, m, last_biterr, s->calls, s->ends, (unsigned)rxr.rx.frames,
	       (unsigned)rxr.rx.bad_lich, (unsigned)rxr.rx.bad_sacch, (unsigned)rxr.rx.false_syncs);
}

static tx_t txa;
static sink_t sinka;

int main(void)
{
	printf("sizeof nxdn_demod_t %u, nxdn_rx_t %u, nxdn_receiver_t %u\n", (unsigned)sizeof(nxdn_demod_t),
	       (unsigned)sizeof(nxdn_rx_t), (unsigned)sizeof(nxdn_receiver_t));
	CHECK(sizeof(nxdn_receiver_t) <= 1024u);

	/* Scrambler constant = PN9 x^9 + x^4 + 1, seed 0xE4, one bit a symbol on its sign, after the FSW */
	{
		uint8_t m[NXDN_FRAME_BYTES] = { 0 };
		unsigned s = 0xE4;
		for (unsigned sym = NXDN_FSW_BITS / 2u; sym < NXDN_FRAME_SYMBOLS; sym++) {
			nxdn_set_bit(m, 2u * sym, s & 1u);
			s = (s >> 1) | (((s ^ (s >> 4)) & 1u) << 8);
		}
		CHECK(memcmp(m, nxdn_scrambler, sizeof m) == 0);
	}

	/* LICH parity: every byte; the 16-bit field with its fixed second bits */
	{
		unsigned good = 0;
		for (unsigned v = 0; v < 256u; v++) {
			nxdn_lich_t l;
			bool ok = nxdn_lich_unpack((uint8_t)v, &l);
			good += ok;
			CHECK(ok == (lich_pack(l.rfct, l.fct, l.option, l.direction) == v));
		}
		CHECK(good == 128u);
		uint8_t f[NXDN_FRAME_BYTES];
		frame_start(f, NXDN_USC_SACCH_SS, NXDN_STEAL_NONE);
		unsigned off = 9;
		CHECK(nxdn_lich_get(f, &off) == lich_pack(NXDN_RFCT_RDCH, NXDN_USC_SACCH_SS, NXDN_STEAL_NONE, 0) && off == 0);
	}

	/* Golay: up to 3 errors corrected; 4 in the 24-bit word always detected */
	for (unsigned it = 0; it < 3000u; it++) {
		uint16_t d = (uint16_t)(rnd() & 0xFFFu), got = 0;
		uint32_t c23 = golay23_encode(d), c24 = golay24_encode(d);
		unsigned ne = it % 4u;
		uint32_t e = 0;
		while (__builtin_popcount(e) < (int)ne)
			e |= 1u << (rnd() % 23u);
		CHECK(nxdn_golay23_decode(c23 ^ e, &got) == (int)ne && got == d);
		uint32_t e24 = 0;
		while (__builtin_popcount(e24) < (int)ne)
			e24 |= 1u << (rnd() % 24u);
		CHECK(nxdn_golay24_decode(c24 ^ e24, &got) == (int)ne && got == d);
		uint32_t e4 = 0;
		while (__builtin_popcount(e4) < 4)
			e4 |= 1u << (rnd() % 24u);
		CHECK(nxdn_golay24_decode(c24 ^ e4, &got) == -1);
	}

	/* AMBE+2 field: the 49 bits back through up to 3 errors in C0 and in C1 (C2/C3 errors pass through) */
	for (unsigned it = 0; it < 2000u; it++) {
		uint8_t p[NXDN_AMBE_BITS], q[NXDN_AMBE_BITS], slot[NXDN_SLOT_BYTES];
		for (unsigned i = 0; i < NXDN_AMBE_BITS; i++)
			p[i] = (uint8_t)(rnd() & 1u);
		ambe_encode(p, slot);
		CHECK(nxdn_ambe_params(slot, q) == 0 && memcmp(p, q, sizeof p) == 0);
		unsigned e0 = it % 4u, e1 = (it / 4u) % 4u;
		uint32_t m0 = 0, m1 = 0;
		while (__builtin_popcount(m0) < (int)e0)
			m0 |= 1u << (rnd() % 24u);
		while (__builtin_popcount(m1) < (int)e1)
			m1 |= 1u << (rnd() % 23u);
		for (unsigned n = 0; n < 47u; n++)
			if (n < 24u ? (m0 >> n) & 1u : (m1 >> (n - 24u)) & 1u)
				flip(slot, 4u * (n % 18u) + n / 18u);
		CHECK(nxdn_ambe_params(slot, q) == (int)(e0 + e1) && memcmp(p, q, sizeof p) == 0);
	}

	/* SACCH and FACCH1 through channel errors at distinct random places (punctured K=5 code: one error always
	 * corrected; more, mostly; tests/plus/test_nxdn_ref.cpp compares MMDVMHost's decoder). A decode that passes its
	 * CRC must be right */
	{
		uint16_t surv[NXDN_VIT_STEPS];
		unsigned sacch_ok[4] = { 0 }, facch_ok[6] = { 0 }, wrong = 0;
		for (unsigned it = 0; it < 6000u; it++) {
			uint8_t f[NXDN_FRAME_BYTES] = { 0 }, l3[NXDN_LAYER3_BYTES], got[NXDN_LAYER3_BYTES];
			uint8_t ran = (uint8_t)(rnd() & 63u), st = (uint8_t)(rnd() & 3u);
			uint32_t data = rnd() & 0x3FFFFu;
			sacch_put(f, ran, st, data);
			for (unsigned i = 0; i < NXDN_LAYER3_BYTES; i++)
				l3[i] = (uint8_t)rnd();
			unsigned which = it & 1u, es = it % 4u, ef = (it / 4u) % 6u;
			facch1_put(f, which, l3);
			uint64_t used = 0;
			for (unsigned e = 0; e < es;) {
				unsigned b = rnd() % 60u;
				if (!((used >> b) & 1u)) {
					used |= 1ull << b;
					flip(f, NXDN_SACCH_OFFSET + b);
					e++;
				}
			}
			uint8_t usedf[18] = { 0 };
			for (unsigned e = 0; e < ef;) {
				unsigned b = rnd() % 144u;
				if (!nxdn_bit(usedf, b)) {
					nxdn_set_bit(usedf, b, 1);
					flip(f, NXDN_PAYLOAD_OFFSET + which * 144u + b);
					e++;
				}
			}
			nxdn_sacch_t s;
			unsigned cor = 99;
			if (nxdn_sacch_decode(f, &s, &cor, surv)) {
				if (s.ran == ran && s.structure == st && s.data == data && cor == es)
					sacch_ok[es]++;
				else
					wrong++;
			}
			if (nxdn_facch1_decode(f, which, got, &cor, surv)) {
				if (memcmp(got, l3, sizeof got) == 0 && cor == ef)
					facch_ok[ef]++;
				else
					wrong++;
			}
		}
		printf("  SACCH decoded with 0-3 errors: %u %u %u %u of 1500; FACCH1 with 0-5: %u %u %u %u %u %u of 1000;"
		       " wrong but CRC good: %u\n", sacch_ok[0], sacch_ok[1], sacch_ok[2], sacch_ok[3], facch_ok[0],
		       facch_ok[1], facch_ok[2], facch_ok[3], facch_ok[4], facch_ok[5], wrong);
		CHECK(sacch_ok[0] == 1500u && sacch_ok[1] == 1500u && sacch_ok[2] >= 1300u);
		CHECK(facch_ok[0] == 1000u && facch_ok[1] == 1000u && facch_ok[2] >= 980u && facch_ok[3] >= 900u);
		CHECK(wrong <= 3u);
		uint8_t f[NXDN_FRAME_BYTES] = { 0 };
		nxdn_sacch_t s;
		sacch_put(f, 5, 1, 0x2AAAA);
		flip(f, 60);
		f[NXDN_SACCH_OFFSET / 8u + 2u] ^= 0xFFu;	/* 8 errors in a row: past the code, the CRC must say so */
		f[NXDN_SACCH_OFFSET / 8u + 3u] ^= 0xFFu;
		CHECK(!nxdn_sacch_decode(f, &s, NULL, surv) || (s.ran == 5 && s.data == 0x2AAAAu));
	}

	pulse_init();

	/* A clean group call: header, 12 voice frames (3 superframes), release */
	call_t c = { 7, NXDN_CALL_GROUP, 0, 1234, 100, 12, false, true, true };
	run_call("clean", &c, clean, 0, 0, 0, &sinka, &txa);
	CHECK(sinka.slots == txa.slots && voice_matches(&txa, &sinka, 0) == txa.slots && sinka.bad_fec == 0);
	CHECK(sinka.calls == 1 && sinka.last.source == 1234 && sinka.last.destination == 100 && sinka.ends == 1);
	CHECK(rxr.rx.releases == 1 && rxr.rx.calls == 1 && rxr.rx.late_entries == 0 && rxr.rx.last_ran == 7);
	/* header, 12 voice frames, release: 14 good frames (the flywheel then reads 3 more of the tail before it lets go) */
	CHECK(rxr.rx.frames - rxr.rx.bad_lich == 14u && rxr.rx.sync_losses == 1);

	/* The same through a worse channel: noise, a 150 Hz carrier error, 50 ppm clock error, an odd timing phase, a
	 * different scale; a FACCH1 VCALL steals half of the third voice frame */
	{
		channel_t ch = { 4.2, 150.0, 150.0, 0.37, 50.0, false };
		call_t c2 = { 7, NXDN_CALL_GROUP, 0, 65000, 2, 25, true, true, true };
		run_call("noise+offset+ppm", &c2, ch, 7, 2, 0, &sinka, &txa);
		CHECK(sinka.slots == txa.slots && voice_matches(&txa, &sinka, 0) == txa.slots);
		CHECK(sinka.calls == 1 && sinka.last.source == 65000 && sinka.last.destination == 2 && sinka.ends == 1);
	}

	/* Clock error (two radios' crystals far apart) and heavy noise: the voice degrades, the call info doesn't */
	{
		channel_t ch = { 6.0, 0, 0, 0.5, 300.0, false };
		call_t c2 = { 7, NXDN_CALL_GROUP, 0, 3000, 2, 40, false, true, true };
		run_call("300 ppm", &c2, ch, 0, 0, 0, &sinka, &txa);
		CHECK(sinka.slots == txa.slots && last_biterr * 100u < txa.slots * NXDN_AMBE_BITS);	/* BER < 1 % */
		ch = (channel_t){ 6.0, -100.0, 300.0, 0.2, 20.0, false };
		run_call("300 Hz rms noise", &c2, ch, 0, 0, 0, &sinka, &txa);
		CHECK(sinka.calls == 1 && sinka.last.source == 3000 && sinka.slots * 10u >= txa.slots * 9u);
		CHECK(last_unaligned == 0 && last_biterr * 50u < sinka.slots * NXDN_AMBE_BITS);	/* BER < 2 % */
	}

	/* Inverted discriminator (the RX RAM's sign is unknown on the DM-32) */
	{
		channel_t ch = clean;
		ch.invert = true;
		ch.noise_hz = 60;
		call_t c3 = { 1, NXDN_CALL_GROUP, 0, 42, 9, 8, false, true, true };
		run_call("inverted", &c3, ch, 0, 0, 0, &sinka, &txa);
		CHECK(rxr.rx.inverted && sinka.slots == txa.slots && voice_matches(&txa, &sinka, 0) == txa.slots);
		CHECK(sinka.last.source == 42 && sinka.last.destination == 9 && sinka.ends == 1);
	}

	/* Late entry: the receiver joins after the header and the first voice frame: the caller from four SACCH parts,
	 * the voice from the fifth frame on, nothing played before the caller is known */
	{
		call_t c4 = { 3, NXDN_CALL_GROUP, 0, 777, 55, 12, false, true, true };
		build(&txa, &c4);
		tx_t *late = malloc(sizeof *late);
		*late = txa;
		unsigned cut = txa.first_voice_dibit + NXDN_FRAME_SYMBOLS;	/* into the second voice frame */
		memmove(late->dib, late->dib + cut, late->n - cut);
		late->n -= cut;
		size_t n;
		channel_t ch = clean;
		ch.noise_hz = 60;
		int16_t *pcm = modulate(late, &ch, 4000, 4000, &n);
		receive(pcm, n, &sinka, 0, 0, 0);
		free(pcm);
		printf("  %-24s slots %3u calls %u late %u\n", "late entry", sinka.slots, sinka.calls,
		       (unsigned)rxr.rx.late_entries);
		CHECK(sinka.calls == 1 && sinka.last.source == 777 && sinka.last.destination == 55 && rxr.rx.late_entries == 1);
		/* the stream starts with voice frame 1: frames 1..4 carry parts 1,2,3,0 and the caller is known in frame 4
		 * (32 fields played), or a frame later if the demodulator needed frame 1 to acquire */
		CHECK(sinka.slots == 28u || sinka.slots == 32u);
		unsigned skip = txa.slots - sinka.slots;
		CHECK(voice_matches(&txa, &sinka, skip) == sinka.slots);
		free(late);
	}

	/* Filters: a foreign RAN never plays; another group is muted (caller still reported); encrypted never plays;
	 * an individual call plays only to its unit */
	{
		call_t c5 = { 9, NXDN_CALL_GROUP, 0, 11, 200, 8, false, true, true };
		run_call("foreign RAN", &c5, clean, 8, 0, 0, &sinka, &txa);
		CHECK(sinka.slots == 0 && rxr.rx.foreign_ran > 0);
		run_call("other group", &c5, clean, 9, 201, 0, &sinka, &txa);
		CHECK(sinka.slots == 0 && sinka.calls == 1 && sinka.last.destination == 200 && rxr.rx.slots_muted == 32u);
		run_call("group ALL", &(call_t){ 9, NXDN_CALL_GROUP, 0, 11, NXDN_GROUP_ALL, 4, false, true, true }, clean, 9,
			 201, 0, &sinka, &txa);
		CHECK(sinka.slots == 16u);
		call_t c6 = { 9, NXDN_CALL_GROUP, 1, 11, 200, 8, false, true, true };
		run_call("encrypted", &c6, clean, 0, 0, 0, &sinka, &txa);
		CHECK(sinka.slots == 0 && sinka.calls == 1 && sinka.last.cipher_type == 1);
		call_t c7 = { 9, NXDN_CALL_INDIVIDUAL, 0, 11, 4321, 4, false, true, true };
		run_call("individual, other unit", &c7, clean, 0, 200, 1234, &sinka, &txa);
		CHECK(sinka.slots == 0);
		run_call("individual, our unit", &c7, clean, 0, 200, 4321, &sinka, &txa);
		CHECK(sinka.slots == 16u && sinka.last.call_type == NXDN_CALL_INDIVIDUAL);
	}

	/* Two calls in one frame stream (a repeater's output) without a release between: the second header's caller
	 * replaces the first */
	{
		call_t a = { 2, NXDN_CALL_GROUP, 0, 100, 1, 4, false, true, false };
		call_t b = { 2, NXDN_CALL_GROUP, 0, 200, 1, 4, false, true, true };
		tx_t *tb = malloc(sizeof *tb);
		build(&txa, &a);
		build(tb, &b);
		txa.n -= 24u;		/* a's tail dotting, b's preamble: frame follows frame */
		memcpy(txa.dib + txa.n, tb->dib + 48u, tb->n - 48u);
		txa.n += tb->n - 48u;
		memcpy(txa.params[txa.slots], tb->params, tb->slots * sizeof tb->params[0]);
		txa.slots += tb->slots;
		size_t n;
		int16_t *pcm = modulate(&txa, &clean, 2000, 2000, &n);
		receive(pcm, n, &sinka, 0, 0, 0);
		free(pcm);
		free(tb);
		CHECK(sinka.calls == 2 && sinka.last.source == 200 && sinka.ends == 2 && sinka.slots == 32u);
		CHECK(voice_matches(&txa, &sinka, 0) == 32u);
	}

	/* Noise alone, 20 s, at three levels: no call, no voice, and the hunt keeps running */
	{
		static const double levels[3] = { 300.0, 1000.0, 3000.0 };
		for (unsigned k = 0; k < 3u; k++) {
			tx_t *none = calloc(1, sizeof *none);
			channel_t ch = { 6.0, 0, levels[k], 0, 0, false };
			size_t n;
			int16_t *pcm = modulate(none, &ch, 160000, 0, &n);
			receive(pcm, n, &sinka, 0, 0, 0);
			free(pcm);
			free(none);
			printf("  noise %4.0f Hz rms: frames %u falsesync %u calls %u slots %u onsets %u\n", levels[k],
			       (unsigned)rxr.rx.frames, (unsigned)rxr.rx.false_syncs, sinka.calls, sinka.slots,
			       (unsigned)rxr.demod.onsets);
			CHECK(sinka.calls == 0 && sinka.slots == 0);
		}
	}

	/* A call that follows 3 s of noise is still found (the demodulator re-acquires at the onset) */
	{
		call_t c8 = { 4, NXDN_CALL_GROUP, 0, 31337, 7, 8, false, true, true };
		build(&txa, &c8);
		channel_t ch = { 6.0, -80.0, 150.0, 0.8, -20.0, false };
		size_t n;
		int16_t *pcm = modulate(&txa, &ch, 24000, 4000, &n);
		receive(pcm, n, &sinka, 0, 0, 0);
		free(pcm);
		unsigned m = voice_matches(&txa, &sinka, 0);
		printf("  %-24s slots %3u/%3u match %3u biterr %4u  calls %u\n", "after 3 s of noise", sinka.slots, txa.slots,
		       m, last_biterr, sinka.calls);
		CHECK(sinka.calls == 1 && sinka.last.source == 31337 && sinka.slots == txa.slots && m == txa.slots);
	}

	if (failures) {
		printf("test_nxdn: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_nxdn: all passed\n");
	return 0;
}
