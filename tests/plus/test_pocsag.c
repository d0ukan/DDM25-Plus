// SPDX-License-Identifier: GPL-3.0-or-later
// DM32 Plus POCSAG decoder (integration/radiofoundry/msg/pocsag.c): transmissions made by this test's own encoder
// (ITU-R M.584-2) at 512, 1200 and 2400 bit/s as 8 kHz receive audio: flat (the discriminator), through a 300 Hz
// high pass, through de-emphasis, with noise, a DC offset, clipping, a clock offset and either polarity; numeric,
// alphanumeric, tone only and long messages; corrected and uncorrectable bit errors; noise and voice-like audio
// must not produce a message.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pocsag.h"

#define FS 8000.0
#define MAXS (8000 * 40)

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// --- messages out of the decoder ---
typedef struct {
	uint32_t capcode;
	uint8_t function, rate, kind;
	bool errors, cut;
	char text[100];
} got_t;
static got_t got[32];
static int ngot;
static void on_msg(void *ctx, const pocsag_msg_t *m)
{
	(void)ctx;
	if (ngot >= 32)
		return;
	got_t *g = &got[ngot++];
	g->capcode = m->capcode;
	g->function = m->function;
	g->rate = m->rate;
	g->errors = m->errors;
	g->cut = m->cut;
	g->kind = pocsag_text(m, g->text, sizeof g->text);
}

// --- the encoder: codewords of a transmission ---
static uint32_t cws[2048];
static int ncw;
static uint8_t txbits[70000];
static int nbits;
static uint32_t rng = 12345;
static double urand(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return (rng & 0xFFFFFF) / (double)0x1000000;
}
static double grand(void)
{
	double s = 0;
	for (int i = 0; i < 6; i++)
		s += urand();
	return (s - 3.0) * sqrt(2.0);
}

static void put_bit(int b)
{
	if (nbits < (int)sizeof txbits)
		txbits[nbits++] = (uint8_t)b;
}

static void put_word(uint32_t w)
{
	for (int k = 31; k >= 0; k--)
		put_bit((w >> k) & 1u);
}

// One page: its address in its frame, then its message codewords
typedef struct {
	uint32_t capcode;
	uint8_t function;
	int alpha;		// 1 alphanumeric, 0 numeric, -1 tone only
	const char *text;
} page_t;

static int msg_words(const page_t *p, uint32_t *out)
{
	uint8_t mb[2000];
	int n = 0;
	if (p->alpha < 0)
		return 0;
	for (const char *s = p->text; *s; s++) {
		unsigned c;
		int w;
		if (p->alpha) {
			c = (unsigned char)*s;
			w = 7;
		} else {
			const char *k = strchr("0123456789*U -][", *s);
			c = k ? (unsigned)(k - "0123456789*U -][") : 0;
			w = 4;
		}
		for (int b = 0; b < w; b++)
			mb[n++] = (uint8_t)((c >> b) & 1u);
	}
	int words = (n + 19) / 20;
	// fill: NUL bits for text, spaces (0xC) for numbers
	for (int i = n; i < words * 20; i++)
		mb[i] = p->alpha ? 0 : (uint8_t)((0xCu >> ((i - n) % 4)) & 1u);
	for (int w = 0; w < words; w++) {
		uint32_t info = 1u << 20;
		for (int b = 0; b < 20; b++)
			info |= (uint32_t)mb[w * 20 + b] << (19 - b);
		out[w] = pocsag_encode(info);
	}
	return words;
}

// The codewords of a transmission with these pages: preamble bits, then batches (sync + 16 words)
static void transmission(const page_t *pages, int npages, int preamble_bits)
{
	nbits = 0;
	for (int i = 0; i < preamble_bits; i++)
		put_bit(!(i & 1));
	ncw = 0;
	for (int p = 0; p < npages; p++) {
		int frame = (int)(pages[p].capcode & 7u);
		while (ncw % 16 != frame * 2)
			cws[ncw++] = POCSAG_IDLE;
		uint32_t info = ((pages[p].capcode >> 3) & 0x3FFFFu) << 2 | (pages[p].function & 3u);
		cws[ncw++] = pocsag_encode(info);
		ncw += msg_words(&pages[p], &cws[ncw]);
	}
	cws[ncw++] = POCSAG_IDLE;	// a message ends at an idle codeword
	while (ncw % 16 != 0)
		cws[ncw++] = POCSAG_IDLE;
	for (int i = 0; i < ncw; i++) {
		if (i % 16 == 0)
			put_word(POCSAG_SYNC);
		put_word(cws[i]);
	}
}

