/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus CW practice (integration/radiofoundry/cw/cw_train.c) and its Morse in the beep generator (beep.c's
 * BEEP_MORSE, prepare_cw_rx): the Koch order and the random draw, the score (edit distance), the playing time, the
 * hand-over between the tasks, and the PCM the beep generator makes: every mark and gap measured in samples against
 * the units at the speed, decoded back by the CW decoder (cwdec.c), on its own stream and mixed into a voice stream;
 * the table pattern BEEP_MORSE keeps for the radio's beep list. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "radio/cw_train.h"
#include "radio/morse.h"
#include "radio/beep.h"
#include "radio/cwdec.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* --- a fake PCM stream --- */
static struct {
	bool started, pcm;
	audio_block_cb_t cb;
	void *ctx;
} aud;
static bool a_start(audio_block_cb_t cb, void *ctx) { aud.cb = cb; aud.ctx = ctx; aud.started = true; return true; }
static void a_stop(void) { aud.started = false; }
static void a_pcm(bool on) { aud.pcm = on; }
static const hal_audio_t fake_audio = { .start = a_start, .stop = a_stop, .speaker_pcm = a_pcm };

static void test_koch(void)
{
	CHECK(strlen(cw_koch) == CW_TRAIN_KOCH_N);
	for (unsigned i = 0; i < CW_TRAIN_KOCH_N; i++) {
		CHECK(morse_code(cw_koch[i]) != 0);
		CHECK(strchr(cw_koch + i + 1, cw_koch[i]) == NULL);	/* each once */
	}
	CHECK(strncmp(cw_koch, "KMURESNAPTLWI.JZ=FOY,VG5/Q92H38B?47C1D60X", CW_TRAIN_KOCH_N) == 0);
	/* the draw: only the first `level` characters, every one of them, the newest the most */
	for (uint8_t level = CW_TRAIN_LEVEL_MIN; level <= CW_TRAIN_KOCH_N; level++) {
		unsigned count[CW_TRAIN_KOCH_N] = { 0 }, n = 4000;
		uint32_t rng = level * 7919u;
		for (unsigned i = 0; i < n; i++) {
			char c = cw_train_char(&rng, level);
			const char *p = strchr(cw_koch, c);
			CHECK(p != NULL && (unsigned)(p - cw_koch) < level);
			if (p != NULL)
				count[p - cw_koch]++;
		}
		for (unsigned i = 0; i < level; i++)
			CHECK(count[i] > 0);
		if (level > CW_TRAIN_LEVEL_MIN)
			CHECK(count[level - 1u] > n / 3u);	/* a third and its share of the rest */
	}
	uint32_t rng = 1;
	CHECK(strchr("KM", cw_train_char(&rng, 0)) != NULL);	/* below the minimum: K and M */
	char c = cw_train_char(&rng, 200);
	CHECK(strchr(cw_koch, c) != NULL);
	rng = 0;	/* a zero state still draws */
	CHECK(strchr("KM", cw_train_char(&rng, 2)) != NULL && rng != 0);
	char g[CW_TRAIN_MAX + 2];
	memset(g, '#', sizeof g);
	cw_train_group(&rng, 5, 20, g);	/* at most CW_TRAIN_MAX */
	CHECK(g[CW_TRAIN_MAX] == '#');
	for (unsigned i = 0; i < CW_TRAIN_MAX; i++)
		CHECK(strchr("KMURE", g[i]) != NULL);
	printf("Koch order: 41 characters, the draw within the level, the newest a third and more\n");
}

