/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus round 14 air items (docs/plus.md "Round 14: air band and spectrum") through the real UI in upstream's
 * simulator (tests/plus/ui_sim_air.c wraps it: a radio that receives AM, the test's position), on the DM-32UV's display
 * class (240 x 320, insets 10 x 5), in English and in Turkish. Each case runs in a child process (the screens' static
 * state starts over). Covers:
 *   - item 44a, Tools > Airfields after the table's cleanup: "+N" on the list line when the service shown has more
 *     frequencies (Antalya's two towers), none with one; an airfield's every frequency in its details, the list
 *     scrolling to the last one and OK tuning it; every page fits. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "sim/ui_sim.h"
#include "ui/ui_app.h"
#include "ui/ui_nav.h"
#include "ui/i18n.h"
#include "ui_sim_air.h"

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

/* "a=b c=d": every check after the script, as tests/host/uiFlowTest.c's run_flow */
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

static void layout(void)
{
	char issues[SIM_LAYOUT_MAX][160];
	run("wait:200");
	int n = sim_layout_issues(issues, SIM_LAYOUT_MAX);
	for (int i = 0; i < n; i++)
		printf("FAIL %s (%s): layout: %s\n", name, turkish ? "tr" : "en", issues[i]);
	fails += n;
}

static void pos(double la, double lo)
{
	air_sim.pos = true;
	air_sim.lat_e7 = (int32_t)(la * 1e7 + (la < 0 ? -0.5 : 0.5));
	air_sim.lon_e7 = (int32_t)(lo * 1e7 + (lo < 0 ? -0.5 : 0.5));
}

static void start(const char *n)
{
	name = n;
	sim_reset(sim_geom_find("240x320i10x5"), SIM_MAP_K);
	const char *shots = getenv("AIR_SHOTS");	/* a directory: the "shot:" tokens save the screens there */
	if (shots != NULL)
		sim_set_shot_dir(shots, turkish ? "air-tr-" : "air-en-");
	air_sim_setup();
	if (turkish && !lang_set(lang_find('t' << 8 | 'r'))) {
		printf("FAIL %s: no Turkish in this build\n", name);
		fails++;
	}
	pos(39.9208, 32.8541);		/* Ankara, Kizilay */
	run("wait:200");
}

static void airfields(void)
{
	ui_push(SCR_AIRFIELDS, 0);
	run("wait:100");
}

/* --- Item 44a: the airfield table --- */

/* Antalya: two tower frequencies, "+1" after the first; its details list all seven, the last one reached by
 * scrolling and tuned */
static void af_more(void)
{
	start("AF several frequencies on one service");
	pos(36.8969, 30.7133);		/* Antalya */
	airfields();
	expect(T("screen=airfields shows=Nearest_first shows=LTAI shows=TWR_125.000_+1",
		 "screen=airfields shows=En_yak shows=LTAI shows=TWR_125.000_+1"));
	run("shot:airfields-more");
	layout();
	run("ok");
	expect("shows=ATIS_118.275 shows=GND_121.900 shows=TWR_125.000 hides=OPS_131.450 hides=DEL_123.175");
	layout();
	run("down down down down down down");	/* ATIS, GND, TWR, TWR, APP, APP, APP 124.425 */
	expect("shows=APP_124.425");
	run("shot:airfield-last");
	layout();
	run("ok");
	expect("screen=home vfo=1 freq=124425000");
}

/* Kizilay: Guvercinlik's one tower, no "+"; Etimesgut keeps only its tower (Esenboga approach's 119.100 removed) */
static void af_one(void)
{
	start("AF one frequency on a service");
	airfields();
	expect("shows=LTAB shows=TWR_132.000 hides=TWR_132.000_+");
	run("down ok");
	expect("shows=LTAD shows=TWR_122.100 hides=APP_119.100");
	layout();
}

/* Kastamonu: none in the data; the details say so within the panel */
static void af_none(void)
{
	start("AF no frequency");
	pos(41.3142, 33.7958);
	airfields();
	run("ok");
	expect(T("shows=LTAL shows=No_frequencies", "shows=LTAL shows=rekans_yok"));
	layout();
}

typedef void (*case_fn)(void);
static const case_fn all[] = { af_more, af_one, af_none };

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
	printf("Plus air UI: %d cases (English and Turkish), %d failed\n", cases, bad);
	return bad != 0;
}
