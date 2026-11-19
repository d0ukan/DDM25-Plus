// SPDX-License-Identifier: GPL-3.0-or-later
// DM32 Plus SSTV decoder (integration/radiofoundry/sstv/sstv.c): pictures made by this test's own encoder (written
// from the published mode timings, independent of the decoder's tables) as 8 kHz receive audio: every mode with its
// VIS; Robot 36, PD120, Martin M1 and Scottie S1 also with noise, a +-0.3 % sample clock offset (the slant), a
// frequency offset, de-emphasized audio and without the VIS (start on the line syncs). Each decoded line is compared
// with the test pattern (colour bars over a colour gradient), mean absolute error per colour channel. Noise and
// voice-like audio must not start a picture. The display dither keeps its colours and the mean level. The radio task
// glue (radio_sstv.c) hands every display row once, in order, dithered close to the pattern.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sstv.h"
#include "radio/radio_sstv.h"	// the radio task glue, built with the audio tests' RTOS stubs

void audio_test_enter(void) {}
void audio_test_exit(void) {}

#define FS 8000.0
#define MAXS (8000 * 290)

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// --- the modes as the encoder knows them ---
enum { E_ROBOT36, E_ROBOT72, E_PD, E_MARTIN, E_SCOTTIE };
typedef struct {
	const char *name;
	int mode;		// the decoder's
	int vis, enc, w, h;	// picture width, lines
	double scan;		// ms: one colour scan (PD, Martin, Scottie)
} emode_t;
static const emode_t emodes[] = {
	{ "Robot 36", SSTV_ROBOT36, 8, E_ROBOT36, 320, 240, 0 },
	{ "Robot 72", SSTV_ROBOT72, 12, E_ROBOT72, 320, 240, 0 },
	{ "PD50", SSTV_PD50, 93, E_PD, 320, 256, 91.52 },
	{ "PD90", SSTV_PD90, 99, E_PD, 320, 256, 170.24 },
	{ "PD120", SSTV_PD120, 95, E_PD, 640, 496, 121.6 },
	{ "PD180", SSTV_PD180, 96, E_PD, 640, 496, 183.04 },
	{ "PD240", SSTV_PD240, 97, E_PD, 640, 496, 244.48 },
	{ "Martin M1", SSTV_MARTIN1, 44, E_MARTIN, 320, 256, 146.432 },
	{ "Martin M2", SSTV_MARTIN2, 40, E_MARTIN, 320, 256, 73.216 },
	{ "Scottie S1", SSTV_SCOTTIE1, 60, E_SCOTTIE, 320, 256, 138.24 },
	{ "Scottie S2", SSTV_SCOTTIE2, 56, E_SCOTTIE, 320, 256, 88.064 },
	{ "Scottie DX", SSTV_SCOTTIEDX, 76, E_SCOTTIE, 320, 256, 345.6 },
};
#define NMODES ((int)(sizeof emodes / sizeof emodes[0]))

// --- the test pattern: eight colour bars over the top half, a colour gradient below ---
static void pattern(int x, int y, int w, int h, double rgb[3])
{
	if (y < h / 2) {
		static const int bars[8][3] = { { 255, 255, 255 }, { 255, 255, 0 }, { 0, 255, 255 }, { 0, 255, 0 },
						{ 255, 0, 255 }, { 255, 0, 0 }, { 0, 0, 255 }, { 0, 0, 0 } };
		int b = x * 8 / w;
		for (int c = 0; c < 3; c++)
			rgb[c] = bars[b][c] * 0.85 + 20;	// inside the range, so clipping hides no error
		return;
	}
	rgb[0] = 20 + 215.0 * x / (w - 1);
	rgb[1] = 20 + 215.0 * (y - h / 2) / (h / 2 - 1);
	rgb[2] = 235 - 215.0 * x / (w - 1);
}

static void yuv(const double rgb[3], double *y, double *u, double *v)
{
	*y = 0.299 * rgb[0] + 0.587 * rgb[1] + 0.114 * rgb[2];
	*u = 128 + (rgb[2] - *y) / 1.772;
	*v = 128 + (rgb[0] - *y) / 1.402;
}

// --- the encoder: phase continuous FM tones ---
static int16_t audio[MAXS];
static int ns;
static double t_send, ph, stretch = 1.0, f_off = 0, amp = 12000;

static void tone(double hz, double ms)
{
	double end = t_send + ms / 1000.0;
	// a receiver sample every 1 / 8000 s of its own clock: stretch = its clock / the sender's
	while (ns < MAXS && ns / (FS * stretch) < end) {
		audio[ns++] = (int16_t)lrint(amp * sin(ph));
		ph += 2 * M_PI * (hz + f_off) / (FS * stretch);
		if (ph > 2 * M_PI)
			ph -= 2 * M_PI;
	}
	t_send = end;
}

