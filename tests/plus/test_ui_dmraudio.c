/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus DMR receive audio diagnostics (tools/plus_dmraudio.py) on Tools > Diagnostics, through the real UI in
 * upstream's simulator on the DM-32UV's display class (240 x 320, insets 10 x 5, key map M), in English and Turkish.
 * The page's six new lines come from the real formatter (dmraudio_line and dm32_diag_digit, cut out of the generated
 * boards/dm32/diag_dm32.c by tools/test_dmraudio.py into DMRAUDIO_LINES_INC) over faked counters. Covers: the lines'
 * text and width with the largest counters, the page showing them first, 0 stepping the mode through m0..m3 and back,
 * 8 stepping the voice gain through x1..x8 and back, 9 zeroing the counters (the mode and the gain stay), other
 * digits left alone, and the layout check in both languages. Each case runs in a child process. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "sim/ui_sim.h"
#include "ui/ui_app.h"
#include "ui/ui_nav.h"
#include "ui/i18n.h"
#include "dmr/ambe/ambeVoice.h"
#include "board.h"

/* --- The counters behind the lines, faked --- */
#include DMRAUDIO_TYPES_INC	/* hrc_pcm_diag_t and HRC_PCM_* from the generated audio_pcm.h */
static ambe_voice_stats_t fa;
static hrc_pcm_diag_t fp;
static int resets, voice_resets;
static uint8_t fgain;
static uint32_t fclipped, fpeak_out;

static uint8_t fake_rx_gain(void)
{
	return fgain;
}

static void fake_set_rx_gain(uint8_t shift)
{
	fgain = shift & 3u;
}

static void fake_rx_level(uint32_t *clipped, uint32_t *peak_out)
{
	*clipped = fclipped;
	*peak_out = fpeak_out;
}

static void fake_voice_stats(ambe_voice_stats_t *out)
{
	*out = fa;
}

static void fake_voice_reset(void)
{
	memset(&fa, 0, sizeof fa);
	fclipped = fpeak_out = 0;
	voice_resets++;
}

static void hrc_audio_pcm_diag(hrc_pcm_diag_t *out)
{
	*out = fp;
}

static void hrc_audio_pcm_set_mode(uint8_t mode)
{
	fp.mode = mode & (HRC_PCM_PREFILL | HRC_PCM_PACED);
}

static uint8_t hrc_audio_pcm_mode(void)
{
	return (uint8_t)fp.mode;
}

static void dm32_diag_reset(void)
{
	uint32_t mode = fp.mode;
	memset(&fp, 0, sizeof fp);
	fp.mode = mode;
	fp.lat_min_us = UINT32_MAX;
	resets++;
}

bool dm32_diag_digit(uint8_t d);
#define ambeVoiceGetStats fake_voice_stats
#define ambeVoiceResetStats fake_voice_reset
#define ambeVoiceRxGain fake_rx_gain
#define ambeVoiceSetRxGain fake_set_rx_gain
#define ambeVoiceRxLevel fake_rx_level
#include DMRAUDIO_LINES_INC
#undef ambeVoiceGetStats
#undef ambeVoiceResetStats
#undef ambeVoiceRxGain
#undef ambeVoiceSetRxGain
#undef ambeVoiceRxLevel

/* The page's lines: ours first (as dm32_diag_line), then two of the board's */
static bool page_line(uint8_t i, char *buf, uint8_t len)
{
	if (i < DMRAUDIO_LINES)
		return dmraudio_line(i, buf, len);
	i -= DMRAUDIO_LINES;
	if (i < 2) {
		snprintf(buf, len, i == 0 ? "DMR on+cb tk123" : "ts 1/2 si3");
		return true;
	}
	return false;
}

/* --- The checks --- */
static int fails;
static const char *name;
static bool turkish;

static void ok_run(const char *script)
{
	char err[300];
	if (!sim_run(script, err, sizeof err)) {
		printf("FAIL %s: %s: %s\n", name, script, err);
		fails++;
	}
}

static void check(bool c, const char *what)
{
	if (!c) {
		printf("FAIL %s: %s\n", name, what);
		fails++;
	}
}

