/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus round 15 scan job (docs/plus.md "Search ranges and speed") through the real UI in upstream's simulator
 * (tests/plus/ui_sim_scan.c wraps it), on the DM-32UV's display class (240 x 320, insets 10 x 5), in English and in
 * Turkish. Each case runs in a child process (the screens' static state starts over). Covers item 1, CONFIG_SEARCH_RANGE,
 * on Tools > P25 search and DMR search:
 *   - the range line with the step; Left / Right through the presets; * opens the editor, a digit opens it typing Lower;
 *   - typing Lower and Upper (OK goes on, OK applies), Back deleting a digit, Back leaving without a change;
 *   - the step row (every step, both ways), Left / Right on Lower / Upper / Move by one step, held by 1 MHz;
 *   - refusals: Upper not above Lower, a range across two bands, an end moved out of the band;
 *   - the request: the range and its step as the radio core decodes them; a running search starts again on an
 *     applied range, a stopped one waits for OK;
 *   - the range and step kept when the screen is left and opened again, the manual range then among the presets;
 *   - the layout of the screen and the editor. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "sim/ui_sim.h"
#include "ui/ui_app.h"
#include "ui/ui_nav.h"
#include "ui/i18n.h"
#include "ui_sim_scan.h"

static int fails;
static const char *name;
static bool turkish;

/* English or Turkish (Turkish: an ASCII part of the translation, as the screen draws it) */
#define T(en, tr) (turkish ? (tr) : (en))

static bool run(const char *script)
{
	char err[300];
	if (!sim_run(script, err, sizeof err)) {
		printf("FAIL %s (%s): %s: %s\n", name, turkish ? "tr" : "en", script, err);
		fails++;
		return false;
	}
	return true;
}

static void expect(const char *e)
{
	char buf[600], *o = buf;
	while (*e && o < buf + sizeof buf - 16) {
		while (*e == ' ')
			e++;
		if (!*e)
			break;
		memcpy(o, "expect:", 7);
		o += 7;
		while (*e && *e != ' ' && o < buf + sizeof buf - 2)
			*o++ = *e++;
		*o++ = ' ';
	}
	*o = 0;
	run(buf);
}

static void check(bool c, const char *what)
{
	if (!c) {
		printf("FAIL %s (%s): %s\n", name, turkish ? "tr" : "en", what);
		fails++;
	}
}

static void layout(void)
{
	char issues[SIM_LAYOUT_MAX][160];
	run("wait:200");
	int n = sim_layout_issues(issues, SIM_LAYOUT_MAX);
	for (int i = 0; i < n; i++)
		printf("FAIL %s (%s): layout: %s\n", name, turkish ? "tr" : "en", issues[i]);
	fails += n;
}

/* The requests recorded since the last call, compared */
static void requests(const char *want)
{
	if (strcmp(sc_sim.req, want) != 0) {
		printf("FAIL %s (%s): requests \"%s\", expected \"%s\"\n", name, turkish ? "tr" : "en", sc_sim.req, want);
		fails++;
	}
	sc_sim.req[0] = 0;
}

static void start(const char *n, ui_scr_t scr)
{
	name = n;
	sim_reset(sim_geom_find("240x320i10x5"), SIM_MAP_M);	/* Left / Right, * and #; held Left / Right repeat (the DM-32 sends USER_1 / USER_4) */
	sc_sim_clear();
	const char *shots = getenv("SCAN_SHOTS");	/* a directory: the "shot:" tokens save the screens there */
	if (shots != NULL)
		sim_set_shot_dir(shots, turkish ? "scan-tr-" : "scan-en-");
	if (turkish && !lang_set(lang_find('t' << 8 | 'r'))) {
		printf("FAIL %s: no Turkish in this build\n", name);
		fails++;
	}
	run("wait:200");
	ui_push(scr, 0);
	run("wait:200");
}

/* The DM-32's held Left / Right (its long actions; the simulator's maps have none) */
static void user_long(uint8_t action)
{
	input_event_t ev = { action, GESTURE_LONG, 0 };
	ui_app_key(&ev);
	run("wait:100");
}

/* The editor's rows: Lower, Upper, Step, Move */
static const char *ed_title(void) { return T("shows=Search_range,_MHz", "shows=Arama_aral"); }

/* --- P25 search --- */

/* The line names the preset and the step; Left / Right go through the presets (no manual one yet) */
static void p25_presets(void)
{
	start("P25 presets and the range line", SCR_P25SEARCH);
	expect(T("screen=p25search shows=Range_+-1_MHz__12.5k shows=*_", "screen=p25search shows=+-1_MHz__12.5k"));
	layout();
	run("right");
	expect("shows=+-2.5_MHz__12.5k");
	run("right right");
	expect("shows=144-146_MHz__12.5k");
	run("right");	/* round to the first: no manual range yet */
	expect("shows=+-500_kHz__12.5k");
	run("left");
	expect("shows=144-146_MHz");
	run("ok");
	requests("p25s:144000-146000/12500");
}

/* Typing: a digit opens the editor typing Lower; OK goes on to Upper; OK applies; the search waits for OK */
static void p25_type(void)
{
	start("P25 typed range", SCR_P25SEARCH);
	run("4 3 3");
	expect(T("shows=Search_range shows=433. shows=Lower", "shows=Arama_aral shows=433. shows=Alt"));
	layout();
	run("5 ok");
	expect("shows=433.5 hides=433.5-");
	run("4 3 4 ok");
	expect(T("screen=p25search shows=Range_433.5-434__12.5k hides=Search_range",
		 "screen=p25search shows=433.5-434__12.5k hides=Arama_aral"));
	layout();
	check(sc_sim.requests == 0, "nothing asked of the radio before OK");
	run("ok");
	requests("p25s:433500-434000/12500");
	run("shot:p25-typed");
}

/* Back deletes the last digit, then leaves without a change */
static void p25_back(void)
{
	start("P25 editor Back", SCR_P25SEARCH);
	run("quick");
	expect(ed_title());
	run("1 4 5 back");
	expect("shows=14");
	run("back back back");	/* the two digits, then the editor */
	expect(T("screen=p25search shows=Range_+-1_MHz__12.5k hides=Search_range", "screen=p25search shows=+-1_MHz"));
	check(sc_sim.requests == 0, "nothing asked");
}

/* The step row: every step in order, round both ways; the applied step reaches the request */
static void p25_step(void)
{
	static const char *const steps[10] = { "20_kHz", "25_kHz", "50_kHz", "100_kHz", "1_MHz", "2.5_kHz", "5_kHz",
					       "6.25_kHz", "10_kHz", "12.5_kHz" };
	start("P25 steps", SCR_P25SEARCH);
	run("quick down down");
	expect("shows=12.5_kHz");
	layout();
	char e[64];
	for (int i = 0; i < 10; i++) {
		run("right");
		snprintf(e, sizeof e, "shows=%s", steps[i]);
		expect(e);
	}
	run("left left");	/* 12.5 -> 10 -> 6.25 */
	expect("shows=6.25_kHz");
	run("up up 1 4 4 ok 1 4 6 ok");
	expect("screen=p25search shows=144-146__6.25k");
	run("ok");
	requests("p25s:144000-146000/6250");
	/* 2.5 kHz and 1 MHz both ride on the start */
	run("ok quick down down left left left left left");	/* 6.25 -> 5 -> 2.5 -> 1 MHz -> 100 -> 50 */
	expect("shows=50_kHz");
	run("right right ok");	/* 1 MHz; the search was stopped: it waits */
	requests("p25s:stop");
	run("ok");
	requests("p25s:144000-146000/1000000");
}

/* Left / Right: one step on Lower, Upper and Move; held (the DM-32's long Left / Right): 1 MHz */
static void p25_move(void)
{
	start("P25 moving the range", SCR_P25SEARCH);
	run("quick 4 3 0 ok 4 4 0");	/* Upper typed, not applied yet */
	run("down right");		/* Upper taken; Step 20 kHz */
	run("down right");		/* Move: +20 kHz */
	expect("shows=430.02 shows=440.02 shows=10_MHz");
	run("long:right");		/* +1 MHz */
	expect("shows=431.02 shows=441.02");
	user_long(ACT_USER_1);	/* the DM-32's held Left: -1 MHz */
	run("long:left left");	/* -1 MHz, -20 kHz */
	expect("shows=429 shows=439 hides=429.02");
	run("up up up right");		/* Lower +20 kHz */
	expect("shows=429.02 shows=439");
	run("down");
	user_long(ACT_USER_1);	/* Upper -1 MHz */
	expect("shows=438 hides=439");
	layout();
	run("shot:p25-editor");
	run("ok");
	expect("shows=429.02-438__20k");
	run("ok");
	requests("p25s:429020-438000/20000");
}

/* Refusals: Upper not above Lower, a range across two bands, an end moved out of its band */
static void p25_refuse(void)
{
	start("P25 refusals", SCR_P25SEARCH);
	run("quick 4 3 5 ok 4 3 4 ok");
	expect(T("toast=Upper_must_be_above_Lower", "toast=*"));
	expect(ed_title());
	run("up 1 7 3 ok 4 0 1 ok");	/* 173-401 MHz: two bands */
	expect(T("toast=Range_must_lie_in_one_band", "toast=Aral"));
	expect(ed_title());
	run("up 4 7 9 ok 4 8 0 ok");	/* 479-480 */
	expect("screen=p25search shows=479-480");
	run("quick down down down");
	user_long(ACT_USER_4);	/* Move +1 MHz: Upper would leave the band */
	expect("shows=479 shows=480 shows=1_MHz");
	run("back");
	check(sc_sim.requests == 0, "nothing asked");
}

/* A running search starts again on an applied range; the manual range is kept with its step when the screen is left
 * and opened again, and is one of the presets from then on */
static void p25_running_kept(void)
{
	start("P25 running search, range kept", SCR_P25SEARCH);
	run("ok");
	sc_sim.req[0] = 0;
	run("quick 1 4 5 ok 1 4 5 5 ok");
	requests("p25s:145000-145500/12500");
	run("back");	/* stops it and leaves */
	requests("p25s:stop");
	int asked = sc_sim.requests;
	ui_push(SCR_P25SEARCH, 0);
	run("wait:200");
	expect("screen=p25search shows=145-145.5__12.5k");
	run("left");
	expect("shows=144-146_MHz");
	run("right");
	expect("shows=145-145.5");
	run("right");
	expect("shows=+-500_kHz");
	check(sc_sim.requests == asked, "Left / Right on a stopped search ask nothing");
}

/* --- DMR search: the same editor and request --- */

static void dmr_range(void)
{
	start("DMR typed range and step", SCR_DMRSEARCH);
	expect(T("screen=dmrsearch shows=Range_+-1_MHz__12.5k shows=*_", "screen=dmrsearch shows=+-1_MHz__12.5k"));
	layout();
	run("4 3 8 ok 4 3 9 ok");
	expect("shows=438-439__12.5k");
	run("quick down down right right");	/* 25 kHz */
	expect("shows=25_kHz");
	layout();
	run("ok");
	expect("shows=438-439__25k");
	layout();
	run("ok");
	requests("dmrs:438000-439000/25000");
	run("quick down down right ok");	/* running: 50 kHz, started again */
	requests("dmrs:438000-439000/50000");
	run("shot:dmr-range");
}

typedef void (*case_fn)(void);
static const case_fn all[] = {
	p25_presets, p25_type, p25_back, p25_step, p25_move, p25_refuse, p25_running_kept, dmr_range,
};

int main(void)
{
	int bad = 0, cases = 0;
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
			cases++;
			if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
				bad++;
				if (!WIFEXITED(st))
					printf("FAIL case %u (%s): crashed\n", i, l ? "tr" : "en");
			}
		}
	}
	printf("Plus search range UI: %d cases (English and Turkish), %d failed\n", cases, bad);
	return bad != 0;
}