static void test_score(void)
{
	CHECK(cw_train_right("KMURE", 5, "KMURE", 5) == 5);
	CHECK(cw_train_right("KMURE", 5, "KMUSE", 5) == 4);	/* one wrong */
	CHECK(cw_train_right("KMURE", 5, "KMRE", 4) == 4);	/* one missed: the rest still right */
	CHECK(cw_train_right("KMURE", 5, "KMUURE", 6) == 4);	/* one extra */
	CHECK(cw_train_right("KMURE", 5, "MURE", 4) == 4);	/* the first missed */
	CHECK(cw_train_right("KMURE", 5, "", 0) == 0);
	CHECK(cw_train_right("KMURE", 5, "SSSSSSSS", 8) == 0);	/* never below 0 */
	CHECK(cw_train_right("K", 1, "K", 1) == 1 && cw_train_right("K", 1, "M", 1) == 0);
	CHECK(cw_train_right("KMUREKMU", 8, "KMUREKMU", 8) == 8);
	CHECK(cw_train_right("KMUREKMUXX", 10, "KMUREKMUXX", 10) == 8);	/* clamped to CW_TRAIN_MAX */
	printf("Score: right = sent - edit distance (wrong, missed, extra), at least 0\n");
}

static void test_timing(void)
{
	/* PARIS is 50 units with its word space, 43 without (the last gap isn't played) */
	CHECK(cw_train_ms("PARIS", 5, 20) == 43u * 60u);
	CHECK(cw_train_ms("PARIS PARIS", 11, 20) == (43u + 7u + 43u) * 60u);
	CHECK(cw_train_ms("PARIS  PARIS", 12, 20) == (43u + 7u + 43u) * 60u);	/* spaces: one word gap */
	CHECK(cw_train_ms("E", 1, 10) == 120u && cw_train_ms("T", 1, 10) == 360u);
	CHECK(cw_train_ms("?", 1, 12) == 15u * 100u);	/* ..--..: 1+1+3+3+1+1 and 5 gaps */
	CHECK(cw_train_ms("EE", 2, 40) == 5u * 30u && cw_train_ms("", 0, 20) == 0);
	CHECK(cw_train_ms("E#E", 3, 40) == 5u * 30u);	/* a character without Morse: skipped */
	CHECK(cw_train_ms("E", 1, 2) == 1200u / MORSE_WPM_MIN);	/* the speed clamped as the keyer does */
	char p[10];
	cw_train_pattern('K', p);
	CHECK(strcmp(p, "-.-") == 0);
	cw_train_pattern('?', p);
	CHECK(strcmp(p, "..--..") == 0);
	cw_train_pattern('5', p);
	CHECK(strcmp(p, ".....") == 0);
	cw_train_pattern('#', p);
	CHECK(p[0] == 0);
	printf("Timing: PARIS 43 units at 1200 / WPM ms, word gaps 7, patterns\n");
}

static void test_handover(void)
{
	cw_play_t p;
	CHECK(!cw_train_take(&p) && cw_train_state() == CW_PLAY_IDLE);
	cw_train_play("KMURE", 5, 18, 650);
	CHECK(cw_train_state() == CW_PLAY_PENDING);
	CHECK(cw_train_take(&p) && p.len == 5 && memcmp(p.text, "KMURE", 5) == 0 && p.wpm == 18 && p.hz == 650);
	CHECK(!cw_train_take(&p));	/* taken once */
	cw_train_done(true);
	CHECK(cw_train_state() == CW_PLAY_PLAYING);
	/* two before the radio looks: the newer one */
	cw_train_play("AAA", 3, 10, 400);
	cw_train_play("BB", 2, 30, 900);
	CHECK(cw_train_take(&p) && p.len == 2 && memcmp(p.text, "BB", 2) == 0 && p.wpm == 30);
	/* one asked for while the radio handles the last: its state stays pending */
	cw_train_play("CC", 2, 30, 900);
	cw_train_done(false);
	CHECK(cw_train_state() == CW_PLAY_PENDING);
	CHECK(cw_train_take(&p) && memcmp(p.text, "CC", 2) == 0);
	cw_train_done(false);
	CHECK(cw_train_state() == CW_PLAY_REFUSED);
	/* longer than a request holds: cut */
	cw_train_play("ABCDEFGHIJK", 11, 20, 700);
	CHECK(cw_train_take(&p) && p.len == CW_TRAIN_MAX);
	cw_train_done(true);
	printf("Hand-over: pending, taken once, the newest wins, refused / playing\n");
}