static void layout(void)
{
	char issues[SIM_LAYOUT_MAX][160];
	int n = sim_layout_issues(issues, SIM_LAYOUT_MAX);
	for (int k = 0; k < n; k++)
		printf("FAIL %s: layout: %s\n", name, issues[k]);
	fails += n != 0;
}

static void big_counters(void)
{
	fa.rx_in = fa.rx_frames = 4000000000u;
	fa.rx_concealed = fa.rx_underruns = 4000000000u;
	fa.rx_bad = fa.rx_lost = fa.rx_bit_errors = 4000000000u;
	fa.rx_queue_max = 8;
	fa.rx_peak = 32768;
	fp.gaps = fp.full = fp.dropped = fp.slow = fp.overruns = 4000000000u;
	fp.gap_max_us = fp.lat_max_us = 4000000000u;
	fp.lat_min_us = 1234;
	fp.loud_blocks = 5;
	fclipped = fpeak_out = 4000000000u;
	fgain = 3;
}

static void start(const char *n)
{
	name = n;
	sim_reset(sim_geom_find("240x320i10x5"), SIM_MAP_M);
	board_t *b = (board_t *)&board;	/* the simulator's board is writable (sim/ui_sim.c) */
	b->diag_line = page_line;
	b->diag_digit = dm32_diag_digit;
	memset(&fa, 0, sizeof fa);
	memset(&fp, 0, sizeof fp);
	fp.lat_min_us = UINT32_MAX;
	fgain = 0;
	fclipped = fpeak_out = 0;
	if (turkish && !lang_set(lang_find('t' << 8 | 'r'))) {
		printf("FAIL %s: no Turkish in this build\n", name);
		fails++;
	}
	ok_run("wait:100");
	ui_push(SCR_DIAG, 0);
	ok_run("wait:300");
}

/* The formatter alone: the text, and the width with every counter at its largest (26 characters fit the page) */
static void lines_text(void)
{
	name = "lines: text and width";
	char b[40];
	memset(&fa, 0, sizeof fa);
	memset(&fp, 0, sizeof fp);
	fp.lat_min_us = UINT32_MAX;
	fa.rx_in = 150;
	fa.rx_frames = 147;
	fa.rx_concealed = 2;
	fa.rx_underruns = 1;
	fa.rx_bad = 3;
	fa.rx_lost = 4;
	fa.rx_bit_errors = 56;
	fa.rx_queue_max = 4;
	fa.rx_peak = 12000;
	fp.mode = HRC_PCM_PREFILL;
	fp.gaps = 7;
	fp.gap_max_us = 4049;
	fp.full = 300;
	fp.dropped = 1;
	fp.overruns = 2;
	fgain = 1;
	fclipped = 12;
	fpeak_out = 24000;
	check(dmraudio_line(0, b, sizeof b) && strcmp(b, "dv m1 x2 in150 pl147") == 0, b);
	check(dmraudio_line(1, b, sizeof b) && strcmp(b, "dv cc2 un1 q4 pk12000") == 0, b);
	check(dmraudio_line(2, b, sizeof b) && strcmp(b, "dv bd3 ls4 be56") == 0, b);
	check(dmraudio_line(3, b, sizeof b) && strcmp(b, "sp gp7/4.0 fu300 dr1") == 0, b);
	check(dmraudio_line(4, b, sizeof b) && strcmp(b, "au bl- sl0 ov2") == 0, b);	/* no loud block yet */
	fp.loud_blocks = 9;
	fp.lat_min_us = 2049;
	fp.lat_max_us = 7951;
	fp.slow = 1;
	check(dmraudio_line(4, b, sizeof b) && strcmp(b, "au bl2.0-8.0 sl1 ov2") == 0, b);
	check(dmraudio_line(5, b, sizeof b) && strcmp(b, "dv cl12 po24000") == 0, b);
	check(!dmraudio_line(6, b, sizeof b), "six lines");
	big_counters();
	for (uint8_t i = 0; i < DMRAUDIO_LINES; i++) {
		check(dmraudio_line(i, b, sizeof b) && strlen(b) <= 26, "26 characters at most");
		char s[8];
		check(dmraudio_line(i, s, sizeof s) && strlen(s) == sizeof s - 1, "cut at a short buffer");
	}
	check(dmraudio_line(3, b, sizeof b) && strcmp(b, "sp gp999/99.9 fu9999 dr999") == 0, b);
	check(dmraudio_line(4, b, sizeof b) && strcmp(b, "au bl1.2-99.9 sl999 ov999") == 0, b);
	check(dmraudio_line(0, b, sizeof b) && strcmp(b, "dv m1 x8 in99999 pl99999") == 0, b);
	check(dmraudio_line(5, b, sizeof b) && strcmp(b, "dv cl99999 po99999") == 0, b);
}

