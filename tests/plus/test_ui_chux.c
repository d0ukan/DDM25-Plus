/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus r15 "channels" (tools/plus_chux.py, docs/plus.md "Channels, zones and DMR rows") through the real UI in
 * upstream's simulator, on the DM-32UV's display class (240 x 320, insets 10 x 5) with its key map, in English and in
 * Turkish. Each case runs in a child process (the screens' static state starts over); the layout check on every
 * screen a case ends on. Covers:
 *   1. zone size: the zone list's Add channel "16/80", the channel editor's Zones "12/80" beside a zone it is not in,
 *      Zones > Create New Zone "3/68"; a full zone (80): Add channel dimmed "80/80" and refused "Zone full: max 80
 *      channels", Copy here refused the same way. */
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
#include "codeplug/codeplug.h"

static int fails;
static const char *name;
static bool turkish;

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

static void start(const char *n)
{
	name = n;
	sim_reset(sim_geom_find("240x320i10x5"), SIM_MAP_K);
	const char *shots = getenv("CHUX_SHOTS");	/* a directory: the "shot:" tokens save the screens there */
	if (shots != NULL)
		sim_set_shot_dir(shots, turkish ? "chux-tr-" : "chux-en-");
	if (turkish && !lang_set(lang_find('t' << 8 | 'r'))) {
		printf("FAIL %s: no Turkish in this build\n", name);
		fails++;
	}
	run("wait:200");
}

/* ---------------- 1. zone size ---------------- */

static void case_zone_counts(void)
{
	start("zone list: Add channel 16/80");
	run("ok 4 ctx");
	for (int k = 0; k < 20 && strcmp(ui_app_focus_label(), T("Add channel", "Kanal ekle")) != 0; k++)
		run("up");
	expect(T("screen=zone focus=Add_channel shows=16/80", "screen=zone focus=Kanal_ekle shows=16/80"));
	layout();
	start("channel editor Zones: 12/80 beside Simplex");
	run("ok 5 ok up up up up ok");
	expect("screen=chzones shows=12/80 shows=12/80");
	expect(T("shows=In", "shows=Var"));
	layout();
	start("Create New Zone: 3/68");
	run("ok 4 down down down");
	expect("screen=zones shows=3/68");
	layout();
}

static void case_zone_full(void)
{
	start("full zone: Add channel 80/80, refused");
	sim_worst(true);
	run("ok 4 ctx");
	for (int k = 0; k < 90 && strcmp(ui_app_focus_label(), T("Add channel", "Kanal ekle")) != 0; k++)
		run("up");
	expect("screen=zone shows=80/80");
	layout();
	run("ok");
	expect(T("toast=Zone_full:_max_80_channels writes=0", "toast=B\366lge_dolu:_en_\347ok_80_kanal writes=0"));
	layout();
	start("full zone: Copy here refused");
	sim_worst(true);
	run("ok 4 ctx ctx 3");
	expect(T("toast=Zone_full:_max_80_channels writes=0", "toast=B\366lge_dolu:_en_\347ok_80_kanal writes=0"));
}

/* A position whose channel can't be read shows ("17 ? Ch 77", "Can't be read") instead of vanishing */
static void case_zone_unreadable(void)
{
	start("zone list: a channel that can't be read shows");
	zone_t z;
	codeplug_zone(0, &z);
	z.channels[z.count++] = 77;	/* no channel 77 in the simulator */
	codeplug_set_zone(0, &z);
	run("ok 4 ctx");
	expect("count=19");
	for (int k = 0; k < 20 && strncmp(ui_app_focus_label(), "17 ?", 4) != 0; k++)
		run("up");
	expect(T("focus=17_?_Ch_77 shows=Can't_be_read", "focus=17_?_Ch_77 shows=Okunam"));
	layout();
}

typedef void (*case_fn)(void);

static int run_case(case_fn fn)
{
	fflush(stdout);
	pid_t pid = fork();
	if (pid == 0) {
		fails = 0;
		fn();
		fflush(stdout);
		_exit(fails ? 1 : 0);
	}
	int st = 0;
	waitpid(pid, &st, 0);
	return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : 1;
}

int main(void)
{
	static const case_fn cases[] = { case_zone_counts, case_zone_full, case_zone_unreadable };
	int bad = 0, n = 0;
	for (int lang = 0; lang < 2; lang++) {
		turkish = lang == 1;
		for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++, n++)
			bad += run_case(cases[i]);
	}
	printf("Plus r15 channels UI: %d cases (English and Turkish), %d failed\n", n, bad);
	return bad != 0;
}