static double level_hz(double v)
{
	return 1500 + 800 * (v < 0 ? 0 : v > 255 ? 255 : v) / 255;
}

static void vis_code(int code)
{
	tone(1900, 300);
	tone(1200, 10);
	tone(1900, 300);
	tone(1200, 30);
	int ones = 0;
	for (int b = 0; b < 7; b++) {
		int one = (code >> b) & 1;
		ones += one;
		tone(one ? 1100 : 1300, 30);
	}
	tone(ones & 1 ? 1100 : 1300, 30);
	tone(1200, 30);
}

// one colour scan of line y: channel 0..2 RGB, 3 Y, 4 U (B-Y), 5 V (R-Y); pairs: chroma of y and y + 1
static void scan(const emode_t *m, int y, int chan, double ms, int px, bool pair)
{
	for (int i = 0; i < px; i++) {
		int x = i * m->w / px;
		double v = 0;
		for (int k = 0; k < (pair ? 2 : 1); k++) {
			double rgb[3], Y, U, V;
			pattern(x, y + k, m->w, m->h, rgb);
			yuv(rgb, &Y, &U, &V);
			v += chan < 3 ? rgb[chan] : chan == 3 ? Y : chan == 4 ? U : V;
		}
		tone(level_hz(v / (pair ? 2 : 1)), ms / px);
	}
}

static void encode(const emode_t *m, bool with_vis, double lead_ms)
{
	ns = 0;
	t_send = 0;
	ph = 0;
	tone(1900, 0);
	if (lead_ms > 0)
		tone(1500, lead_ms);	// silence-like carrier before
	if (with_vis)
		vis_code(m->vis);
	if (m->enc == E_SCOTTIE)
		tone(1200, 9);		// Scottie's starting sync
	int lines = m->enc == E_PD ? m->h / 2 : m->h;
	for (int l = 0; l < lines; l++) {
		switch (m->enc) {
		case E_ROBOT36:
			tone(1200, 9);
			tone(1500, 3);
			scan(m, l, 3, 88, m->w, false);
			tone(l & 1 ? 2300 : 1500, 4.5);
			tone(1900, 1.5);
			scan(m, l, l & 1 ? 4 : 5, 44, m->w / 2, false);
			break;
		case E_ROBOT72:
			tone(1200, 9);
			tone(1500, 3);
			scan(m, l, 3, 138, m->w, false);
			tone(1500, 4.5);
			tone(1900, 1.5);
			scan(m, l, 5, 69, m->w / 2, false);
			tone(2300, 4.5);
			tone(1900, 1.5);
			scan(m, l, 4, 69, m->w / 2, false);
			break;
		case E_PD:
			tone(1200, 20);
			tone(1500, 2.08);
			scan(m, 2 * l, 3, m->scan, m->w, false);
			scan(m, 2 * l, 5, m->scan, m->w, true);
			scan(m, 2 * l, 4, m->scan, m->w, true);
			scan(m, 2 * l + 1, 3, m->scan, m->w, false);
			break;
		case E_MARTIN:
			tone(1200, 4.862);
			tone(1500, 0.572);
			scan(m, l, 1, m->scan, m->w, false);
			tone(1500, 0.572);
			scan(m, l, 2, m->scan, m->w, false);
			tone(1500, 0.572);
			scan(m, l, 0, m->scan, m->w, false);
			tone(1500, 0.572);
			break;
		case E_SCOTTIE:
			tone(1500, 1.5);
			scan(m, l, 1, m->scan, m->w, false);
			tone(1500, 1.5);
			scan(m, l, 2, m->scan, m->w, false);
			tone(1200, 9);
			tone(1500, 1.5);
			scan(m, l, 0, m->scan, m->w, false);
			break;
		}
	}
	tone(1500, 300);	// the transmitter's tail, then the carrier drops (quiet below)
}

// --- channel effects ---
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
	return (s - 3) * sqrt(2.0);
}

static int16_t sat16(double v)
{
	return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : lrint(v));
}

// white noise at snr dB against the tone (power over the whole 4 kHz band)
static void add_noise(double snr_db)
{
	double sigma = amp / sqrt(2.0) / pow(10, snr_db / 20);
	for (int i = 0; i < ns; i++)
		audio[i] = sat16(audio[i] + sigma * grand());
}