static void page_first(void)
{
	start("page: our lines first");
	ok_run("expect:screen=diag");
	ok_run("expect:shows=dv_m0_x1 expect:shows=dv_cl0_po0 expect:shows=sp_gp0 expect:shows=au_bl- expect:shows=DMR_on");
	layout();
	big_counters();
	ok_run("wait:300");
	layout();
}

static void digit_mode(void)
{
	start("page: 0 steps the mode");
	ok_run("0");
	check(fp.mode == 1, "m1");
	ok_run("wait:300 expect:shows=dv_m1 expect:screen=diag");
	ok_run("0");
	check(fp.mode == 2, "m2");
	ok_run("0");
	check(fp.mode == 3, "m3");
	ok_run("wait:300 expect:shows=dv_m3");
	ok_run("0");
	check(fp.mode == 0, "back to m0");
	layout();
}

static void digit_gain(void)
{
	start("page: 8 steps the voice gain");
	ok_run("8");
	check(fgain == 1 && fp.mode == 0, "x2, the mode unchanged");
	ok_run("wait:300 expect:shows=dv_m0_x2 expect:screen=diag");
	ok_run("8");
	check(fgain == 2, "x4");
	ok_run("8");
	check(fgain == 3, "x8");
	ok_run("wait:300 expect:shows=x8");
	layout();
	ok_run("9");
	check(fgain == 3 && resets == 1, "9 keeps the gain");
	ok_run("8");
	check(fgain == 0, "back to x1");
	ok_run("wait:300 expect:shows=dv_m0_x1");
}

static void digit_reset(void)
{
	start("page: 9 zeroes the counters");
	fa.rx_in = 77;
	fp.gaps = 5;
	fp.mode = HRC_PCM_PACED;
	ok_run("wait:300 expect:shows=in77 expect:shows=gp5");
	ok_run("9");
	check(resets == 1 && voice_resets == 1 && fa.rx_in == 0 && fp.gaps == 0, "zeroed");
	check(fp.mode == HRC_PCM_PACED, "the mode stays");
	ok_run("wait:300 expect:shows=in0 expect:shows=gp0 expect:screen=diag");
	/* other digits: not ours, the page stays and nothing changes */
	ok_run("5 wait:300 expect:screen=diag");
	check(resets == 1 && fp.mode == HRC_PCM_PACED, "5 does nothing");
	ok_run("quick wait:300 expect:screen=diag");	/* * still pages */
	layout();
}

typedef void (*case_fn)(void);
static const case_fn all[] = { lines_text, page_first, digit_mode, digit_gain, digit_reset };

int main(void)
{
	int bad = 0, run = 0;
	for (int l = 0; l < 2; l++) {
		for (unsigned i = 0; i < sizeof all / sizeof all[0]; i++) {
			fflush(stdout);
			pid_t pid = fork();
			if (pid == 0) {
				turkish = l == 1;
				all[i]();
				fflush(stdout);
				_exit(fails != 0);
			}
			int st = 0;
			waitpid(pid, &st, 0);
			run++;
			if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
				bad++;
				if (!WIFEXITED(st))
					printf("FAIL case %u (%s): crashed\n", i, l ? "tr" : "en");
			}
		}
	}
	printf("DMR receive audio diagnostics page: %d cases (English and Turkish), %d failed\n", run, bad);
	return bad != 0;
}
