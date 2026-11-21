/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus round 14 air items (docs/plus.md "Round 14: air band and spectrum") through the real UI in upstream's
 * simulator (tests/plus/ui_sim_air.c wraps it: a radio that receives AM, the test's position), on the DM-32UV's display
 * class (240 x 320, insets 10 x 5), in English and in Turkish. Each case runs in a child process (the screens' static
 * state starts over). Covers:
 *   - item 44a, Tools > Airfields after the table's cleanup: "+N" on the list line when the service shown has more
 *     frequencies (Antalya's two towers), none with one; an airfield's every frequency in its details, the list
 *     scrolling to the last one and OK tuning it; every page fits;
 *   - item 44b, air watch (CONFIG_AIR_WATCH, ui_airwatch.inc; the radio core faked in ui_sim_air.c): # on an
 *     airfield's page puts its first six frequencies on the multi-watch ring, A..F each its own VFO; Back twice, or
 *     # 5 Stop, brings the radio and the user's watch back as they were (off, or the user's ring with its selection
 *     and channels), nothing saved meanwhile; two frequencies make dual watch's pair, one is tuned as OK does (a
 *     channel name on two services counts once); refused while transmitting and without frequencies; a power off
 *     during it doesn't bring it back. */
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

/* --- Item 44b: air watch --- */

#if CONFIG_AIR_WATCH
static void check(bool c, const char *what)
{
	if (!c) {
		printf("FAIL %s (%s): %s\n", name, turkish ? "tr" : "en", what);
		fails++;
	}
}

/* Antalya: GND 121.900, TWR 125.000, TWR 126.100, APP 119.650, APP 124.350, APP 124.425 (its ATIS 118.275 left out:
 * the recording never stops) */
static const uint32_t lti_hz[6] = { 121900000, 125000000, 126100000, 119650000, 124350000, 124425000 };

static void antalya_watch(void)
{
	pos(36.8969, 30.7133);
	airfields();
	run("ok");
	run("ctx");
}

/* # on Antalya's page: its six (not the ATIS) on the ring, A..F each its own VFO, received there; Home shows it */
static void aw_watch(void)
{
	start("AW watch all");
	pos(36.8969, 30.7133);
	airfields();
	run("ok");
	expect(T("shows=#_watch_all", "shows=#_t"));
	layout();
	run("ctx");
	expect(T("screen=home dual=1 mwn=6 mwradion=6 mwsel=A toast=LTAI:_Air_watch_on",
		 "screen=home dual=1 mwn=6 mwradion=6 mwsel=A toast=LTAI:_Hava_izleme_a"));
	check(air_sim.vfo_calls == 6, "six VFOs asked for");
	for (uint8_t k = 0; k < 6; k++)
		check(air_sim.hz[k] == lti_hz[k] && air_sim_vfo_hz(k) == lti_hz[k], "each letter's VFO on its frequency");
	expect("vfo=1 vfoidx=0 freq=121900000 shows=121.90 shows=AM_124.425 hides=FM_124 hides=118.27");
	run("tune:e");
	expect("vfo=1 vfoidx=4 freq=124350000");
	run("shot:home-air");
	layout();
}

/* Back twice stops it: the radio back on the channel it was on, the watch off, nothing saved meanwhile */
static void aw_stop(void)
{
	start("AW stop: back as before");
	uint16_t pos0 = ui_view()->zone_pos;
	bool vfo0 = ui_view()->vfo;
	int32_t b0 = ui_svc_setting_get(UI_SET_DUAL_B), p0 = ui_svc_setting_get(UI_SET_MW_P1);
	antalya_watch();
	expect("dual=1 mwn=6");
	run("abkey abkey");	/* C selected: a change the user's watch never had */
	expect("mwsel=C");
	check(ui_svc_setting_get(UI_SET_DUAL_B) == b0 && ui_svc_setting_get(UI_SET_MW_P1) == p0, "nothing saved");
	run("back back wait:300");
	expect(T("dual=0 toast=Air_watch_off", "dual=0 toast=Hava_izleme_kapal"));
	check(air_sim.end_calls == 1, "the end asked for once");
	check(ui_view()->zone_pos == pos0 && ui_view()->vfo == vfo0, "the radio back where it was");
	check(ui_svc_setting_get(UI_SET_DUAL_B) == b0 && ui_svc_setting_get(UI_SET_MW_P1) == p0, "the settings as before");
	layout();
}

