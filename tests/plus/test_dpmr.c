/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus dPMR receive (src/dpmr, docs/dpmr.md) host tests.
 *
 * The NXDN48 tests run first, whole (test_nxdn.c is included here for its 4FSK modulator, channel model, AMBE+2
 * encoder and NXDN frame builder): the demodulator both modes share still decodes NXDN.
 *
 * The transmit side is written here again from the standards, independently of src/dpmr: the scrambler as an LFSR,
 * the Hamming code from TS 102 490 Table 7.3's generator rows, the CRCs by long division, the interleave as the
 * matrix written by columns and read by rows (clause 7.5), the frames of clauses 4.2.2 and 9, the voice blocks as
 * DMR AMBE+2 air frames of known parameter bits. Whole transmissions go through the float modulator (root raised
 * cosine 0.2 x sin(x)/x, TS 102 490 12.2.2) to 8 kHz discriminator samples with noise, carrier error, clock error,
 * deviation error, de-emphasis and inverted polarity; the receiver must give the IDs, colour code, mode and every
 * voice block's 49 parameter bits, refuse a foreign colour code and data, and hear nothing in noise, speech-like FM
 * audio or NXDN. */
#define main nxdn_main
#include "test_nxdn.c"
#undef main
#include "dpmr/dpmr.h"
#include "dpmr/dpmr_rx.h"

/* ---------------------------------------------------------------- coding, test side */

static uint8_t scr[120];

static void scr_init(void)
{
	/* x^9 + x^5 + 1 drawn as a 9-stage register S1..S9, taps at S9 and S5, all ones: o[n] = o[n-9] ^ o[n-5] */
	for (unsigned n = 0; n < 120u; n++)
		scr[n] = n < 9u ? 1u : (uint8_t)(scr[n - 9u] ^ scr[n - 5u]);
}

/* TS 102 490 Table 7.3, rows X7 .. X1, "1": the parity bits C3 C2 C1 C0 each data bit adds */
static const uint8_t gen_rows[8] = { 0xE, 0x7, 0xA, 0x5, 0xB, 0xC, 0x6, 0x3 };

static uint16_t ham_encode(uint8_t d)
{
	uint8_t p = 0;
	for (unsigned k = 0; k < 8u; k++)
		if (d & (0x80u >> k))
			p ^= gen_rows[k];
	return (uint16_t)(d << 4 | p);
}

/* Remainder of bits[0..n) x^deg by the generator g (deg + 1 bits, top bit set), by long division */
static unsigned crc_div(const uint8_t *bytes, unsigned n, unsigned g, unsigned deg)
{
	uint8_t w[128] = { 0 };
	for (unsigned i = 0; i < n; i++)
		w[i] = (uint8_t)nxdn_bit(bytes, i);
	for (unsigned i = 0; i < n; i++)
		if (w[i])
			for (unsigned k = 0; k <= deg; k++)
				w[i + k] ^= (uint8_t)((g >> (deg - k)) & 1u);
	unsigned r = 0;
	for (unsigned k = 0; k < deg; k++)
		r = r << 1 | w[n + k];
	return r;
}

static void put_field(uint8_t *b, unsigned *at, uint32_t v, unsigned bits)
{
	for (unsigned i = 0; i < bits; i++)
		nxdn_set_bit(b, (*at)++, (v >> (bits - 1u - i)) & 1u);
}

/* data bytes -> Hamming words -> 12 x cols matrix written by columns, read by rows -> scrambled air bits */
static void block_encode(const uint8_t *data, unsigned cols, uint8_t *air)
{
	uint8_t m[12][10];
	for (unsigned c = 0; c < cols; c++) {
		uint16_t wd = ham_encode(data[c]);
		for (unsigned r = 0; r < 12u; r++)
			m[r][c] = (uint8_t)((wd >> (11u - r)) & 1u);
	}
	for (unsigned r = 0; r < 12u; r++)
		for (unsigned c = 0; c < cols; c++)
			nxdn_set_bit(air, r * cols + c, m[r][c] ^ scr[r * cols + c]);
}

static void hi_encode(const dpmr_hi_t *h, uint8_t air[15])
{
	uint8_t d[10] = { 0 };
	unsigned at = 0;
	put_field(d, &at, h->type, 4);
	put_field(d, &at, h->called, 24);
	put_field(d, &at, h->own, 24);
	put_field(d, &at, h->mode, 3);
	put_field(d, &at, h->format, 4);
	put_field(d, &at, h->flags, 2);
	put_field(d, &at, h->ci, 11);
	put_field(d, &at, crc_div(d, 72, 0x107, 8), 8);
	memset(air, 0, 15);
	block_encode(d, 10, air);
}