// Flip bit k (0 = MSB) of codeword i of the transmission (counted after the preamble, syncs left out)
static void flip(int preamble_bits, int i, int k)
{
	int at = preamble_bits + (i / 16 + 1) * 32 + i * 32 + k;
	txbits[at] ^= 1u;
}

// --- audio ---
static double sig[MAXS];
static int16_t pcm[MAXS];
static int len;

static void silence(double ms)
{
	int n = (int)(ms * FS / 1000.0);
	for (int i = 0; i < n && len < MAXS; i++)
		sig[len++] = 0;
}

// NRZ at rate x baud_k: +amp for a 1
static void nrz(double rate, double amp, double baud_k)
{
	double spb = FS / (rate * baud_k), start = len;
	for (int b = 0; b < nbits; b++) {
		int end = (int)(start + (b + 1) * spb + 0.5);
		while (len < end && len < MAXS)
			sig[len++] = txbits[b] ? amp : -amp;
	}
}

static void voice(double ms, double amp)
{
	int n = (int)(ms * FS / 1000.0);
	double y1[2] = { 0, 0 }, y2[2] = { 0, 0 }, ph = 0;
	for (int i = 0; i < n && len < MAXS; i++) {
		double f0 = 110 + 60 * sin(2 * M_PI * i / (FS * 0.7)) + 20 * sin(2 * M_PI * i / (FS * 0.13));
		ph += f0 / FS;
		double x = 0;
		if (ph >= 1) {
			ph -= 1;
			x = 1;
		}
		double fa = 700 + 300 * sin(2 * M_PI * i / (FS * 0.31)), fb = 1200 + 500 * sin(2 * M_PI * i / (FS * 0.23));
		double r = 0.97;
		double a1 = 2 * r * cos(2 * M_PI * fa / FS), b1 = 2 * r * cos(2 * M_PI * fb / FS);
		double v1 = x + a1 * y1[0] - r * r * y1[1];
		y1[1] = y1[0];
		y1[0] = v1;
		double v2 = v1 + b1 * y2[0] - r * r * y2[1];
		y2[1] = y2[0];
		y2[0] = v2;
		sig[len++] = amp * v2 / 40.0;
	}
}

// The receive path: FILT_FLAT (only a 3.4 kHz first-order low pass: the RX RAM's own band), FILT_HP (and a 300 Hz
// one-pole high pass: the FM path's sub-audio filter), FILT_DEEMPH (and 750 us de-emphasis, gain made up)
enum { FILT_FLAT, FILT_HP, FILT_DEEMPH };
static pocsag_t dec;
static void run(int filt, double noise_rms, int sign, double dc)
{
	double alp = 1.0 - exp(-2 * M_PI * 3400 / FS), ahp = 1.0 / (1.0 + 2 * M_PI * 300 / FS);
	double ade = 1.0 - exp(-1.0 / (750e-6 * FS));
	double lp = 0, hx = 0, hy = 0, de = 0;
	for (int i = 0; i < len; i++) {
		lp += alp * (sig[i] - lp);
		double y = lp;
		if (filt == FILT_HP) {
			hy = ahp * (hy + y - hx);
			hx = y;
			y = hy;
		} else if (filt == FILT_DEEMPH) {
			de += ade * (y - de);
			y = de;
		}
		double v = sign * y + noise_rms * grand() + dc;
		if (v > 32767)
			v = 32767;
		if (v < -32768)
			v = -32768;
		pcm[i] = (int16_t)lrint(v);
	}
	ngot = 0;
	pocsag_init(&dec, on_msg, NULL);
	int i = 0, k = 0;
	while (i < len) {
		int n = 17 + (k++ * 37) % 190;
		if (n > len - i)
			n = len - i;
		pocsag_audio(&dec, &pcm[i], (size_t)n);
		i += n;
	}
	pocsag_flush(&dec);
}

static const double rates[3] = { 512, 1200, 2400 };

static void make(const page_t *pages, int npages, int rate, double amp, double baud_k)
{
	len = 0;
	silence(300);
	transmission(pages, npages, 576);
	nrz(rates[rate], amp, baud_k);
	silence(300);
}

static int find(uint32_t capcode)
{
	for (int i = 0; i < ngot; i++)
		if (got[i].capcode == capcode)
			return i;
	return -1;
}

