/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus CW decoder (integration/radiofoundry/cw/cwdec.c) against synthetic MCW: our own keyer and audio here
 * (its own Morse table, not the decoder's), random text at 5, 12, 20 and 30 WPM (and 8, 35), with element jitter,
 * keyer weight, white noise, slow fading, tone drift and tones across 400-1000 Hz; the character error rate
 * (Levenshtein distance / characters sent) must stay under 2 % at a reasonable SNR. Then the prosigns, the speed and
 * tone read back, no text from noise or a held carrier tone, the gate's speed range, and the radio task glue
 * (radio_cw.c: the text ring, info, start / stop) with the audio tests' RTOS stubs.
 *
 * SNR here: the tone's power over the white noise's power in the whole 0-4 kHz audio band (an FM / AM receiver's
 * audio). The table printed at the end goes into docs/plus.md. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "radio/cwdec.h"
#include "radio/radio_cw.h"

void audio_test_enter(void) {}
void audio_test_exit(void) {}

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* --- our own Morse table (ITU-R M.1677-1 and the prosigns, by pattern) --- */
static const struct {
	const char *text, *pat;
} morse[] = {
	{ "A", ".-" }, { "B", "-..." }, { "C", "-.-." }, { "D", "-.." }, { "E", "." }, { "F", "..-." }, { "G", "--." },
	{ "H", "...." }, { "I", ".." }, { "J", ".---" }, { "K", "-.-" }, { "L", ".-.." }, { "M", "--" }, { "N", "-." },
	{ "O", "---" }, { "P", ".--." }, { "Q", "--.-" }, { "R", ".-." }, { "S", "..." }, { "T", "-" }, { "U", "..-" },
	{ "V", "...-" }, { "W", ".--" }, { "X", "-..-" }, { "Y", "-.--" }, { "Z", "--.." },
	{ "0", "-----" }, { "1", ".----" }, { "2", "..---" }, { "3", "...--" }, { "4", "....-" }, { "5", "....." },
	{ "6", "-...." }, { "7", "--..." }, { "8", "---.." }, { "9", "----." },
	{ ".", ".-.-.-" }, { ",", "--..--" }, { "?", "..--.." }, { "/", "-..-." }, { "=", "-...-" }, { "-", "-....-" },
	{ ":", "---..." }, { "'", ".----." }, { ")", "-.--.-" }, { "\"", ".-..-." }, { "@", ".--.-." },
	{ "<AR>", ".-.-." }, { "<KN>", "-.--." }, { "<SK>", "...-.-" }, { "<AS>", ".-..." }, { "<KA>", "-.-.-" },
	{ "<VE>", "...-." }, { "<BK>", "-...-.-" }, { "<SOS>", "...---..." }, { "<CL>", "-.-..-.." },
	{ "<HH>", "........" },
};
#define MORSE_N (sizeof morse / sizeof morse[0])

/* --- the synthesizer --- */
typedef struct {
	double wpm;
	double jitter;		/* each element and gap times 1 +- this (uniform) */
	double weight;		/* dot units added to each mark, taken off each gap */
	double snr_db;		/* 99: no noise */
	double fade;		/* amplitude dips to 1 - fade (slow sine) */
	double fade_s;
	double tone, drift;	/* Hz, Hz per second */
	double farns;		/* extra units after each character (Farnsworth-like spacing) */
	unsigned seed;
} syn_t;

static uint64_t rng_s;
static double urand(void)
{
	rng_s = rng_s * 6364136223846793005ull + 1442695040888963407ull;
	return (double)(rng_s >> 11) / 9007199254740992.0;
}
static double grand(void)
{
	double u = urand(), v = urand();
	return sqrt(-2.0 * log(u + 1e-300)) * cos(2.0 * M_PI * v);
}

static int16_t *pcm;
static size_t pcm_n, pcm_cap;
static double ph, t_s, tail_ms = 3000;

static void put(double env, const syn_t *s)
{
	if (pcm_n == pcm_cap) {
		pcm_cap = pcm_cap ? pcm_cap * 2 : 1 << 20;
		pcm = realloc(pcm, pcm_cap * sizeof *pcm);
	}
	double amp = 8000.0 * (1.0 - s->fade * (0.5 - 0.5 * cos(2.0 * M_PI * t_s / s->fade_s)));
	double f = s->tone + s->drift * t_s;
	ph += 2.0 * M_PI * f / 8000.0;
	double v = amp * env * sin(ph);
	if (s->snr_db < 90)
		v += grand() * 8000.0 / sqrt(2.0) * pow(10.0, -s->snr_db / 20.0);
	v = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
	pcm[pcm_n++] = (int16_t)lrint(v);
	t_s += 1.0 / 8000.0;
}

/* on for ms with 4 ms raised cosine edges, or silent */
static void seg(bool on, double ms, const syn_t *s)
{
	int n = (int)lrint(ms * 8.0), r = 32;
	for (int i = 0; i < n; i++) {
		double e = 0;
		if (on) {
			e = 1.0;
			if (i < r)
				e = 0.5 - 0.5 * cos(M_PI * i / r);
			else if (n - 1 - i < r)
				e = 0.5 - 0.5 * cos(M_PI * (n - 1 - i) / r);
		}
		put(e, s);
	}
}

static const char *pattern(const char **p)
{
	for (unsigned i = 0; i < MORSE_N; i++) {
		size_t n = strlen(morse[i].text);
		if (strncmp(*p, morse[i].text, n) == 0) {
			*p += n;
			return morse[i].pat;
		}
	}
	(*p)++;
	return NULL;
}

static double jit(const syn_t *s)
{
	return 1.0 + s->jitter * (2.0 * urand() - 1.0);
}

/* The text keyed (tokens as the decoder prints them), with noise before and after */
static void key_text(const char *text, const syn_t *s, double lead_ms)
{
	double dot = 1200.0 / s->wpm, w = s->weight * dot;
	seg(false, lead_ms, s);
	const char *p = text;
	while (*p) {
		if (*p == ' ') {
			seg(false, 4.0 * dot * jit(s), s);	/* 3 after the character (less the weight) + 4 = 7 */
			p++;
			continue;
		}
		const char *pat = pattern(&p);
		if (pat == NULL)
			continue;
		for (const char *e = pat; *e; e++) {
			seg(true, (*e == '-' ? 3.0 : 1.0) * dot * jit(s) + w, s);
			double gap = (e[1] ? 1.0 : 3.0 + s->farns) * dot * jit(s) - w;
			seg(false, gap < 0.2 * dot ? 0.2 * dot : gap, s);
		}
	}
	seg(false, tail_ms, s);
}

/* --- the decoder's output --- */
static char got[8192];
static size_t got_n;
static void on_text(void *ctx, const char *s)
{
	(void)ctx;
	while (*s && got_n + 1 < sizeof got)
		got[got_n++] = *s++;
	got[got_n] = 0;
}

static cwdec_t dec;
static uint16_t max_tone, min_tone;

static void decode(void)
{
	got_n = 0;
	got[0] = 0;
	cwdec_init(&dec, on_text, NULL);
	max_tone = 0;
	min_tone = 0xFFFF;
	/* in uneven pieces, as the ring hands them over */
	for (size_t i = 0; i < pcm_n;) {
		size_t n = 1 + (i * 7) % 61;
		if (n > pcm_n - i)
			n = pcm_n - i;
		cwdec_audio(&dec, pcm + i, (uint16_t)n);
		uint16_t t = cwdec_tone(&dec);
		if (t) {
			max_tone = t > max_tone ? t : max_tone;
			min_tone = t < min_tone ? t : min_tone;
		}
		i += n;
	}
	cwdec_flush(&dec);
}

/* Spaces collapsed, ends trimmed */
static void norm(const char *in, char *out, size_t cap)
{
	size_t n = 0;
	bool sp = true;
	for (; *in && n + 1 < cap; in++) {
		if (*in == ' ') {
			if (!sp)
				out[n++] = ' ';
			sp = true;
		} else {
			out[n++] = *in;
			sp = false;
		}
	}
	while (n > 0 && out[n - 1] == ' ')
		n--;
	out[n] = 0;
}

static int lev(const char *a, const char *b)
{
	size_t n = strlen(a), m = strlen(b);
	int *prev = malloc((m + 1) * sizeof *prev), *cur = malloc((m + 1) * sizeof *cur);
	for (size_t j = 0; j <= m; j++)
		prev[j] = (int)j;
	for (size_t i = 1; i <= n; i++) {
		cur[0] = (int)i;
		for (size_t j = 1; j <= m; j++) {
			int c = prev[j - 1] + (a[i - 1] != b[j - 1]);
			int d = prev[j] + 1 < cur[j - 1] + 1 ? prev[j] + 1 : cur[j - 1] + 1;
			cur[j] = c < d ? c : d;
		}
		int *t = prev;
		prev = cur;
		cur = t;
	}
	int r = prev[m];
	free(prev);
	free(cur);
	return r;
}

static const char charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789./?,=";

static void random_text(char *out, size_t chars, unsigned seed)
{
	rng_s = seed * 2654435761u + 1;
	size_t n = 0;
	while (n < chars) {
		int len = 2 + (int)(urand() * 6);
		for (int i = 0; i < len && n < chars; i++, n++)
			*out++ = charset[(int)(urand() * (sizeof charset - 1))];
		*out++ = ' ';
	}
	out[-1] = 0;
}

/* Key, decode, the error rate (in %, of the characters sent; a space counts as one) */
static double run_case(const char *text, const syn_t *s)
{
	pcm_n = 0;
	ph = t_s = 0;
	rng_s = s->seed * 0x9E3779B97F4A7C15ull + 7;
	key_text(text, s, 1500);
	decode();
	char a[8192], b[8192];
	norm(got, a, sizeof a);
	norm(text, b, sizeof b);
	return 100.0 * lev(a, b) / (double)strlen(b);
}

static double worst;

static void cer_case(const char *name, syn_t s, size_t chars, double limit)
{
	char text[2048];
	double sum = 0;
	int runs = 3;
	for (int r = 0; r < runs; r++) {
		random_text(text, chars, s.seed + 100u * (unsigned)r);
		s.seed += 17;
		double c = run_case(text, &s);
		sum += c;
		if (c >= limit) {
			char a[8192];
			norm(got, a, sizeof a);
			printf("FAIL %s run %d: CER %.2f %% (limit %.1f)\n  sent: %.160s\n  got:  %.160s\n", name, r, c, limit,
			       text, a);
			fails++;
		}
	}
	double mean = sum / runs;
	if (limit <= 2.0)
		worst = mean > worst ? mean : worst;
	printf("  %-46s CER %5.2f %%  %2u WPM read, tone %u-%u Hz\n", name, mean, cwdec_wpm(&dec), min_tone, max_tone);
}

static syn_t base(double wpm)
{
	syn_t s = { .wpm = wpm, .jitter = 0, .weight = 0, .snr_db = 99, .fade = 0, .fade_s = 3, .tone = 700, .drift = 0,
		    .seed = 1 };
	return s;
}

static void test_cer(void)
{
	static const double speeds[] = { 5, 12, 20, 30 };
	printf("Synthetic MCW, CER over 3 random texts each (limit 2 %%):\n");
	for (unsigned i = 0; i < 4; i++) {
		double wpm = speeds[i];
		size_t chars = wpm <= 5 ? 90 : wpm <= 12 ? 160 : 240;
		char name[64];
		syn_t s = base(wpm);
		snprintf(name, sizeof name, "%2.0f WPM clean", wpm);
		cer_case(name, s, chars, 2.0);
		CHECK(abs((int)cwdec_wpm(&dec) - (int)wpm) <= (int)(wpm / 8) + 1);
		CHECK(abs((int)cwdec_tone(&dec) - 700) <= 6 || cwdec_tone(&dec) == 0);
		CHECK(abs((int)max_tone - 700) <= 8 && abs((int)min_tone - 700) <= 8);
		s.jitter = 0.15;
		s.snr_db = 6;
		snprintf(name, sizeof name, "%2.0f WPM jitter 15 %%, 6 dB SNR", wpm);
		cer_case(name, s, chars, 2.0);
		s = base(wpm);
		s.jitter = 0.1;
		s.weight = 0.3;
		s.snr_db = 10;
		s.tone = 950;
		snprintf(name, sizeof name, "%2.0f WPM heavy weight +0.3, 950 Hz, 10 dB", wpm);
		cer_case(name, s, chars, 2.0);
		s.weight = -0.25;
		s.tone = 420;
		snprintf(name, sizeof name, "%2.0f WPM light weight -0.25, 420 Hz, 10 dB", wpm);
		cer_case(name, s, chars, 2.0);
		s = base(wpm);
		s.jitter = 0.1;
		s.snr_db = 18;
		s.fade = 0.8;	/* -14 dB at the bottom: +4 dB SNR there */
		s.fade_s = 4;
		snprintf(name, sizeof name, "%2.0f WPM fading -14 dB / 4 s, 18 dB (+4 low)", wpm);
		cer_case(name, s, chars, 2.0);
		s.snr_db = 12;
		s.fade = 0.5;	/* -6 dB */
		s.fade_s = 1.5;	/* fast flutter */
		snprintf(name, sizeof name, "%2.0f WPM flutter -6 dB / 1.5 s, 12 dB (+6 low)", wpm);
		cer_case(name, s, chars, 2.0);
		s = base(wpm);
		s.jitter = 0.1;
		s.snr_db = 10;
		s.tone = 500;
		s.drift = wpm <= 5 ? 0.6 : 2.5;	/* 500 Hz drifting up ~100-300 Hz over the text */
		snprintf(name, sizeof name, "%2.0f WPM tone drift +%.1f Hz/s from 500 Hz", wpm, s.drift);
		cer_case(name, s, chars, 2.0);
		CHECK(max_tone > 560);
		s = base(wpm);
		s.jitter = 0.15;
		s.weight = 0.15;
		s.snr_db = 12;
		s.fade = 0.5;
		s.fade_s = 5;
		s.tone = 650;
		s.drift = 1;
		snprintf(name, sizeof name, "%2.0f WPM all: jitter, weight, -6 dB fade, drift", wpm);
		cer_case(name, s, chars, 2.0);
	}
	/* the ends of the range */
	syn_t s = base(8);
	s.snr_db = 10;
	s.jitter = 0.1;
	cer_case(" 8 WPM jitter 10 %, 10 dB", s, 120, 2.0);
	s = base(35);
	s.snr_db = 10;
	s.jitter = 0.08;
	cer_case("35 WPM jitter 8 %, 10 dB", s, 300, 2.0);
	s = base(20);
	s.farns = 3;
	s.snr_db = 10;
	cer_case("20 WPM chars, Farnsworth gaps +3 units, 10 dB", s, 200, 2.0);
	/* tone across the band */
	for (double f = 400; f <= 1000; f += 150) {
		char name[64];
		s = base(18);
		s.tone = f;
		s.snr_db = 10;
		s.jitter = 0.05;
		snprintf(name, sizeof name, "18 WPM tone %.0f Hz, 10 dB", f);
		cer_case(name, s, 150, 2.0);
		CHECK(abs((int)max_tone - (int)f) <= 8 && abs((int)min_tone - (int)f) <= 8);
	}
	/* where it gives up: for the docs */
	printf("Noise: 20 WPM, jitter 10 %%, CER by SNR (not checked below 3 dB):\n");
	for (int snr = 10; snr >= -6; snr -= 3) {
		char name[64];
		s = base(20);
		s.jitter = 0.1;
		s.snr_db = snr;
		snprintf(name, sizeof name, "20 WPM SNR %+d dB", snr);
		cer_case(name, s, 200, snr >= 3 ? 2.0 : 1000.0);
	}
}

static void test_prosigns(void)
{
	const char *text = "CQ CQ DE TA1ABC TA1ABC <KN> RST 599 = TNX <AR> 73 <SK> <BK> <AS> <KA> <VE> <CL> <SOS> "
			   "QTH IST? 5W/P, <HH> TEST";
	syn_t s = base(18);
	s.snr_db = 12;
	s.jitter = 0.05;
	double c = run_case(text, &s);
	char a[8192];
	norm(got, a, sizeof a);
	CHECK(c == 0.0);
	if (c != 0.0)
		printf("  prosigns: got %s\n", a);
	CHECK(strstr(a, "<KN>") && strstr(a, "<AR>") && strstr(a, "<SK>") && strstr(a, "<SOS>") && strstr(a, "<HH>"));
	/* the table: each code once, every one of ours found */
	for (unsigned i = 0; i < MORSE_N; i++) {
		uint16_t code = 1;
		for (const char *e = morse[i].pat; *e; e++)
			code = (uint16_t)(code << 1 | (*e == '-'));
		const char *t = cwdec_lookup(code);
		CHECK(t != NULL && strcmp(t, morse[i].text) == 0);
	}
	CHECK(cwdec_lookup(0x24) == NULL && cwdec_lookup(0) == NULL && cwdec_lookup(1) == NULL);
	printf("Prosigns: <AR> <KN> <SK> <AS> <KA> <VE> <BK> <CL> <SOS> <HH> and = in a QSO, 18 WPM: \"%.60s...\"\n", a);
}

/* Another station at another speed: after a pause the gate keeps open through (1 s), and after one it closes on (5 s:
 * learned afresh); and a short CW ID alone (learned and shown when the tone goes) */
static void test_speed_change(void)
{
	static const struct {
		double a, b, pause;
	} cases[] = { { 15, 30, 1000 }, { 30, 12, 1000 }, { 25, 8, 5000 }, { 8, 25, 5000 } };
	char ta[512], tb[512], both[1100], a[8192], want[8192];
	for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		random_text(ta, 80, 11u + i);
		random_text(tb, 80, 23u + i);
		syn_t s = base(cases[i].a);
		s.snr_db = 12;
		s.jitter = 0.08;
		pcm_n = 0;
		ph = t_s = 0;
		rng_s = 77 + i;
		tail_ms = cases[i].pause;
		key_text(ta, &s, 1000);
		s.wpm = cases[i].b;
		tail_ms = 3000;
		key_text(tb, &s, 0);
		decode();
		snprintf(both, sizeof both, "%s %s", ta, tb);
		norm(got, a, sizeof a);
		norm(both, want, sizeof want);
		double c = 100.0 * lev(a, want) / (double)strlen(want);
		printf("Speed change %2.0f -> %2.0f WPM, pause %4.0f ms: CER %.2f %%, %u WPM read at the end\n", cases[i].a,
		       cases[i].b, cases[i].pause, c, cwdec_wpm(&dec));
		CHECK(c < 2.0);
		CHECK(abs((int)cwdec_wpm(&dec) - (int)cases[i].b) <= (int)cases[i].b / 8 + 1);
	}
	/* a repeater's CW ID: 25 WPM, then the carrier drops */
	syn_t s = base(25);
	s.snr_db = 15;
	double c = run_case("DE TA1ABC/R", &s);
	norm(got, a, sizeof a);
	printf("CW ID \"DE TA1ABC/R\" at 25 WPM alone: \"%s\"\n", a);
	CHECK(c == 0.0);
}