static void cch_encode(const dpmr_cch_t *c, uint8_t air[9])
{
	uint8_t d[6] = { 0 };
	unsigned at = 0;
	put_field(d, &at, c->fn, 2);
	put_field(d, &at, c->id, 12);
	put_field(d, &at, c->mode, 3);
	put_field(d, &at, c->format, 4);
	put_field(d, &at, c->flags, 2);
	put_field(d, &at, c->sld, 18);
	put_field(d, &at, crc_div(d, 41, 0x89, 7), 7);
	memset(air, 0, 9);
	block_encode(d, 6, air);
}

/* both END copies, 72 bits: three Hamming words, no interleave, the scrambler started again for each copy */
static void end_encode(const dpmr_end_t *e, uint8_t air[9])
{
	uint8_t d[3] = { 0 };
	unsigned at = 0;
	put_field(d, &at, e->type, 2);
	put_field(d, &at, e->arq, 2);
	put_field(d, &at, e->wait, 4);
	put_field(d, &at, e->status, 5);
	put_field(d, &at, 0, 4);
	put_field(d, &at, crc_div(d, 17, 0x89, 7), 7);
	memset(air, 0, 9);
	for (unsigned copy = 0; copy < 2u; copy++)
		for (unsigned w = 0; w < 3u; w++) {
			uint16_t wd = ham_encode(d[w]);
			for (unsigned b = 0; b < 12u; b++)
				nxdn_set_bit(air, 36u * copy + 12u * w + b, ((wd >> (11u - b)) & 1u) ^ scr[12u * w + b]);
		}
}

/* ---------------------------------------------------------------- transmissions */

static void put_bits(tx_t *t, const uint8_t *b, unsigned nbits)
{
	for (unsigned i = 0; i + 1u < nbits && t->n < MAX_DIBITS; i += 2u)
		t->dib[t->n++] = (uint8_t)(nxdn_bit(b, i) << 1 | nxdn_bit(b, i + 1u));
}

static void put_word(tx_t *t, uint64_t w, unsigned nbits)
{
	for (unsigned i = nbits; i >= 2u && t->n < MAX_DIBITS; i -= 2u)
		t->dib[t->n++] = (uint8_t)((w >> (i - 2u)) & 3u);
}

typedef struct {
	uint8_t cc, mode, format, flags;
	uint32_t called, own;
	unsigned superframes;
	bool header, end;
	unsigned data_frames;	/* mode 101: the last n frames carry data (SLD DP 11) */
} dcall_t;

static void put_header(tx_t *t, const dcall_t *c, uint8_t type)
{
	for (unsigned i = 0; i < 9u; i++)	/* the minimum preamble: 72 bits of 5F */
		put_word(t, DPMR_PREAMBLE, 8);
	put_word(t, DPMR_FS1, 48);
	dpmr_hi_t h = { type, c->called, c->own, c->mode, c->format, c->flags, 0 };
	uint8_t air[15];
	hi_encode(&h, air);
	put_bits(t, air, 120);
	put_word(t, dpmr_cc_codes[c->cc], 24);
	put_bits(t, air, 120);
}

/* frame fn of a superframe: sync field, CCH, four voice blocks (their parameter bits kept in t->params) */
static void put_dframe(tx_t *t, const dcall_t *c, unsigned fn, bool data)
{
	put_word(t, (fn & 1u) ? dpmr_cc_codes[c->cc] : DPMR_FS2, 24);
	uint32_t id = fn < 2u ? c->called : c->own;
	dpmr_cch_t ch = { (uint8_t)fn, (uint16_t)((fn & 1u) ? id & 0xFFFu : id >> 12), c->mode, c->format, c->flags,
			  data ? 3u << 11 : 0u };
	uint8_t air[9];
	cch_encode(&ch, air);
	put_bits(t, air, 72);
	for (unsigned k = 0; k < 4u; k++) {
		uint8_t slot[NXDN_SLOT_BYTES];
		uint8_t *p = t->params[t->slots < MAX_SLOTS ? t->slots : MAX_SLOTS - 1u];
		for (unsigned i = 0; i < NXDN_AMBE_BITS; i++)
			p[i] = (uint8_t)(rnd() & 1u);
		ambe_encode(p, slot);
		put_bits(t, slot, 72);
		if (!data)
			t->slots++;
	}
}