static const page_t p_num = { 1234567, 0, 0, "0123456789 -U[]" };
static const page_t p_alpha = { 2000001, 3, 1, "Hello DM32 Plus: pager test 123!" };
static const page_t p_tone = { 99, 1, -1, "" };
static const page_t p_long = { 1543210, 3, 1,
	"A long page that runs over the end of its batch into the next one, 79 chars ok" };

static int marked_ok;	// a page whose text came out right may carry the errors mark (a codeword after it was lost)
static int page_ok(const page_t *p, int rate)
{
	int i = find(p->capcode);
	if (i < 0)
		return 0;
	const got_t *g = &got[i];
	int kind = p->alpha < 0 ? POCSAG_TONE : p->alpha ? POCSAG_ALPHA : POCSAG_NUMERIC;
	return g->function == p->function && g->rate == rate && g->kind == kind && (marked_ok || !g->errors) &&
	       strcmp(g->text, p->text) == 0;
}

static void test_code(void)
{
	// the sync and idle codewords are code words; single and double errors anywhere correct, triple ones don't
	uint32_t c = POCSAG_SYNC;
	CHECK(pocsag_correct(&c) == 0 && c == POCSAG_SYNC, "sync not a code word");
	c = POCSAG_IDLE;
	CHECK(pocsag_correct(&c) == 0, "idle not a code word");
	uint32_t w = pocsag_encode(0x1ABCDEu);
	CHECK((w >> 11) == 0x1ABCDEu, "encode keeps the information bits: %08x", w);
	int bad = 0, triple_taken = 0;
	for (int i = 0; i < 32; i++) {
		for (int j = i; j < 32; j++) {
			uint32_t e = w ^ (1u << i) ^ (i != j ? 1u << j : 0);
			int r = pocsag_correct(&e);
			if (r != (i == j ? 1 : 2) || e != w)
				bad++;
		}
	}
	CHECK(bad == 0, "%d single / double errors not corrected", bad);
	for (int t = 0; t < 2000; t++) {
		int a = (int)(urand() * 32), b = (int)(urand() * 32), d = (int)(urand() * 32);
		if (a == b || b == d || a == d)
			continue;
		uint32_t e = w ^ (1u << a) ^ (1u << b) ^ (1u << d);
		int r = pocsag_correct(&e);
		if (r >= 0 && e == w)
			triple_taken++;
	}
	CHECK(triple_taken == 0, "%d triple errors 'corrected' to the sent word", triple_taken);
}

static void test_rates(void)
{
	page_t pages[3] = { p_num, p_alpha, p_tone };
	for (int rate = 0; rate < 3; rate++) {
		for (int filt = 0; filt < 3; filt++) {
			// de-emphasis turns 2400 bit/s NRZ into a triangle: only the flat and high passed paths are asked to
			// carry it (the Pager screen switches the flat path on)
			if (filt == FILT_DEEMPH && rate == POCSAG_2400)
				continue;
			for (int sign = -1; sign <= 1; sign += 2) {
				make(pages, 3, rate, 9000, 1.0);
				run(filt, 900, sign, sign > 0 ? 1500 : -800);
				marked_ok = filt != FILT_FLAT;
				int ok = page_ok(&p_num, rate) + page_ok(&p_alpha, rate) + page_ok(&p_tone, rate);
				marked_ok = 0;
				// flat: every page. Through de-emphasis 1200 bit/s smears (a single bit after a run reaches only a third of
				// the swing): most pages
				int need = filt == FILT_DEEMPH && rate == POCSAG_1200 ? 2 : 3;
				CHECK(ok >= need && ngot == 3, "rate %d filt %d sign %d: %d of 3 pages, %d messages (syncs %u corr %u bad %u)",
				      (int)rates[rate], filt, sign, ok, ngot, dec.syncs, dec.corrected, dec.uncorrectable);
				if (ok < need)
					for (int i = 0; i < ngot; i++)
						printf("  got %u f%u r%u k%u e%d \"%s\"\n", got[i].capcode, got[i].function, got[i].rate,
						       got[i].kind, got[i].errors, got[i].text);
			}
		}
	}
}