/* The user's ring of four, C selected: the air watch takes its place; # 5 Stop: the user's ring again as it was */
static void aw_user_ring(void)
{
	start("AW the user's ring kept");
	run("mwatch:4 quick 7 abkey abkey");
	expect("dual=1 mwn=4 mwsel=C mwposb=4 mwposc=5 mwposd=6");
	ui_go_home();
	antalya_watch();
	expect("dual=1 mwn=6 mwradion=6 mwsel=A");
	run("ctx 5 wait:300");
	expect("dual=1 mwn=4 mwradion=4 mwsel=C mwposb=4 mwposc=5 mwposd=6");
	expect(T("toast=Air_watch_off", "toast=Hava_izleme_kapal"));
	layout();
}

/* Two frequencies (Guvercinlik: GND 120.200, TWR 132.000): dual watch's pair of two VFOs */
static void aw_pair(void)
{
	start("AW two frequencies: the pair");
	airfields();
	run("ok ctx");
	expect("screen=home dual=1 mwn=0 mwradion=2 vfo=1 freq=120200000");
	check(air_sim.vfo_calls == 2 && air_sim.hz[1] == 132000000, "two VFOs");
	run("back back wait:300");
	expect("dual=0");
	check(air_sim.end_calls == 1, "ended");
}

/* One frequency (Etimesgut: TWR 122.100): tuned as OK does; one channel name on two services (Afyon: TWR and APP
 * 122.100) is one frequency too */
static void aw_one(void)
{
	start("AW one frequency: tuned");
	airfields();
	run("down ok ctx");
	expect("screen=home dual=0 vfo=1 freq=122100000");
	check(air_sim.vfo_calls == 0, "no ring");
	pos(38.7264, 30.6011);	/* Afyon */
	airfields();
	expect("shows=LTAH");
	run("ok ctx");
	expect("screen=home dual=0 freq=122100000");
	check(air_sim.vfo_calls == 0, "no ring for one channel name");
}

/* Refused while transmitting; an airfield without frequencies */
static void aw_refused(void)
{
	start("AW refused");
	pos(36.8969, 30.7133);
	airfields();
	run("ok");
	air_sim_state(RS_TX);
	run("ctx");
	expect(T("screen=airfields toast=Can't_tune_while_transmitting", "screen=airfields"));
	air_sim_state(RS_IDLE);
	check(air_sim.vfo_calls == 0, "nothing asked for");
	pos(41.3142, 33.7958);	/* Kastamonu: none in the data */
	run("back back");
	airfields();
	run("ok ctx");
	expect("screen=airfields dual=0");
	check(air_sim.vfo_calls == 0, "nothing asked for");
}

/* A power off during it: the watch doesn't come back (nothing of it was saved) */
static void aw_reboot(void)
{
	start("AW power off during it");
	int32_t b0 = ui_svc_setting_get(UI_SET_DUAL_B);
	antalya_watch();
	expect("dual=1 mwn=6");
	run("reboot wait:500");
	expect("dual=0");
	check(ui_svc_setting_get(UI_SET_DUAL_B) == b0, "the settings as before");
}
#endif

typedef void (*case_fn)(void);
static const case_fn all[] = {
	af_more, af_one, af_none,
#if CONFIG_AIR_WATCH
	aw_watch, aw_stop, aw_user_ring, aw_pair, aw_one, aw_refused, aw_reboot,
#endif
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
	printf("Plus air UI: %d cases (English and Turkish), %d failed\n", cases, bad);
	return bad != 0;
}