/* --- the beep generator's Morse --- */
static int16_t *pcm;
static size_t pcm_n;

/* Blocks from the beep's own stream until it is done (and the route's hold after) */
static void render(void)
{
	pcm_n = 0;
	int16_t mic[AUDIO_BLOCK_SAMPLES] = { 0 };
	for (int b = 0; b < 4000; b++) {
		pcm = realloc(pcm, (pcm_n + AUDIO_BLOCK_SAMPLES) * sizeof *pcm);
		beep_audio_block(mic, pcm + pcm_n, NULL);
		pcm_n += AUDIO_BLOCK_SAMPLES;
		beep_service(false);
		if (!aud.started)
			break;
	}
}

static char got[256];
static size_t got_n;
static void on_text(void *ctx, const char *s)
{
	(void)ctx;
	while (*s && got_n + 1 < sizeof got)
		got[got_n++] = *s++;
	got[got_n] = 0;
}

static void decode(void)
{
	static cwdec_t d;
	got_n = 0;
	got[0] = 0;
	cwdec_init(&d, on_text, NULL);
	cwdec_audio(&d, pcm, (uint16_t)(pcm_n > 60000 ? 60000 : pcm_n));
	for (size_t i = 60000; i < pcm_n; i += 60000)
		cwdec_audio(&d, pcm + i, (uint16_t)(pcm_n - i > 60000 ? 60000 : pcm_n - i));
	int16_t z[CWDEC_N] = { 0 };
	for (int i = 0; i < 300; i++)
		cwdec_audio(&d, z, CWDEC_N);	/* 4.8 s of silence: the gate closes, all is out */
	cwdec_flush(&d);
}

/* The expected segments of a text (in units, + mark / - gap) */
static int expect(const char *t, int *seg)
{
	int n = 0;
	bool any = false, space = false;
	for (; *t; t++) {
		if (*t == ' ') {
			space = true;
			continue;
		}
		char p[10];
		cw_train_pattern(*t, p);
		if (!p[0])
			continue;
		if (any)
			seg[n++] = space ? -7 : -3;
		for (int i = 0; p[i]; i++) {
			if (i)
				seg[n++] = -1;
			seg[n++] = p[i] == '-' ? 3 : 1;
		}
		any = true;
		space = false;
	}
	return n;
}

/* Run lengths of tone / silence between the first and last tone sample, against the units: within the fades */
static bool measure(const char *text, unsigned dot_ms, int *worst)
{
	int seg[256], n = expect(text, seg), k = 0;
	size_t i = 0;
	while (i < pcm_n && abs(pcm[i]) < 100)
		i++;
	*worst = 0;
	while (i < pcm_n && k < n) {
		/* a mark: until 4 ms of silence (the zero crossings inside a tone are shorter) */
		size_t j = i, quiet = 0;
		bool tone = seg[k] > 0;
		if (tone) {
			while (j < pcm_n && quiet < 32) {
				quiet = abs(pcm[j]) < 100 ? quiet + 1 : 0;
				j++;
			}
			j -= quiet;
		} else {
			while (j < pcm_n && abs(pcm[j]) < 100)
				j++;
		}
		int got_s = (int)(j - i), want = abs(seg[k]) * (int)dot_ms * 8;
		int err = abs(got_s - want);
		*worst = err > *worst ? err : *worst;
		if (err > 40) {
			printf("  %s: segment %d is %d samples, %d wanted\n", text, k, got_s, want);
			return false;
		}
		i = j;
		k++;
	}
	return k == n;
}