/* Noise alone (squelch open, no carrier), a held tone, voice-like audio: no text */
static void test_quiet(void)
{
	syn_t s = base(20);
	s.snr_db = -40;	/* noise 40 dB over the (absent) tone: just noise */
	pcm_n = 0;
	ph = t_s = 0;
	rng_s = 99;
	seg(false, 60000, &s);
	for (size_t i = 0; i < pcm_n; i++)
		pcm[i] = (int16_t)lrint(grand() * 6000.0);
	decode();
	printf("Noise only, 60 s: %zu characters out\n", got_n);
	CHECK(got_n <= 2);
	/* a tone held for 10 s, then off */
	pcm_n = 0;
	ph = t_s = 0;
	s.snr_db = 20;
	seg(false, 1000, &s);
	seg(true, 10000, &s);
	seg(false, 3000, &s);
	decode();
	CHECK(got_n == 0);
	/* "voice": a buzz at a moving pitch (120-220 Hz, its harmonics through the band), syllables on and off */
	pcm_n = 0;
	double vph = 0;
	for (int i = 0; i < 8000 * 30; i++) {
		double t = i / 8000.0;
		double f0 = 170 + 50 * sin(2 * M_PI * 0.7 * t) + 20 * sin(2 * M_PI * 3.1 * t);
		vph += 2 * M_PI * f0 / 8000.0;
		double env = sin(2 * M_PI * 2.3 * t) > -0.2 ? 1.0 : 0.0;
		double v = 0;
		for (int h = 1; h <= 12; h++)
			v += sin(h * vph) / h;
		pcm_n = (size_t)i;
		put(0, &s);
		pcm[i] = (int16_t)lrint(v * env * 5000.0 + grand() * 200.0);
	}
	decode();
	printf("Voice-like buzz, 30 s: %zu characters out\n", got_n);
	CHECK(got_n <= 30);
}

