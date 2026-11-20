/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus round 3 "misc" screens through the real UI in upstream's simulator (tests/plus/ui_sim_misc.c wraps it: the
 * radio requests recorded, a radio that receives AM, the test's position and battery voltage), on the DM-32UV's display
 * class (240 x 320, insets 10 x 5) with its key map. Built like tests/classic/test_ui.c with -DCONFIG_AM_RX=1
 * -DCONFIG_AIRFIELDS=1 -DCONFIG_MARINE=1 -DCONFIG_QSO_LOG=1 -DCONFIG_BATT_GRAPH=1 and this job's
 * files. Each case runs in a child process (the screens' static state starts over). Covers:
 *   - Tools > Airfields: nearest first from the position (and by code without one), OK the details, OK tunes the VFO
 *     exactly as Tools > Air band does (vfo on, vfo_set; AM and no TX follow from the band), an 8.33 kHz channel name
 *     to its grid point, # the nearest, an airfield without frequencies, refused while transmitting;
 *   - Tools > Marine VHF: opens on 16; OK: VFO on the coast frequency, FM, 25 kHz, no tones, simplex (the request
 *     sequence), a duplex channel's coast frequency, 70 (DSC) and AIS refused, # back to 16, Right ten on, the wrap;
 *   - Recent: * marks a QSO and unmarks it (not the quick settings), the mark in the debug link's export;
 *   - Tools > Battery: the page, "collecting", the graph after samples, the time left on a falling voltage;
 *   - the layout check on each new screen. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "sim/ui_sim.h"
#include "ui/ui_app.h"
#include "ui/qso_log.h"
#include "ui/ui_svc.h"
#include "ui_sim_misc.h"

static int fails;
static const char *name;

static bool run(const char *script)
{
	char err[300];
	if (!sim_run(script, err, sizeof err)) {
		printf("FAIL %s: %s: %s\n", name, script, err);
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

static void check(bool c, const char *what)
{
	if (!c) {
		printf("FAIL %s: %s\n", name, what);
		fails++;
	}
}

static void requests(const char *want)
{
	if (strcmp(misc_sim.req, want) != 0) {
		printf("FAIL %s: requests \"%s\", expected \"%s\"\n", name, misc_sim.req, want);
		fails++;
	}
	misc_sim.req[0] = 0;
}

static void layout(void)
{
	char issues[SIM_LAYOUT_MAX][160];
	run("wait:200");
	int n = sim_layout_issues(issues, SIM_LAYOUT_MAX);
	for (int i = 0; i < n; i++)
		printf("FAIL %s: layout: %s\n", name, issues[i]);
	fails += n;
}

static void pos(double la, double lo)
{
	misc_sim.pos = true;
	misc_sim.lat_e7 = (int32_t)(la * 1e7 + 0.5);
	misc_sim.lon_e7 = (int32_t)(lo * 1e7 + 0.5);
}

/* Tools, then down to the row called label, then OK */
static void tool(const char *label)
{
	run("ok 6");
	for (int k = 0; k < 40 && strcmp(ui_app_focus_label(), label) != 0; k++)
		run("down");
	check(strcmp(ui_app_focus_label(), label) == 0, label);
	run("ok");
	misc_sim.req[0] = 0;
}

static void start(const char *n)
{
	name = n;
	sim_reset(sim_geom_find("240x320i10x5"), SIM_MAP_K);
	const char *shots = getenv("MISC_SHOTS");	/* a directory: the "shot:" tokens save the screens there */
	if (shots != NULL)
		sim_set_shot_dir(shots, "misc-");
	misc_sim_setup();
	pos(39.9208, 32.8541);		/* Ankara, Kizilay */
	misc_sim_battery_mv(7900);
	run("wait:200");
}

typedef void (*case_fn)(void);

static int run_case(const char *n, case_fn fn)
{
	fflush(stdout);
	pid_t pid = fork();
	if (pid == 0) {
		start(n);
		fn();
		fflush(stdout);
		_exit(fails != 0);
	}
	int st = 0;
	waitpid(pid, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
		if (!WIFEXITED(st))
			printf("FAIL %s: crashed\n", n);
		return 1;
	}
	return 0;
}

/* --- Tools > Airfields --- */

static void air_list(void)
{
	tool("Airfields");
	expect("screen=airfields shows=Nearest_first shows=LTAB_G shows=LTAD_Etimesgut_AB shows=9.8_km_W");
	run("shot:airfields");
	layout();
	run("ok");
	expect("screen=airfields shows=ATIS_118.675 shows=TWR_132.000 shows=AM,_RX_only shows=Not_for_navigation");
	run("shot:airfield");
	layout();
	run("ok");
	expect("screen=home vfo=1 freq=118675000 toast=LTAB_ATIS_118.675:_AM,_RX_only");
	requests("vfo:1 vfo_set:118675000");
}

static void air_pick_tune(void)
{
	tool("Airfields");
	run("down down ok down down");	/* LTAB, LTAD, LTAC Esenboga: ATIS, GND, TWR 118.100 */
	expect("shows=TWR_118.100");
	run("ok");
	expect("screen=home vfo=1 freq=118100000 toast=LTAC_TWR_118.100");
	requests("vfo:1 vfo_set:118100000");
}

static void air_833(void)
{
	pos(40.8986, 29.3092);		/* Sabiha Gokcen */
	tool("Airfields");
	expect("shows=LTFJ");
	run("ctx");			/* # the nearest's details */
	expect("shows=LTFJ shows=8.33 shows=ATIS_128.200");
	run("down down down");		/* ATIS, ATIS, DEL, GND 121.580 */
	expect("shows=GND_121.580");
	run("ok");
	expect("screen=home freq=121575000 toast=LTFJ_GND_121.580");
	requests("vfo:1 vfo_set:121575000");
}

static void air_no_position(void)
{
	misc_sim.pos = false;
	tool("Airfields");
	expect("shows=No_position:_by_code shows=LTAB hides=km");
	run("up");			/* round to the last code */
	run("ok");
	expect("shows=TR-0040");
	layout();
}

static void air_no_freq(void)
{
	pos(41.3142, 33.7958);		/* Kastamonu */
	tool("Airfields");
	run("ok");
	expect("shows=LTAL shows=No_frequencies_in_the_data");
	run("ok");
	expect("screen=airfields toast=No_frequencies_in_the_data");
	requests("");
}

static void air_tx(void)
{
	tool("Airfields");
	run("ok");
	misc_sim_state(RS_TX);
	run("ok");
	expect("screen=airfields toast=Can't_tune_while_transmitting");
	misc_sim_state(RS_IDLE);
	requests("");
	run("back");
	expect("screen=airfields shows=Nearest_first");
}

/* --- Tools > Marine VHF --- */

static void marine_16(void)
{
	tool("Marine VHF");
	expect("screen=marine shows=Distress,_calling shows=156.800 shows=Ship/coast_MHz,_RX_only");
	run("shot:marine");
	layout();
	run("ok");
	expect("screen=home vfo=1 freq=156800000 txfreq=156800000 mode=fm toast=Ch_16_156.800:_RX_only");
	requests("vfo_set:156800000 mode_fm:1 bw:25 tone_tx:0 tone_rx:0 vfo_tx:0");
}

static void marine_duplex(void)
{
	tool("Marine VHF");
	run("down down down down down down");	/* 16 -> 22 */
	expect("shows=157.100_/_161.700 shows=Public_corr.");
	run("shot:marine-duplex");
	layout();		/* the duplex pairs beside the channel numbers (18 and on overlapped them) */
	run("ok");
	expect("screen=home freq=161700000 toast=Ch_22_161.700");
	requests("vfo_set:161700000 mode_fm:1 bw:25 tone_tx:0 tone_rx:0 vfo_tx:0");
}

static void marine_data(void)
{
	tool("Marine VHF");
	run("down down down down down down down down down down down down down down down down down down down down down down down");	/* 16 -> 70 (the list: 01..28, 60..88, 87B, 88B) */
	expect("shows=DSC_data,_no_voice shows=156.525");
	run("ok");
	expect("screen=marine toast=Ch_70_is_DSC_data");
	requests("");
	run("ctx");
	expect("shows=Distress");	/* # back to 16 */
	run("up up up up up up up up up up up up up up up up");		/* 16 -> 01 -> round to 88B */
	expect("shows=AIS_data shows=162.025");
	run("ok");
	expect("toast=AIS_is_data:_not_tuned");
	requests("");
}

/* --- Recent: the QSO mark --- */

static void qso_star(void)
{
	run("call:3101234,91,g wait:3000 ok 3");
	expect("screen=recent shows=*_QSO");
	run("quick");
	expect("screen=recent toast=Marked_as_QSO");
	check(qso_log_marked(0) && qso_log_user(0), "the row is marked");
	run("shot:recent");
	uint8_t out[1024];
	size_t len = qso_log_export(out, sizeof out, sim_now());
	check(len == QSO_EXPORT_HEAD + QSO_EXPORT_ENTRY && out[1] == 1, "one entry exported");
	check(len > QSO_EXPORT_HEAD && (out[QSO_EXPORT_HEAD + 5] & QSO_F_QSO) != 0 &&
	      out[QSO_EXPORT_HEAD + 8] == (3101234 & 0xFF), "exported with its mark");
	run("quick");
	expect("screen=recent toast=QSO_mark_removed");
	check(!qso_log_marked(0), "the mark is gone");
	run("back");
	run("quick");			/* elsewhere * is the quick settings as before */
	expect("screen=quick");
}

/* --- Tools > Battery --- */

static void battery_graph(void)
{
	tool("Battery");
	expect("screen=battery shows=84% shows=7.90_V shows=Time_left:_-- shows=Collecting shows=First_point_in_2_min");
	layout();
	run("wait:400000");		/* three samples */
	expect("hides=First_point shows=Time_left:_-- shows=Collecting shows=now");
	/* 300 mV an hour down for 40 minutes: 7.70 V, (7.70 - 6.40) / 0.3 = 4 h 20 min */
	for (int k = 1; k <= 40; k++) {
		misc_sim_battery_mv((uint16_t)(7900 - 5 * k));
		run("wait:60000");
	}
	expect("shows=Time_left:_4_h");
	run("shot:battery");
	layout();
	/* Charging: rising again */
	for (int k = 1; k <= 6; k++) {
		misc_sim_battery_mv((uint16_t)(7700 + 30 * k));
		run("wait:60000");
	}
	expect("shows=Time_left:_-- shows=Charging");
}

/* --- Settings > Display > Large text: Home with the channel and the talk target only, large --- */

static void large_text_home(void)
{
	run("shot:home");
	ui_svc_setting_set(UI_SET_LARGE_TEXT, 1);
	run("reboot wait:200");
	expect("screen=home");
	run("shot:home-large");
	layout();
}

int main(void)
{
	static const struct { const char *name; case_fn fn; } cases[] = {
		{ "AF list and listen", air_list },
		{ "AF pick and tune", air_pick_tune },
		{ "AF 8.33 kHz name", air_833 },
		{ "AF no position", air_no_position },
		{ "AF without frequencies", air_no_freq },
		{ "AF refused while transmitting", air_tx },
		{ "MV channel 16", marine_16 },
		{ "MV duplex channel", marine_duplex },
		{ "MV data channels", marine_data },
		{ "QSO star on Recent", qso_star },
		{ "BG battery graph", battery_graph },
		{ "LT large text Home", large_text_home },
	};
	int failed = 0;
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
		failed += run_case(cases[i].name, cases[i].fn);
	printf("Plus misc UI: %zu flows, %d failures\n", sizeof cases / sizeof cases[0], failed);
	return failed != 0;
}
