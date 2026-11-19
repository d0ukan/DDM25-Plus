// SPDX-License-Identifier: GPL-3.0-or-later
// DM32 Plus ACARS decoder (integration/radiofoundry/msg/acars.c): blocks made by this test's own encoder (the
// ARINC 618 character format as acars.h describes it) as 8 kHz AM receive audio: MSK at 2400 bit/s on 1200 / 2400 Hz,
// each of the four ways bits can map to tones, with the AM detector's DC, a voice band filter, a tilt, noise, a
// clock offset and a weak signal; uplink and downlink blocks, the text cut at 79 characters; noise, voice-like audio
// and a corrupted block must not produce a message.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "acars.h"

#define FS 8000.0
#define MAXS (8000 * 30)

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static acars_msg_t got[16];
static int ngot;
static void on_msg(void *ctx, const acars_msg_t *m)
{
	(void)ctx;
	if (ngot < 16)
		got[ngot++] = *m;
}

static uint32_t rng = 777;
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

// --- the encoder: the block's bytes ---
static uint8_t bytes[3200];
static int nbytes;

static uint8_t odd(uint8_t c)
{
	uint8_t v = c & 0x7Fu;
	int n = 0;
	for (int k = 0; k < 7; k++)
		n += (v >> k) & 1;
	return (uint8_t)(v | ((n & 1) ? 0 : 0x80u));
}

typedef struct {
	char mode;
	const char *addr;	// 7 characters
	char ack;
	const char *label;	// 2
	char block;
	const char *text;	// NULL: no text (ETX right after the block ID)
	int etb;
} block_t;

static void encode(const block_t *b, int corrupt_at)
{
	nbytes = 0;
	for (int i = 0; i < 16; i++)
		bytes[nbytes++] = 0xFF;	// pre-key
	bytes[nbytes++] = odd('+');
	bytes[nbytes++] = odd('*');
	bytes[nbytes++] = odd(0x16);
	bytes[nbytes++] = odd(0x16);
	bytes[nbytes++] = odd(0x01);
	int from = nbytes;
	bytes[nbytes++] = odd((uint8_t)b->mode);
	for (int i = 0; i < 7; i++)
		bytes[nbytes++] = odd((uint8_t)b->addr[i]);
	bytes[nbytes++] = odd((uint8_t)b->ack);
	bytes[nbytes++] = odd((uint8_t)b->label[0]);
	bytes[nbytes++] = odd((uint8_t)b->label[1]);
	bytes[nbytes++] = odd((uint8_t)b->block);
	if (b->text != NULL) {
		bytes[nbytes++] = odd(0x02);
		for (const char *s = b->text; *s; s++)
			bytes[nbytes++] = odd((uint8_t)*s);
	}
	bytes[nbytes++] = odd(b->etb ? 0x17 : 0x03);
	uint16_t crc = 0;
	for (int i = from; i < nbytes; i++)
		crc = acars_crc(crc, bytes[i]);
	bytes[nbytes++] = (uint8_t)(crc & 0xFFu);
	bytes[nbytes++] = (uint8_t)(crc >> 8);
	bytes[nbytes++] = odd(0x7F);
	if (corrupt_at > 0)
		bytes[from + corrupt_at] ^= 0x41u;	// two bits: the parity still holds, the BCS doesn't
}

// --- audio ---
static double sig[MAXS];
static int16_t pcm[MAXS];
static int len;
static double phase;

static void silence(double ms)
{
	int n = (int)(ms * FS / 1000.0);
	for (int i = 0; i < n && len < MAXS; i++)
		sig[len++] = 0;
}