/* --- radio_cw.c --- */

/* What is kept, from the first kept character on */
static uint16_t kept(char *out, uint16_t n)
{
	return radio_cw_copy(radio_cw_first(), out, n);
}

static void test_glue(void)
{
	radio_cw_info_t in;
	char text[RADIO_CW_TEXT + 1];
	radio_cw_clear();
	CHECK(kept(text, RADIO_CW_TEXT) == 0 && text[0] == 0);
	CHECK(radio_cw_start());
	radio_cw_info(&in);
	CHECK(in.on && in.wpm == 0 && in.tone_hz == 0);
	syn_t s = base(20);
	s.snr_db = 15;
	pcm_n = 0;
	ph = t_s = 0;
	rng_s = 5;
	key_text("CQ DE TA1ABC K", &s, 500);
	uint32_t q0 = radio_cw_seq(), w0 = radio_cw_written();
	bool saw_key = false, saw_tone = false;
	for (size_t i = 0; i < pcm_n; i += 32) {
		radio_cw_audio(pcm + i, (uint16_t)(pcm_n - i < 32 ? pcm_n - i : 32));
		radio_cw_info(&in);
		saw_key |= in.key;
		saw_tone |= in.tone_hz >= 690 && in.tone_hz <= 710;
	}
	CHECK(radio_cw_seq() != q0 && saw_key && saw_tone);
	radio_cw_info(&in);
	CHECK(in.wpm >= 18 && in.wpm <= 22);
	uint16_t n = kept(text, RADIO_CW_TEXT);
	CHECK(n == strlen(text) && n == radio_cw_written() - w0 && strstr(text, "CQ DE TA1ABC K") != NULL);
	/* the ring keeps the newest RADIO_CW_TEXT characters, numbered from power-on */
	pcm_n = 0;
	key_text("PARIS PARIS PARIS PARIS PARIS PARIS PARIS PARIS PARIS PARIS PARIS PARIS PARIS PARIS PARIS PARIS PARIS "
		 "PARIS PARIS PARIS PARIS PARIS PARIS PARIS PARIS PARIS PARIS PARIS", &s, 0);
	for (size_t i = 0; i < pcm_n; i += 1000)
		radio_cw_audio(pcm + i, (uint16_t)(pcm_n - i < 1000 ? pcm_n - i : 1000));
	n = kept(text, RADIO_CW_TEXT);
	CHECK(n == RADIO_CW_TEXT && strlen(text) == RADIO_CW_TEXT && strstr(text, "PARIS PARIS") != NULL);
	CHECK(radio_cw_written() - radio_cw_first() == RADIO_CW_TEXT && radio_cw_written() > RADIO_CW_TEXT);
	/* a piece by number; numbers no longer kept are skipped; past the end nothing */
	char piece[8];
	uint32_t last = radio_cw_written() - 3u;
	CHECK(radio_cw_copy(last, piece, 7) == 3 && memcmp(piece, text + RADIO_CW_TEXT - 3, 3) == 0);
	CHECK(radio_cw_copy(0, piece, 7) == 7 && memcmp(piece, text, 7) == 0);
	CHECK(radio_cw_copy(radio_cw_written(), piece, 7) == 0 && piece[0] == 0);
	radio_cw_stop();
	radio_cw_info(&in);
	CHECK(!in.on && in.key == false && in.wpm != 0);
	radio_cw_audio(pcm, 100);	/* stopped: ignored */
	CHECK(kept(text, RADIO_CW_TEXT) == RADIO_CW_TEXT);
	uint32_t q1 = radio_cw_seq();
	radio_cw_clear();
	CHECK(kept(text, RADIO_CW_TEXT) == 0 && radio_cw_seq() != q1 && radio_cw_first() == radio_cw_written());
	printf("Radio glue: start, text ring (newest %u, numbered), info, stop, clear; cwdec_t %zu bytes\n",
	       RADIO_CW_TEXT, sizeof(cwdec_t));
}


int main(int argc, char **argv)
{
	if (argc > 1) {		/* one case by hand: wpm jitter weight snr fade tone drift [text] */
		syn_t s = base(atof(argv[1]));
		s.jitter = argc > 2 ? atof(argv[2]) : 0;
		s.weight = argc > 3 ? atof(argv[3]) : 0;
		s.snr_db = argc > 4 ? atof(argv[4]) : 99;
		s.fade = argc > 5 ? atof(argv[5]) : 0;
		s.fade_s = 4;
		s.tone = argc > 6 ? atof(argv[6]) : 700;
		s.drift = argc > 7 ? atof(argv[7]) : 0;
		char text[2048];
		random_text(text, 120, 1);
		double c = run_case(argc > 8 ? argv[8] : text, &s);
		char a[8192];
		norm(got, a, sizeof a);
		printf("\nCER %.2f %%\nsent: %s\ngot:  %s\n", c, argc > 8 ? argv[8] : text, a);
		return 0;
	}
	test_prosigns();
	test_quiet();
	test_glue();
	test_speed_change();
	test_cer();
	free(pcm);
	printf("CW decoder: worst mean CER %.2f %% (checked cases), %d failures\n", worst, fails);
	return fails != 0;
}