static void test_conditions(void)
{
	page_t pages[2] = { p_long, p_num };
	for (int rate = 0; rate < 3; rate++) {
		// a long message over two batches, a clock 0.3 % off, clipped audio (flat path at full gain)
		make(pages, 2, rate, 60000, 1.003);
		run(FILT_FLAT, 2000, 1, 0);
		CHECK(page_ok(&p_long, rate) && page_ok(&p_num, rate), "rate %d clipped/clock: %d messages", (int)rates[rate],
		      ngot);
		// noise: an SNR (signal peak to noise rms) of 4 (12 dB) on the flat path, every message
		make(pages, 2, rate, 4000, 0.998);
		run(FILT_FLAT, 1000, -1, 300);
		CHECK(page_ok(&p_long, rate) && page_ok(&p_num, rate), "rate %d at 12 dB: %d messages", (int)rates[rate],
		      ngot);
		// a weak signal still decodes most of the time: 10 runs at 8 dB
		int good = 0;
		for (int t = 0; t < 10; t++) {
			make(pages, 2, rate, 2500, 1.0);
			run(FILT_FLAT, 1000, 1, 0);
			good += page_ok(&p_long, rate) + page_ok(&p_num, rate);
		}
		printf("test_pocsag: %d bit/s at 8 dB: %d of 20 pages\n", (int)rates[rate], good);
		CHECK(good >= 14, "rate %d at 8 dB: %d of 20", (int)rates[rate], good);
	}
}

static void test_errors(void)
{
	page_t pages[1] = { p_alpha };
	for (int rate = 0; rate < 3; rate++) {
		// two bit errors in the address and in two message codewords: corrected, the page as sent
		len = 0;
		silence(200);
		transmission(pages, 1, 576);
		int addr = (int)(p_alpha.capcode & 7u) * 2;
		flip(576, addr, 3);
		flip(576, addr, 30);
		flip(576, addr + 1, 0);
		flip(576, addr + 2, 12);
		flip(576, addr + 2, 31);
		nrz(rates[rate], 8000, 1.0);
		silence(200);
		run(FILT_FLAT, 300, 1, 0);
		CHECK(page_ok(&p_alpha, rate), "rate %d corrected errors: %d messages", (int)rates[rate], ngot);
		CHECK(dec.corrected == 5, "rate %d corrected %u bits", (int)rates[rate], dec.corrected);
		// three in a message codeword: the page comes out marked
		len = 0;
		silence(200);
		transmission(pages, 1, 576);
		flip(576, addr + 3, 5);
		flip(576, addr + 3, 9);
		flip(576, addr + 3, 22);
		nrz(rates[rate], 8000, 1.0);
		silence(200);
		run(FILT_FLAT, 300, 1, 0);
		int i = find(p_alpha.capcode);
		CHECK(ngot == 1 && i == 0 && got[0].errors, "rate %d uncorrectable: %d messages, errors %d", (int)rates[rate],
		      ngot, i >= 0 ? got[i].errors : -1);
	}
	// a transmission whose preamble was missed (24 bits: too short for one) still decodes on an exact sync
	len = 0;
	silence(200);
	transmission(pages, 1, 24);
	nrz(1200, 8000, 1.0);
	silence(200);
	run(FILT_FLAT, 300, 1, 0);
	CHECK(page_ok(&p_alpha, POCSAG_1200), "no preamble: %d messages", ngot);
}

static void test_false(void)
{
	// white noise (an open receiver, no carrier) for 30 s at three levels, flat and high passed
	for (int lvl = 0; lvl < 3; lvl++) {
		len = 0;
		silence(30000);
		run(lvl == 1 ? FILT_HP : FILT_FLAT, lvl == 0 ? 300 : lvl == 1 ? 3000 : 12000, 1, 0);
		CHECK(ngot == 0, "noise %d: %d messages", lvl, ngot);
	}
	// voice-like audio, 20 s
	len = 0;
	voice(20000, 12000);
	run(FILT_FLAT, 200, 1, 0);
	CHECK(ngot == 0, "voice: %d messages", ngot);
	// random NRZ at each rate without a sync (other data: a different pager format)
	for (int rate = 0; rate < 3; rate++) {
		len = 0;
		nbits = 0;
		for (int i = 0; i < 576; i++)
			put_bit(!(i & 1));
		while (nbits < (int)(rates[rate] * 10))
			put_bit(urand() < 0.5);
		nrz(rates[rate], 8000, 1.0);
		run(FILT_FLAT, 500, 1, 0);
		CHECK(ngot == 0, "random NRZ at %d: %d messages", (int)rates[rate], ngot);
	}
}

int main(void)
{
	test_code();
	test_rates();
	test_conditions();
	test_errors();
	test_false();
	if (fails) {
		printf("test_pocsag: %d failures\n", fails);
		return 1;
	}
	printf("test_pocsag: ok (512 / 1200 / 2400 bit/s, numeric, alphanumeric, tone only, FEC, no false pages)\n");
	return 0;
}