// an FM receiver's audio: 750 us de-emphasis and a 300 Hz high pass
static void deemphasis(void)
{
	double a = exp(-1.0 / (FS * 750e-6)), y = 0, hp_x = 0, hp_y = 0, b = exp(-2 * M_PI * 300 / FS);
	for (int i = 0; i < ns; i++) {
		y = a * y + (1 - a) * audio[i];
		double h = b * (hp_y + y * 3 - hp_x);
		hp_x = y * 3;
		hp_y = h;
		audio[i] = sat16(h);
	}
}

// --- the decoder's output ---
static uint8_t got[496][SSTV_W][3];
static bool have[496];
static int rows_got;
static void on_row(void *ctx, const sstv_t *d, uint16_t line, uint8_t sub)
{
	(void)ctx;
	if (line >= 496)
		return;
	for (uint16_t x = 0; x < SSTV_W; x++)
		sstv_rgb(d, sub, x, got[line][x]);
	have[line] = true;
	rows_got++;
}

static sstv_t dec;

static void decode(void)
{
	memset(have, 0, sizeof have);
	rows_got = 0;
	sstv_init(&dec, on_row, NULL);
	for (int i = 0; i < ns; i += 200)
		sstv_audio(&dec, &audio[i], (size_t)(ns - i < 200 ? ns - i : 200));
	int16_t quiet[200] = { 0 };
	for (int i = 0; i < 40; i++)		// a second of nothing after
		sstv_audio(&dec, quiet, 200);
}

// mean absolute error of the decoded lines against the pattern (decoded line l as picture line l + shift), over the
// lines both have; the lines next to the bars' lower edge and Robot's half-resolution chroma there are left out
static double mae(const emode_t *m, int shift, int *lines)
{
	double sum = 0;
	long n = 0;
	*lines = 0;
	for (int l = 0; l < m->h; l++) {
		int y = l + shift;
		if (!have[l] || y < 0 || y >= m->h)
			continue;
		(*lines)++;
		if (abs(y - m->h / 2) <= 2)
			continue;
		for (int x = 0; x < (int)SSTV_W; x++) {
			int x0 = x * m->w / SSTV_W, x1 = (x + 1) * m->w / SSTV_W;
			double want[3] = { 0, 0, 0 };
			for (int sx = x0; sx < x1; sx++) {
				double rgb[3];
				pattern(sx, y, m->w, m->h, rgb);
				for (int c = 0; c < 3; c++)
					want[c] += rgb[c] / (x1 - x0);
			}
			for (int c = 0; c < 3; c++) {
				sum += fabs(got[l][x][c] - want[c]);
				n++;
			}
		}
	}
	return n ? sum / n : 999;
}

static void run_case(const char *what, const emode_t *m, double max_mae, int min_pct)
{
	decode();
	int lines;
	double e = mae(m, 0, &lines);
	printf("  %-11s %-26s mode %-10s lines %3d/%3d  mae %5.1f  clock %+5d ppm\n", m->name, what,
	       sstv_mode_name(dec.mode), lines, m->h, e, (int)sstv_clock_ppm(&dec));
	CHECK(dec.mode == m->mode, "%s %s: mode %d, want %d", m->name, what, dec.mode, m->mode);
	CHECK(lines * 100 >= m->h * min_pct, "%s %s: %d of %d lines", m->name, what, lines, m->h);
	CHECK(e <= max_mae, "%s %s: mean abs error %.1f > %.1f", m->name, what, e, max_mae);
}

static const emode_t *by_mode(int mode)
{
	for (int i = 0; i < NMODES; i++)
		if (emodes[i].mode == mode)
			return &emodes[i];
	return NULL;
}

static void reset_channel(void)
{
	stretch = 1.0;
	f_off = 0;
	amp = 12000;
	rng = 12345;
}

static void no_start(const char *what)
{
	decode();
	printf("  %-38s state %d rows %d vis_unknown %d\n", what, dec.state, rows_got, dec.vis_unknown);
	CHECK(dec.state == SSTV_IDLE && rows_got == 0, "%s started a picture (mode %d)", what, dec.mode);
}