// MSK: map 0: a 1 bit is 1200 Hz; 1: a 1 bit is 2400 Hz; 2: a 1 bit changes the tone; 3: a 0 bit changes it
static void msk(int map, double amp, double baud_k)
{
	double spb = FS / (2400.0 * baud_k), start = len;
	int tone = 0;	// 0: 1200 Hz
	int nb = 0;
	for (int i = 0; i < nbytes; i++) {
		for (int k = 0; k < 8; k++, nb++) {
			int b = (bytes[i] >> k) & 1;
			if (map == 0)
				tone = !b;
			else if (map == 1)
				tone = b;
			else if (map == 2)
				tone ^= b;
			else
				tone ^= !b;
			double f = tone ? 2400.0 : 1200.0;
			int end = (int)(start + (nb + 1) * spb + 0.5);
			while (len < end && len < MAXS) {
				sig[len++] = amp * sin(phase);
				phase += 2 * M_PI * f * baud_k / FS;
			}
		}
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

// The receive path: the AM detector's DC, a 300 Hz high pass and a 3 kHz low pass (the voice band), tilt: a further
// first-order low pass at 1 kHz (2400 Hz about 4 dB under 1200 Hz), noise
static acars_t dec;
static void run(int tilt, double noise_rms, double dc)
{
	double ahp = 1.0 / (1.0 + 2 * M_PI * 300 / FS), alp = 1.0 - exp(-2 * M_PI * 3000 / FS);
	double atl = 1.0 - exp(-2 * M_PI * 1000 / FS);
	double hx = 0, hy = 0, lp = 0, tl = 0;
	for (int i = 0; i < len; i++) {
		double x = sig[i] + dc;
		hy = ahp * (hy + x - hx);
		hx = x;
		lp += alp * (hy - lp);
		double y = lp;
		if (tilt) {
			tl += atl * (y - tl);
			y = tl * 1.8;
		}
		double v = y + noise_rms * grand() + dc * 0.1;
		if (v > 32767)
			v = 32767;
		if (v < -32768)
			v = -32768;
		pcm[i] = (int16_t)lrint(v);
	}
	ngot = 0;
	acars_init(&dec, on_msg, NULL);
	int i = 0, k = 0;
	while (i < len) {
		int n = 17 + (k++ * 37) % 190;
		if (n > len - i)
			n = len - i;
		acars_audio(&dec, &pcm[i], (size_t)n);
		i += n;
	}
}

static const block_t down = { '2', ".TC-JFV", 0x15, "H1", '4', "M01ATK1234#DFB/PER TEST ALT 37000 FUEL 12.3", 0 };
static const block_t up = { '2', ".N123AB", 'A', "_\x7f", 'B', NULL, 0 };
static const block_t up_text = { 'E', "..G-ABC", 0x15, "QA", 'C', "AN UPLINK WITH TEXT\r\nON TWO LINES", 1 };

static int down_ok(const acars_msg_t *m)
{
	return m->crc_ok && m->mode == '2' && strcmp(m->reg, "TC-JFV") == 0 && strcmp(m->label, "H1") == 0 &&
	       m->block == '4' && strcmp(m->msn, "M01A") == 0 && strcmp(m->flight, "TK1234") == 0 &&
	       strcmp(m->text, "#DFB/PER TEST ALT 37000 FUEL 12.3") == 0 && !m->more && !m->cut;
}

static void make(const block_t *b, int map, double amp, double baud_k)
{
	len = 0;
	phase = 0;
	silence(200);
	encode(b, 0);
	msk(map, amp, baud_k);
	silence(200);
}

static void test_blocks(void)
{
	for (int map = 0; map < 4; map++) {
		for (int tilt = 0; tilt < 2; tilt++) {
			make(&down, map, 8000, 1.0);
			run(tilt, 300, map & 1 ? 2000 : -1500);
			CHECK(ngot == 1 && down_ok(&got[0]), "map %d tilt %d: %d messages (syncs %u crc bad %u bits %u)", map, tilt,
			      ngot, dec.syncs, dec.crc_bad, dec.bits);
			if (ngot == 1 && !down_ok(&got[0]))
				printf("  got %c '%s' %s %c '%s' '%s' crc %d \"%s\"\n", got[0].mode, got[0].reg, got[0].label,
				       got[0].block, got[0].msn, got[0].flight, got[0].crc_ok, got[0].text);
		}
	}
	// an uplink without text (label "_" DEL), one with text over two lines that more blocks follow (ETB)
	make(&up, 2, 8000, 1.0);
	run(0, 300, 0);
	CHECK(ngot == 1 && got[0].crc_ok && strcmp(got[0].reg, "N123AB") == 0 && strcmp(got[0].label, "_d") == 0 &&
	      got[0].block == 'B' && got[0].text[0] == 0 && got[0].flight[0] == 0, "uplink: %d messages", ngot);
	make(&up_text, 2, 8000, 1.0);
	run(0, 300, 0);
	CHECK(ngot == 1 && got[0].crc_ok && strcmp(got[0].reg, "G-ABC") == 0 && got[0].more &&
	      strcmp(got[0].text, "AN UPLINK WITH TEXT  ON TWO LINES") == 0, "uplink text: %d messages \"%s\"", ngot,
	      ngot ? got[0].text : "");
	// a long text: the first 79 characters, marked cut
	static char longtext[221];
	for (int i = 0; i < 220; i++)
		longtext[i] = (char)('A' + i % 26);
	longtext[220] = 0;
	block_t lb = up_text;
	lb.text = longtext;
	lb.etb = 0;
	make(&lb, 2, 8000, 1.0);
	run(0, 300, 0);
	CHECK(ngot == 1 && got[0].crc_ok && got[0].cut && strlen(got[0].text) == ACARS_TEXT_MAX - 1 &&
	      strncmp(got[0].text, longtext, ACARS_TEXT_MAX - 1) == 0, "long text: %d messages", ngot);
	// two blocks back to back (a downlink and its acknowledgement)
	len = 0;
	phase = 0;
	silence(100);
	encode(&down, 0);
	msk(2, 8000, 1.0);
	silence(150);
	encode(&up, 0);
	msk(2, 8000, 1.0);
	silence(100);
	run(1, 300, 500);
	CHECK(ngot == 2 && down_ok(&got[0]) && strcmp(got[1].reg, "N123AB") == 0, "two blocks: %d messages", ngot);
}

static void test_conditions(void)
{
	// clock 0.2 % off each way, clipped audio
	make(&down, 2, 8000, 1.002);
	run(1, 400, 0);
	CHECK(ngot == 1 && down_ok(&got[0]), "fast clock: %d messages", ngot);
	make(&down, 3, 8000, 0.998);
	run(0, 400, 0);
	CHECK(ngot == 1 && down_ok(&got[0]), "slow clock: %d messages", ngot);
	make(&down, 2, 50000, 1.0);
	run(0, 400, 0);
	CHECK(ngot == 1 && down_ok(&got[0]), "clipped: %d messages", ngot);
	// noise (white over the whole 4 kHz): tone peak to noise rms 6 (15.6 dB) every block of 60 characters; at 5.5
	// (14.8 dB) most of them (the threshold is near 14 dB: one bit error in 500 loses the block)
	int good = 0;
	for (int t = 0; t < 10; t++) {
		make(&down, 2, 6000, 1.0);
		run(t & 1, 1000, 0);
		good += ngot == 1 && down_ok(&got[0]);
	}
	CHECK(good == 10, "15.6 dB: %d of 10", good);
	good = 0;
	for (int t = 0; t < 10; t++) {
		make(&down, 2, 5500, 1.0);
		run(t & 1, 1000, 0);
		good += ngot == 1 && down_ok(&got[0]);
	}
	printf("test_acars: 14.8 dB: %d of 10 blocks\n", good);
	CHECK(good >= 6, "14.8 dB: %d of 10", good);
}

static void test_false(void)
{
	// noise for 20 s at three levels
	for (int lvl = 0; lvl < 3; lvl++) {
		len = 0;
		silence(20000);
		run(0, lvl == 0 ? 200 : lvl == 1 ? 3000 : 12000, 0);
		CHECK(ngot == 0, "noise %d: %d messages", lvl, ngot);
	}
	// voice-like audio, 20 s
	len = 0;
	voice(20000, 12000);
	run(0, 300, 0);
	CHECK(ngot == 0, "voice: %d messages", ngot);
	// random MSK without a sync
	len = 0;
	phase = 0;
	nbytes = 0;
	while (nbytes < 3000)
		bytes[nbytes++] = (uint8_t)(urand() * 256);
	msk(2, 8000, 1.0);
	run(0, 300, 0);
	CHECK(ngot == 0, "random MSK: %d messages", ngot);
	// a block whose BCS doesn't match (two bits of a character wrong, its parity still right) shows as unchecked;
	// one with a parity error doesn't show
	len = 0;
	phase = 0;
	encode(&down, 20);
	msk(2, 8000, 1.0);
	silence(100);
	run(0, 300, 0);
	CHECK(ngot == 1 && !got[0].crc_ok, "BCS error: %d messages, crc_ok %d", ngot, ngot ? got[0].crc_ok : -1);
	len = 0;
	phase = 0;
	encode(&down, 0);
	bytes[16 + 5 + 20] ^= 0x01u;
	msk(2, 8000, 1.0);
	silence(100);
	run(0, 300, 0);
	CHECK(ngot == 0, "parity error: %d messages", ngot);
}

int main(void)
{
	test_blocks();
	test_conditions();
	test_false();
	if (fails) {
		printf("test_acars: %d failures\n", fails);
		return 1;
	}
	printf("test_acars: ok (MSK 2400 bit/s, four bit mappings, downlink / uplink, BCS, no false blocks)\n");
	return 0;
}