static void dbuild(tx_t *t, const dcall_t *c)
{
	memset(t, 0, sizeof *t);
	if (c->header)
		put_header(t, c, DPMR_HT_START);
	t->first_voice_dibit = t->n;
	unsigned frames = 4u * c->superframes;
	for (unsigned f = 0; f < frames; f++)
		put_dframe(t, c, f & 3u, f + c->data_frames >= frames);
	if (c->end) {
		put_word(t, DPMR_FS3, 24);
		dpmr_end_t e = { 0, 0, 0, 0 };
		uint8_t air[9];
		end_encode(&e, air);
		put_bits(t, air, 72);
	}
	for (unsigned i = 0; i < 24u; i++)	/* the carrier's tail */
		t->dib[t->n++] = i & 1u ? 3u : 1u;
}

/* ---------------------------------------------------------------- the receiver under test */

typedef struct {
	sink_t s;		/* first: test_nxdn.c's on_voice takes it */
	dpmr_call_t last;
	unsigned calls, ends;
} dsink_t;

static void on_devent(void *ctx, unsigned ev, const dpmr_call_t *c)
{
	dsink_t *d = ctx;
	if (ev == DPMR_EV_CALL) {
		d->calls++;
		d->last = *c;
	} else if (ev == DPMR_EV_END) {
		d->ends++;
	}
}

static dpmr_receiver_t drx;	/* static: like the firmware's */
static dsink_t dsink;
static uint16_t deemph_rx;	/* the receiver's de-emphasis inverse (0 = flat path) */

static void dreceive(const int16_t *pcm, size_t n, dsink_t *d, uint8_t cc)
{
	memset(d, 0, sizeof *d);
	dpmr_rx_cfg_t cfg = { cc, on_voice, on_devent, d };
	dpmr_receiver_init(&drx, &cfg, 0, deemph_rx);
	for (size_t i = 0; i < n; i += 256u)
		dpmr_receiver_audio(&drx, pcm + i, n - i < 256u ? n - i : 256u);
}

/* a receiver's de-emphasis (bilinear, the inverse nxdn_demod.c undoes) on the samples */
static void deemphasize(int16_t *pcm, size_t n, unsigned tau_us)
{
	double k = 16.0 * tau_us, a = (k - 1000.0) / (k + 1000.0), y = 0;
	for (size_t i = 0; i < n; i++) {
		y = a * y + (1.0 - a) * pcm[i];
		pcm[i] = (int16_t)lrint(y);
	}
}

static void drun(const char *name, const dcall_t *c, channel_t ch, uint8_t cc, unsigned deemph_tx, tx_t *t)
{
	size_t n;
	dbuild(t, c);
	int16_t *pcm = modulate(t, &ch, 4000, 4000, &n);
	if (deemph_tx)
		deemphasize(pcm, n, deemph_tx);
	dreceive(pcm, n, &dsink, cc);
	free(pcm);
	unsigned m = voice_matches(t, &dsink.s, 0);
	printf("  %-26s slots %3u/%3u match %3u biterr %4u  calls %u ends %u frames %2u hdr %u/%u badcch %u falsesync %u"
	       " foreign %u\n", name, dsink.s.slots, t->slots, m, last_biterr, dsink.calls, dsink.ends,
	       (unsigned)drx.rx.frames, (unsigned)drx.rx.headers, (unsigned)drx.rx.bad_hi, (unsigned)drx.rx.bad_cch, (unsigned)drx.rx.false_syncs,
	       (unsigned)drx.rx.foreign_cc);
}

static bool all_voice(const tx_t *t, const dsink_t *d)
{
	return d->s.slots == t->slots && voice_matches(t, &d->s, 0) == t->slots && d->s.bad_fec == 0;
}

static bool ids(const dsink_t *d, uint32_t called, uint32_t own)
{
	return d->last.have_called && d->last.have_own && d->last.called == called && d->last.own == own;
}

static int16_t *noise_audio(double rms_hz, size_t n, size_t *len)
{
	tx_t *none = calloc(1, sizeof *none);
	channel_t ch = { 6.0, 0, rms_hz, 0, 0, false };
	int16_t *pcm = modulate(none, &ch, (unsigned)n, 0, len);
	free(none);
	return pcm;
}

static tx_t dtx;