int main(void)
{
	printf("SSTV: every mode, clean, with its VIS\n");
	for (int i = 0; i < NMODES; i++) {
		reset_channel();
		encode(&emodes[i], true, 200);
		run_case("clean", &emodes[i], 12, 99);
		CHECK(dec.state == SSTV_DONE, "%s: state %d after the picture", emodes[i].name, dec.state);
	}
	static const int main_modes[] = { SSTV_ROBOT36, SSTV_PD120, SSTV_MARTIN1, SSTV_SCOTTIE1 };
	for (unsigned k = 0; k < sizeof main_modes / sizeof main_modes[0]; k++) {
		const emode_t *m = by_mode(main_modes[k]);
		printf("SSTV: %s on a channel\n", m->name);
		reset_channel();
		encode(m, true, 200);
		add_noise(12);
		run_case("noise, 12 dB SNR", m, 25, 97);
		reset_channel();
		for (int s = -1; s <= 1; s += 2) {
			stretch = 1.0 + s * 0.003;
			encode(m, true, 200);
			run_case(s > 0 ? "clock +0.3 %" : "clock -0.3 %", m, 14, 98);
			int ppm = (int)sstv_clock_ppm(&dec);
			CHECK(abs(ppm - s * 3000) < 300, "%s: clock %d ppm, want %d", m->name, ppm, s * 3000);
		}
		reset_channel();
		f_off = 8;
		encode(m, true, 200);
		run_case("frequency +8 Hz", m, 14, 99);
		f_off = -8;
		encode(m, true, 200);
		run_case("frequency -8 Hz", m, 14, 99);
		reset_channel();
		encode(m, true, 200);
		deemphasis();
		run_case("de-emphasized, 300 Hz HP", m, 14, 99);
		reset_channel();
		stretch = 1.002;
		encode(m, true, 200);
		add_noise(15);		// the discriminator's noise, de-emphasized with the signal
		deemphasis();
		run_case("all: de-emph, noise, clock", m, 25, 97);
		// no VIS: the picture starts on its line syncs a few lines in
		reset_channel();
		encode(m, false, 200);
		decode();
		int best_shift = 0, lines = 0;
		double best = 999;
		for (int shift = 0; shift <= 12; shift++) {
			int ln;
			double e = mae(m, shift, &ln);
			if (e < best) {
				best = e;
				best_shift = shift;
				lines = ln;
			}
		}
		printf("  %-11s %-26s mode %-10s lines %3d/%3d  mae %5.1f  (from line %d)\n", m->name, "no VIS",
		       sstv_mode_name(dec.mode), lines, m->h, best, best_shift);
		CHECK(dec.mode == m->mode, "%s no VIS: mode %d", m->name, dec.mode);
		CHECK(lines >= m->h - 16, "%s no VIS: %d lines", m->name, lines);
		CHECK(best <= 12, "%s no VIS: mean abs error %.1f", m->name, best);
	}

	printf("SSTV: no picture from noise or voice\n");
	reset_channel();
	ns = 0;
	for (int i = 0; i < 30 * 8000; i++)
		audio[ns++] = sat16(6000 * grand());
	no_start("30 s white noise");
	// band-limited noise around the SSTV tones
	ns = 0;
	double lp = 0, lp2 = 0;
	for (int i = 0; i < 30 * 8000; i++) {
		lp += 0.5 * (grand() * 8000 - lp);
		lp2 += 0.5 * (lp - lp2);
		audio[ns++] = sat16(lp2 * 2);
	}
	no_start("30 s low-passed noise");
	// voice-like: a gliding pitch with harmonics through two moving formants, syllables 4 a second
	ns = 0;
	double pph = 0;
	for (int i = 0; i < 30 * 8000; i++) {
		double t = i / FS;
		double pitch = 120 + 40 * sin(2 * M_PI * 0.7 * t) + 20 * sin(2 * M_PI * 3.1 * t);
		pph += 2 * M_PI * pitch / FS;
		double f1 = 600 + 300 * sin(2 * M_PI * 2.3 * t), f2 = 1700 + 500 * sin(2 * M_PI * 1.7 * t);
		double v = 0;
		for (int h = 1; h * pitch < 3500; h++) {
			double hf = h * pitch;
			double g = exp(-pow((hf - f1) / 200, 2)) + 0.6 * exp(-pow((hf - f2) / 300, 2)) + 0.05;
			v += g * sin(h * pph);
		}
		double env = 0.5 + 0.5 * sin(2 * M_PI * 4 * t);
		audio[ns++] = sat16(4000 * v * env * env + 300 * grand());
	}
	no_start("30 s voice-like audio");
	// a whistle at 1900 Hz with a 1200 Hz dip that is no VIS (bad parity)
	ns = 0;
	t_send = 0;
	ph = 0;
	tone(1900, 300);
	tone(1200, 10);
	tone(1900, 300);
	tone(1200, 30);
	for (int b = 0; b < 8; b++)
		tone(b == 0 ? 1100 : 1300, 30);	// one 1, parity says even: wrong
	tone(1200, 30);
	tone(1500, 2000);
	no_start("a VIS with bad parity");

	printf("SSTV: display dither\n");
	for (uint8_t i = 0; i < SSTV_COLOURS; i++) {
		uint8_t rgb[3];
		sstv_colour(i, rgb);
		bool same = true;
		for (uint16_t y = 0; y < 4; y++)
			for (uint16_t x = 0; x < 4; x++)
				same = same && sstv_dither(rgb, x, y) == i;
		CHECK(same, "colour %u does not dither to itself", i);
	}
	for (int v = 0; v <= 255; v += 15) {
		double mean[3] = { 0, 0, 0 };
		uint8_t in[3] = { (uint8_t)v, (uint8_t)v, (uint8_t)v };
		for (uint16_t y = 0; y < 4; y++)
			for (uint16_t x = 0; x < 4; x++) {
				uint8_t rgb[3];
				sstv_colour(sstv_dither(in, x, y), rgb);
				for (int c = 0; c < 3; c++)
					mean[c] += rgb[c] / 16.0;
			}
		for (int c = 0; c < 3; c++)
			CHECK(fabs(mean[c] - v) <= 17, "grey %d dithers to %.0f in channel %d", v, mean[c], c);
	}

	// the glue: rows scaled to the display, dithered, through the ring, as the screen takes them
	printf("SSTV: radio task glue (display rows)\n");
	for (int k = 0; k < 2; k++) {
		const emode_t *m = by_mode(k == 0 ? SSTV_ROBOT36 : SSTV_PD120);
		reset_channel();
		encode(m, true, 200);
		CHECK(radio_sstv_start(), "glue start");
		static uint8_t shown[SSTV_ROWS_MAX][RADIO_SSTV_ROW_BYTES];
		radio_sstv_info_t in;
		int rows = 0, last = -1;
		bool order = true;
		for (int i = 0; i < ns; i += 32) {
			radio_sstv_audio(&audio[i], (uint16_t)(ns - i < 32 ? ns - i : 32));
			radio_sstv_info(&in);
			uint8_t y, px[RADIO_SSTV_ROW_BYTES];
			while (radio_sstv_row(in.image, &y, px)) {
				order = order && y > last && y < SSTV_ROWS_MAX;
				last = y;
				memcpy(shown[y < SSTV_ROWS_MAX ? y : 0], px, sizeof px);
				rows++;
			}
		}
		radio_sstv_info(&in);
		int want_rows = sstv_mode_rows((uint8_t)m->mode);
		// each 4 x 4 block of display pixels (the dither's cell) against the pattern there
		double err = 0;
		int nb = 0;
		for (int by = 0; by + 4 <= want_rows; by += 4) {
			int l0 = (by * m->h + want_rows - 1) / want_rows;
			if (abs(l0 - m->h / 2) <= 8)
				continue;
			for (int bx = 0; bx < (int)SSTV_W; bx += 4) {
				double got_c[3] = { 0, 0, 0 }, want_c[3] = { 0, 0, 0 };
				for (int r = by; r < by + 4; r++) {
					int l = (r * m->h + want_rows - 1) / want_rows;
					for (int x = bx; x < bx + 4; x++) {
						uint8_t b = shown[r][x / 2], idx = (uint8_t)(x & 1 ? b & 15u : b >> 4), c[3];
						double p[3];
						sstv_colour(idx, c);
						pattern(x * m->w / (int)SSTV_W + m->w / (2 * (int)SSTV_W), l, m->w, m->h, p);
						for (int ch = 0; ch < 3; ch++) {
							got_c[ch] += c[ch] / 16.0;
							want_c[ch] += p[ch] / 16.0;
						}
					}
				}
				for (int ch = 0; ch < 3; ch++)
					err += fabs(got_c[ch] - want_c[ch]);
				nb += 3;
			}
		}
		err /= nb ? nb : 1;
		printf("  %-11s rows %d/%d in order %d  state %d line %u/%u  block error %.1f\n", m->name, rows,
		       want_rows, order, in.state, in.line, in.lines, err);
		CHECK(rows == want_rows && order, "%s glue: %d rows of %d (in order %d)", m->name, rows, want_rows, order);
		CHECK(in.mode == m->mode && in.state == SSTV_DONE && in.line == in.lines && in.lines == m->h,
		      "%s glue: mode %d state %d line %u/%u", m->name, in.mode, in.state, in.line, in.lines);
		CHECK(err < 20, "%s glue: dithered blocks %.1f off the pattern", m->name, err);
		radio_sstv_stop();
		uint8_t y, px[RADIO_SSTV_ROW_BYTES];
		CHECK(!radio_sstv_row(in.image, &y, px), "a row after stop");
	}

	if (fails) {
		printf("test_sstv: %d FAILED\n", fails);
		return 1;
	}
	printf("test_sstv: all passed\n");
	return 0;
}
