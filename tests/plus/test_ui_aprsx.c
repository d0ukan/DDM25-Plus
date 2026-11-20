/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus round 14 APRS job through the real UI in upstream's simulator (tests/plus/ui_sim_aprsx.c wraps it), on
 * the DM-32UV's display class (240 x 320, insets 10 x 5), in English and in Turkish, every page checked for layout.
 * Each case runs in a child process (the screens' static state starts over). Covers:
 *   - item 47: Settings > APRS (the last group): the beacon format, Standard by default; Mic-E shows the message row,
 *     En route first; the messages M0-M6 round (never the emergency); the standard format again hides it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "sim/ui_sim.h"
#include "ui/ui_app.h"
#include "ui/ui_nav.h"
#include "ui/i18n.h"
#include "ui/ui_svc.h"
#include "ui_sim_aprsx.h"

static int fails;
static const char *name;
static bool turkish;

/* English or Turkish (Turkish: an ASCII part of the translation, as the screen draws it) */
#define T(en, tr) (turkish ? (tr) : (en))

static void ok_run(const char *script)
{
	char err[300];
	if (!sim_run(script, err, sizeof err)) {
		printf("FAIL %s (%s): %s: %s\n", name, turkish ? "tr" : "en", script, err);
		fails++;
		if (getenv("AX_DUMP"))
			ax_sim_dump();
	}
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
	ok_run(buf);
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
	int n = sim_layout_issues(issues, SIM_LAYOUT_MAX);
	for (int k = 0; k < n; k++)
		printf("FAIL %s (%s): layout: %s\n", name, turkish ? "tr" : "en", issues[k]);
	fails += n != 0;
}

static void start(void)
{
	sim_reset(sim_geom_find("240x320i10x5"), SIM_MAP_K);
	ax_sim_clear();
	const char *shots = getenv("AX_SHOTS");	/* a directory: the pages as PNG files */
	if (shots != NULL)
		sim_set_shot_dir(shots, turkish ? "tr-" : "en-");
	if (turkish && !lang_set(lang_find('t' << 8 | 'r'))) {
		printf("FAIL %s: no Turkish in this build\n", name);
		fails++;
	}
	ok_run("wait:100");
}

/* --- item 47: Settings > APRS, the beacon format and the Mic-E message --- */

static void aprs_settings(void)
{
	name = "I47 Settings > APRS";
	ok_run("ok 7 wait:100 up wait:100");	/* the last group, without a digit */
	expect("screen=settings shows=APRS");
	ok_run("ok wait:100");
	expect(T("screen=group shows=Beacon_format shows=Standard hides=Mic-E_message",
		 "screen=group shows=Beacon_bi shows=Standart"));
	check(ui_svc_setting_get(UI_SET_APRS_MICE) == 0, "the standard format by default");
	layout();
	ok_run("shot:aprs-standard");
	ok_run("ok wait:100");	/* Mic-E: the message row appears, En route */
	expect(T("shows=Mic-E shows=Mic-E_message shows=En_route", "shows=Mic-E shows=Mic-E_mesaj shows=Yolda"));
	check(ui_svc_setting_get(UI_SET_APRS_MICE) == 1 && ui_svc_setting_get(UI_SET_APRS_MICE_MSG) == 1,
	      "Mic-E, En route");
	layout();
	ok_run("shot:aprs-mice");
	ok_run("down ok wait:100");
	expect(T("shows=In_service", "shows=Hizmette"));
	check(ui_svc_setting_get(UI_SET_APRS_MICE_MSG) == 2, "In service");
	/* every message once round: Off duty .. Priority, back to Off duty; never the emergency */
	for (int i = 0; i < 5; i++)
		ok_run("ok wait:50");
	expect(T("shows=Off_duty", "shows=G"));
	check(ui_svc_setting_get(UI_SET_APRS_MICE_MSG) == 0, "M0 Off duty after M6");
	layout();
	ok_run("up ok wait:100");	/* the standard format again: no message row */
	expect(T("shows=Standard hides=Mic-E_message", "shows=Standart"));
	check(ui_svc_setting_get(UI_SET_APRS_MICE) == 0, "standard again");
}

typedef void (*case_fn)(void);
static const case_fn all[] = {
	aprs_settings,
};

int main(void)
{
	int bad = 0, run = 0;
	for (int l = 0; l < 2; l++) {
		for (unsigned i = 0; i < sizeof all / sizeof all[0]; i++) {
			fflush(stdout);
			pid_t pid = fork();
			if (pid == 0) {
				turkish = l == 1;
				name = "start";
				start();
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
	printf("Plus round 14 APRS UI: %d cases (English and Turkish), %d failed\n", run, bad);
	return bad != 0;
}