int main(void)
{
	int nx = nxdn_main();	/* NXDN48 first: the shared demodulator still decodes it */
	printf("sizeof dpmr_rx_t %u, dpmr_receiver_t %u (nxdn_receiver_t %u)\n", (unsigned)sizeof(dpmr_rx_t),
	       (unsigned)sizeof(dpmr_receiver_t), (unsigned)sizeof(nxdn_receiver_t));
	CHECK(sizeof(dpmr_receiver_t) <= sizeof(nxdn_receiver_t));	/* the firmware keeps them in one union */
	scr_init();

	/* The scrambler constant, the frame syncs (FS4 = FS1 inverted symbol by symbol), the CC table */
	{
		uint8_t m[15] = { 0 };
		for (unsigned i = 0; i < 120u; i++)
			nxdn_set_bit(m, i, scr[i]);
		CHECK(memcmp(m, dpmr_scrambler, sizeof m) == 0);
		CHECK((DPMR_FS1 ^ DPMR_SYMBOL_INVERT48) == DPMR_FS4);
		unsigned mind = 99;
		for (unsigned a = 0; a < DPMR_CC_COUNT; a++) {
			for (unsigned k = 0; k < 12u; k++) {
				unsigned d = (dpmr_cc_codes[a] >> (22u - 2u * k)) & 3u;
				CHECK(d == 1u || d == 3u);	/* +3 / -3 symbols only (clause 7.1) */
			}
			for (unsigned b = a + 1u; b < DPMR_CC_COUNT; b++) {
				unsigned d = (unsigned)__builtin_popcount(dpmr_cc_codes[a] ^ dpmr_cc_codes[b]);
				mind = d < mind ? d : mind;
			}
			unsigned e;
			CHECK(dpmr_cc_match(dpmr_cc_codes[a], 1, &e) == (int)a && e == 0);
			for (unsigned k = 0; k < 12u; k++)	/* one symbol wrong */
				CHECK(dpmr_cc_match(dpmr_cc_codes[a] ^ (2u << (2u * k)), 1, NULL) == (int)a);
		}
		CHECK(mind == 3u);
		CHECK(dpmr_cc_codes[1] == 0x577577u && dpmr_cc_codes[63] == 0xFDFD77u);	/* TS 102 490 Table 6.1.5 too */
	}

	/* CRCs and the Hamming code against the long division; every byte with every single error */
	{
		for (unsigned it = 0; it < 500u; it++) {
			uint8_t b[10];
			for (unsigned i = 0; i < sizeof b; i++)
				b[i] = (uint8_t)rnd();
			CHECK(dpmr_crc7(b, 41) == crc_div(b, 41, 0x89, 7));
			CHECK(dpmr_crc8(b, 72) == crc_div(b, 72, 0x107, 8));
		}
		for (unsigned d = 0; d < 256u; d++) {
			uint16_t w = ham_encode((uint8_t)d);
			uint8_t r = 0;
			unsigned rem = (unsigned)d << 4;	/* systematic cyclic code: parity = d x^4 mod x^4 + x + 1 */
			for (int i = 11; i >= 4; i--)
				if (rem & (1u << i))
					rem ^= 0x13u << (i - 4);
			CHECK((w & 0xFu) == rem && dpmr_hamming_parity((uint8_t)d) == rem);
			CHECK(dpmr_hamming_decode(w, &r) == 0 && r == d);
			for (unsigned e = 0; e < 12u; e++)
				CHECK(dpmr_hamming_decode((uint16_t)(w ^ (1u << e)), &r) == 1 && r == d);
		}
	}

	/* HI, CCH and END round trips; one error in each Hamming word corrected; IDs as text */
	{
		unsigned ok = 0, wrong = 0;
		for (unsigned it = 0; it < 2000u; it++) {
			dpmr_hi_t h = { (uint8_t)(rnd() & 15u), rnd() & 0xFFFFFFu, rnd() & 0xFFFFFFu, (uint8_t)(rnd() & 7u),
					(uint8_t)(rnd() & 15u), (uint8_t)(rnd() & 3u), (uint16_t)(rnd() & 0x7FFu) }, g;
			uint8_t air[15];
			hi_encode(&h, air);
			if (it & 1u)
				for (unsigned w = 0; w < 10u; w++) {	/* bit 12 w + b of the coded block sits at air b x 10 + w */
					unsigned b = rnd() % 12u;
					flip(air, b * 10u + w);
				}
			unsigned cor = 99;
			if (dpmr_hi_decode(air, &g, &cor) && g.type == h.type && g.called == h.called && g.own == h.own &&
			    g.mode == h.mode && g.format == h.format && g.flags == h.flags && g.ci == h.ci &&
			    cor == ((it & 1u) ? 10u : 0u))
				ok++;
			dpmr_cch_t c = { (uint8_t)(rnd() & 3u), (uint16_t)(rnd() & 0xFFFu), (uint8_t)(rnd() & 7u),
					 (uint8_t)(rnd() & 15u), (uint8_t)(rnd() & 3u), rnd() & 0x3FFFFu }, d;
			uint8_t ca[9];
			cch_encode(&c, ca);
			if (it & 2u)
				for (unsigned w = 0; w < 6u; w++)
					flip(ca, (rnd() % 12u) * 6u + w);
			if (dpmr_cch_decode(ca, &d, &cor) && d.fn == c.fn && d.id == c.id && d.mode == c.mode &&
			    d.format == c.format && d.flags == c.flags && d.sld == c.sld && cor == ((it & 2u) ? 6u : 0u))
				ok++;
			/* random blocks: a CRC must not pass often (Hamming + CRC7 / CRC8) */
			uint8_t junk[15];
			for (unsigned i = 0; i < sizeof junk; i++)
				junk[i] = (uint8_t)rnd();
			wrong += dpmr_cch_decode(junk, &d, NULL) + dpmr_hi_decode(junk, &g, NULL);
		}
		printf("  HI / CCH round trips with 0 and 1 error a word: %u of 4000; random blocks taken: %u of 4000\n", ok,
		       wrong);
		CHECK(ok == 4000u && wrong <= 30u);
		dpmr_end_t e = { 1, 1, 3, 17 }, f;
		uint8_t ea[9];
		end_encode(&e, ea);
		CHECK(dpmr_end_decode(ea, 0, &f) && f.type == 1 && f.arq == 1 && f.wait == 3 && f.status == 17);
		flip(ea, 5);
		flip(ea, 7);	/* two errors in the first copy's first word: the second copy */
		CHECK(!dpmr_end_decode(ea, 0, &f) || f.status != 17 || f.type != 1);
		CHECK(dpmr_end_decode(ea, 36, &f) && f.status == 17);
		/* TS 102 658 Table A.2; TS 102 490 8.1.1 common IDs */
		static const struct { uint32_t id; const char *s; } names[] = {
			{ 0x1B91FD, "1234567" }, { 0x68BF08, "468956*" }, { 0x02C00A, "012345*" }, { 0x02C00B, "0123460" },
			{ 0xDF6767, "999****" }, { 0xFFFFFF, "ALL" }, { 0x05FFFF, "CID 5" }, { 0xFEFFFF, "CID 254" },
			{ 0xE00000, "14680064" },
		};
		for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) {
			char s[12];
			size_t n = dpmr_id_text(names[i].id, s, sizeof s);
			CHECK(strcmp(s, names[i].s) == 0 && n == strlen(names[i].s));
		}
		char tiny[4];
		CHECK(dpmr_id_text(0x1B91FD, tiny, sizeof tiny) == 3u && strcmp(tiny, "123") == 0);
	}

	/* A clean voice call: header, 3 superframes (48 voice blocks), end frame; any colour code */
	dcall_t c = { 20, DPMR_M_VOICE, 1, 0, 0x1B91FD, 0x02C00B, 3, true, true, 0 };
	drun("clean", &c, clean, 0, 0, &dtx);
	CHECK(all_voice(&dtx, &dsink) && ids(&dsink, 0x1B91FD, 0x02C00B) && dsink.last.cc == 20);
	CHECK(dsink.ends == 1 && drx.rx.calls == 1 && drx.rx.headers == 1 && drx.rx.late_entries == 0);
	CHECK(drx.rx.end_frames == 1 && drx.rx.frames == 12u && drx.rx.bad_cch == 0 && !drx.rx.inverted);

	/* The channel's colour code plays; another is refused (header and frames) */
	drun("own CC", &c, clean, 21, 0, &dtx);
	CHECK(all_voice(&dtx, &dsink));
	drun("foreign CC", &c, clean, 6, 0, &dtx);
	CHECK(dsink.s.slots == 0 && drx.rx.foreign_cc > 0 && drx.rx.slots_played == 0);

	/* A worse channel: noise, 150 Hz carrier error, 50 ppm, an odd timing phase, 20 % less deviation. Here the
	 * header is lost (the demodulator is still settling on the 72-bit minimum preamble; dPMR's header has only a
	 * (12,8) code): the call comes by late entry, a superframe later */
	{
		channel_t ch = { 4.8, 150.0, 150.0, 0.37, 50.0, false };
		dcall_t c2 = { 9, DPMR_M_VOICE_SLD, 1, 0, 0x05FFFF, 0x05FFFF, 6, true, true, 0 };
		drun("noise+offset+ppm+dev", &c2, ch, 10, 0, &dtx);
		unsigned m = voice_matches(&dtx, &dsink.s, 0);
		CHECK(ids(&dsink, 0x05FFFF, 0x05FFFF) && dsink.last.cc == 9 && dsink.ends == 1);
		CHECK(dsink.s.slots * 10u >= dtx.slots * 8u && last_unaligned == 0 && m + 1u >= dsink.s.slots);
		/* the same channel 40 times (each a new noise, timing and start): the header nearly always, every call
		 * with its IDs, almost all voice */
		unsigned hdr = 0, idok = 0, slots = 0, total = 0, matched = 0;
		dcall_t c2b = c2;
		c2b.superframes = 2;
		for (unsigned k = 0; k < 40u; k++) {
			size_t n;
			dbuild(&dtx, &c2b);
			int16_t *pcm = modulate(&dtx, &ch, 4000u + rnd() % 800u, 4000, &n);
			dreceive(pcm, n, &dsink, 10);
			free(pcm);
			hdr += drx.rx.headers;
			idok += ids(&dsink, 0x05FFFF, 0x05FFFF);
			slots += dsink.s.slots;
			total += dtx.slots;
			matched += voice_matches(&dtx, &dsink.s, 0);
		}
		printf("  40 x noise+offset+ppm+dev: headers %u, IDs %u, voice %u of %u, exact %u\n", hdr, idok, slots, total,
		       matched);
		CHECK(hdr >= 36u && idok == 40u && slots * 100u >= total * 96u && matched * 100u >= slots * 99u);
	}

	/* 300 ppm clock error and heavy noise: the voice degrades, the call info doesn't */
	{
		channel_t ch = { 6.0, 0, 0, 0.5, 300.0, false };
		dcall_t c3 = { 63, DPMR_M_VOICE, 0, 0, DPMR_ID_ALL, 0x68BF08, 10, true, true, 0 };
		drun("300 ppm", &c3, ch, 64, 0, &dtx);
		CHECK(dsink.s.slots == dtx.slots && last_biterr * 100u < dtx.slots * NXDN_AMBE_BITS);
		CHECK(ids(&dsink, DPMR_ID_ALL, 0x68BF08) && dpmr_all_call(dsink.last.format));
		ch = (channel_t){ 6.0, -100.0, 300.0, 0.2, 20.0, false };
		drun("300 Hz rms noise", &c3, ch, 0, 0, &dtx);
		CHECK(dsink.calls >= 1 && ids(&dsink, DPMR_ID_ALL, 0x68BF08) && dsink.s.slots * 10u >= dtx.slots * 9u);
		CHECK(last_unaligned == 0 && last_biterr * 50u < dsink.s.slots * NXDN_AMBE_BITS);
	}

	/* De-emphasis in the receive path (750 us): with the receiver's inverse every block is exact; without it the
	 * call is still found (reported, not required) */
	{
		channel_t ch = { 6.0, 50.0, 60.0, 0.1, 10.0, false };
		dcall_t c4 = { 3, DPMR_M_VOICE, 1, 0, 0x000123, 0x000456, 4, true, true, 0 };
		deemph_rx = 750;
		drun("de-emphasis, undone", &c4, ch, 4, 750, &dtx);
		CHECK(all_voice(&dtx, &dsink) && ids(&dsink, 0x000123, 0x000456));
		deemph_rx = 0;
		drun("de-emphasis, not undone", &c4, ch, 4, 750, &dtx);
	}

	/* Inverted discriminator */
	{
		channel_t ch = clean;
		ch.invert = true;
		ch.noise_hz = 60;
		dcall_t c5 = { 33, DPMR_M_VOICE, 1, 0, 42, 4242, 2, true, true, 0 };
		drun("inverted", &c5, ch, 34, 0, &dtx);
		CHECK(drx.rx.inverted && all_voice(&dtx, &dsink) && ids(&dsink, 42, 4242) && dsink.ends == 1);
		/* a packet data header (FS4) in normal polarity is told from an inverted voice header and not followed */
		tx_t *pk = calloc(1, sizeof *pk);
		dcall_t cp = c5;
		put_header(pk, &cp, DPMR_HT_START);
		for (unsigned i = 0; i < 24u; i++)
			pk->dib[36u + i] ^= 2u;	/* FS1 -> FS4 */
		size_t n;
		int16_t *pcm = modulate(pk, &clean, 4000, 4000, &n);
		dreceive(pcm, n, &dsink, 0);
		free(pcm);
		free(pk);
		CHECK(drx.rx.data_headers == 1 && drx.rx.headers == 0 && dsink.calls == 0);
	}

	/* Late entry: the receiver joins in the second frame (no header). The first FS2 is frame 3's: tentative, its
	 * voice not played; frame 4's colour code confirms it. The own ID from frames 3 + 4, the called ID from the next
	 * superframe's 1 + 2 */
	{
		dcall_t c6 = { 12, DPMR_M_VOICE, 1, 0, 0x1B91FD, 0x02C00A, 4, true, true, 0 };
		dbuild(&dtx, &c6);
		tx_t *late = malloc(sizeof *late);
		*late = dtx;
		unsigned cut = dtx.first_voice_dibit + DPMR_FRAME_SYMBOLS;	/* the header and frame 1 */
		memmove(late->dib, late->dib + cut, late->n - cut);
		late->n -= cut;
		size_t n;
		channel_t ch = clean;
		ch.noise_hz = 60;
		int16_t *pcm = modulate(late, &ch, 4000, 4000, &n);
		dreceive(pcm, n, &dsink, 13);
		free(pcm);
		printf("  %-26s slots %3u calls %u late %u\n", "late entry", dsink.s.slots, dsink.calls,
		       (unsigned)drx.rx.late_entries);
		CHECK(drx.rx.late_entries == 1 && ids(&dsink, 0x1B91FD, 0x02C00A) && dsink.last.cc == 12);
		/* frames 2 (no FS2), 3 (tentative) not played: 16 frames - 3 = 13 played */
		CHECK(dsink.s.slots == 52u && voice_matches(&dtx, &dsink.s, dtx.slots - 52u) == 52u);
		free(late);
		/* late entry on a foreign colour code: nothing */
		c6.cc = 40;
		dbuild(&dtx, &c6);
		late = malloc(sizeof *late);
		*late = dtx;
		memmove(late->dib, late->dib + cut, late->n - cut);
		late->n -= cut;
		pcm = modulate(late, &ch, 4000, 4000, &n);
		dreceive(pcm, n, &dsink, 13);
		free(pcm);
		free(late);
		CHECK(dsink.s.slots == 0 && drx.rx.foreign_cc > 0);
	}

	/* Data: a type 1 data call is reported, never played; voice + appended data plays only the voice frames */
	{
		dcall_t c7 = { 5, DPMR_M_DATA1, 1, 0, 777, 888, 2, true, true, 0 };
		drun("data call", &c7, clean, 0, 0, &dtx);
		CHECK(dsink.s.slots == 0 && dsink.calls >= 1 && dsink.last.mode == DPMR_M_DATA1 && drx.rx.slots_muted == 32u);
		dcall_t c8 = { 5, DPMR_M_VOICE_DATA, 1, 0, 777, 888, 2, true, true, 2 };
		drun("voice + appended data", &c8, clean, 0, 0, &dtx);
		CHECK(all_voice(&dtx, &dsink) && dtx.slots == 24u && drx.rx.slots_muted == 8u);
	}

	/* Emergency (TS 102 658 flag) and an individual call: reported as sent */
	{
		dcall_t c9 = { 7, DPMR_M_VOICE, 1, 2, 1000, 2000, 1, true, true, 0 };
		drun("emergency", &c9, clean, 0, 0, &dtx);
		CHECK(all_voice(&dtx, &dsink) && dpmr_emergency(dsink.last.flags) && !dpmr_all_call(dsink.last.format));
	}

	/* Two callers back to back (no end frame between): the second header ends the first call */
	{
		dcall_t a = { 2, DPMR_M_VOICE, 1, 0, 100, 1, 1, true, false, 0 };
		dcall_t b = { 2, DPMR_M_VOICE, 1, 0, 100, 2, 1, true, true, 0 };
		tx_t *tb = malloc(sizeof *tb);
		dbuild(&dtx, &a);
		dbuild(tb, &b);
		dtx.n -= 24u;	/* a's tail */
		memcpy(dtx.dib + dtx.n, tb->dib, tb->n);
		dtx.n += tb->n;
		memcpy(dtx.params[dtx.slots], tb->params, tb->slots * sizeof tb->params[0]);
		dtx.slots += tb->slots;
		free(tb);
		size_t n;
		int16_t *pcm = modulate(&dtx, &clean, 2000, 2000, &n);
		dreceive(pcm, n, &dsink, 0);
		free(pcm);
		CHECK(drx.rx.calls == 2 && dsink.ends == 2 && dsink.last.own == 2 && all_voice(&dtx, &dsink));
	}

	/* Noise alone, 20 s at three levels; 20 s of speech-like FM audio; an NXDN call: no call, no voice */
	{
		static const double levels[3] = { 300.0, 1000.0, 3000.0 };
		for (unsigned k = 0; k < 3u; k++) {
			size_t n;
			int16_t *pcm = noise_audio(levels[k], 160000, &n);
			dreceive(pcm, n, &dsink, 0);
			free(pcm);
			printf("  noise %4.0f Hz rms: frames %u falsesync %u calls %u slots %u headers %u/%u\n", levels[k],
			       (unsigned)drx.rx.frames, (unsigned)drx.rx.false_syncs, dsink.calls, dsink.s.slots,
			       (unsigned)drx.rx.headers, (unsigned)drx.rx.bad_hi);
			CHECK(dsink.calls == 0 && dsink.s.slots == 0);
		}
		/* "speech": a 90-220 Hz voice with harmonics through two moving formants, syllables, 3 kHz peak deviation */
		size_t n = 160000;
		int16_t *pcm = malloc(n * sizeof *pcm);
		double ph = 0, f0 = 140, env = 0;
		for (size_t s = 0; s < n; s++) {
			if (s % 800u == 0)
				f0 = 90.0 + (rnd() % 130u);
			double t = s / 8000.0, syl = sin(6.283185307179586 * 4.0 * t), v = 0;
			env += ((syl > 0 ? syl : 0) - env) * 0.01;
			ph += 6.283185307179586 * f0 / 8000.0;
			double fa = 500 + 300 * sin(t * 2.1), fb = 1500 + 700 * sin(t * 1.3);
			for (unsigned h = 1; h * f0 < 3400.0; h++) {
				double f = h * f0, ga = 1.0 / (1.0 + pow((f - fa) / 150.0, 2)), gb = 0.5 / (1.0 + pow((f - fb) / 250.0, 2));
				v += (ga + gb) * sin(h * ph);
			}
			double hz = 1500.0 * env * v + 40.0 * gauss();
			pcm[s] = (int16_t)lrint(6.0 * (hz > 5000 ? 5000 : hz < -5000 ? -5000 : hz));
		}
		dreceive(pcm, n, &dsink, 0);
		free(pcm);
		printf("  speech-like FM 20 s: frames %u falsesync %u calls %u slots %u\n", (unsigned)drx.rx.frames,
		       (unsigned)drx.rx.false_syncs, dsink.calls, dsink.s.slots);
		CHECK(dsink.calls == 0 && dsink.s.slots == 0);
		/* NXDN48 (same symbol rate and deviation): the dPMR receiver hears no call ... */
		call_t nc = { 7, NXDN_CALL_GROUP, 0, 1234, 100, 40, false, true, true };
		build(&txa, &nc);
		pcm = modulate(&txa, &clean, 4000, 4000, &n);
		dreceive(pcm, n, &dsink, 0);
		printf("  NXDN48 call into dPMR: frames %u falsesync %u calls %u slots %u\n", (unsigned)drx.rx.frames,
		       (unsigned)drx.rx.false_syncs, dsink.calls, dsink.s.slots);
		CHECK(dsink.calls == 0 && dsink.s.slots == 0);
		/* ... and the NXDN receiver, still the same, hears it whole */
		receive(pcm, n, &sinka, 0, 0, 0);
		free(pcm);
		CHECK(sinka.slots == txa.slots && voice_matches(&txa, &sinka, 0) == txa.slots && sinka.last.source == 1234);
		/* dPMR into the NXDN receiver: no NXDN call */
		dbuild(&dtx, &c);
		pcm = modulate(&dtx, &clean, 4000, 4000, &n);
		receive(pcm, n, &sinka, 0, 0, 0);
		free(pcm);
		CHECK(sinka.calls == 0 && sinka.slots == 0);
	}

	/* A call after 3 s of noise is still found (the demodulator re-acquires at the onset) */
	{
		dcall_t c10 = { 30, DPMR_M_VOICE, 1, 0, 31337, 7, 2, true, true, 0 };
		dbuild(&dtx, &c10);
		channel_t ch = { 6.0, -80.0, 150.0, 0.8, -20.0, false };
		size_t n;
		int16_t *pcm = modulate(&dtx, &ch, 24000, 4000, &n);
		dreceive(pcm, n, &dsink, 31);
		free(pcm);
		unsigned m = voice_matches(&dtx, &dsink.s, 0);
		printf("  %-26s slots %3u/%3u match %3u biterr %4u  calls %u\n", "after 3 s of noise", dsink.s.slots,
		       dtx.slots, m, last_biterr, dsink.calls);
		CHECK(ids(&dsink, 31337, 7) && dsink.s.slots == dtx.slots && m == dtx.slots);
	}

	if (failures || nx) {
		printf("test_dpmr: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_dpmr: all passed\n");
	return 0;
}