static void test_beep(void)
{
	const beep_seg_t *segs;
	CHECK(beep_pattern(BEEP_MORSE, &segs) == 1 && segs[0].hz != 0);	/* the radio's beep list keeps one */
	beep_init(&fake_audio);
	/* BEEP_MORSE before any text: the table's dot */
	beep_play(BEEP_MORSE, 100, false);
	render();
	size_t on = 0;
	for (size_t i = 0; i < pcm_n; i++)
		on += abs(pcm[i]) > 100;
	CHECK(on > 300 && on < 480);
	static const struct {
		const char *text;
		uint8_t wpm;
		uint16_t hz;
	} cases[] = {
		{ "PARIS", 20, 700 }, { "KMURE", 5, 400 }, { "CQ DE TA1ABC", 25, 600 }, { "5/?=.,", 30, 1000 },
		{ "QX9", 40, 800 }, { "SOS", 12, 550 },
	};
	int worst_all = 0;
	for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; c++) {
		const char *t = cases[c].text;
		beep_set_morse(t, (uint8_t)strlen(t), cases[c].hz, morse_dot_ms(cases[c].wpm));
		beep_play(BEEP_MORSE, 80, false);
		CHECK(beep_morse_playing() && aud.started);
		render();
		CHECK(!beep_morse_playing() && !aud.started && !aud.pcm);
		int worst;
		CHECK(measure(t, morse_dot_ms(cases[c].wpm), &worst));
		worst_all = worst > worst_all ? worst : worst_all;
		decode();
		CHECK(strcmp(got, t) == 0);
		if (strcmp(got, t) != 0)
			printf("  played %s at %u WPM, decoded \"%s\"\n", t, cases[c].wpm, got);
		/* the sound from the first to the last tone sample: the Morse's own time */
		size_t first = 0, last = 0;
		for (size_t i = 0; i < pcm_n; i++)
			if (abs(pcm[i]) > 100) {
				if (!first)
					first = i;
				last = i;
			}
		long span = (long)(last - first + 1), want = (long)cw_train_ms(t, (uint8_t)strlen(t), cases[c].wpm) * 8;
		CHECK(span <= want && span >= want - 40);
	}
	/* longer than 12: cut to 12 */
	beep_set_morse("EEEEEEEEEEEEEEEE", 16, 700, 60);
	beep_play(BEEP_MORSE, 80, false);
	render();
	decode();
	CHECK(strcmp(got, "EEEEEEEEEEEE") == 0);
	/* mixed into a voice stream (DMR voice: shared) */
	beep_set_morse("TEST", 4, 650, 60);
	beep_play(BEEP_MORSE, 80, true);
	pcm_n = 0;
	for (int b = 0; b < 400 && beep_busy(); b++) {
		pcm = realloc(pcm, (pcm_n + AUDIO_BLOCK_SAMPLES) * sizeof *pcm);
		memset(pcm + pcm_n, 0, AUDIO_BLOCK_SAMPLES * sizeof *pcm);
		beep_mix(pcm + pcm_n);
		pcm_n += AUDIO_BLOCK_SAMPLES;
	}
	decode();
	CHECK(strcmp(got, "TEST") == 0);
	/* another beep replaces it at once (the radio core leaves key beeps out meanwhile) */
	beep_set_morse("PARIS", 5, 700, 60);
	beep_play(BEEP_MORSE, 80, false);
	int16_t mic[AUDIO_BLOCK_SAMPLES] = { 0 }, blk[AUDIO_BLOCK_SAMPLES];
	beep_audio_block(mic, blk, NULL);
	beep_audio_block(mic, blk, NULL);
	CHECK(beep_morse_playing());
	beep_play(BEEP_ERROR, 80, false);
	beep_audio_block(mic, blk, NULL);
	CHECK(!beep_morse_playing());
	render();
	printf("Beep Morse: 6 texts at 5-40 WPM, every mark and gap within %d samples, decoded back; mixed into voice; "
	       "the table dot; replaced\n", worst_all);
}

int main(void)
{
	test_koch();
	test_score();
	test_timing();
	test_handover();
	test_beep();
	free(pcm);
	printf("CW practice: %d checks, %d failures\n", checks, fails);
	return fails != 0;
}
