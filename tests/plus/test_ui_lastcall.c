/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus Tools > Last call, the list of the last calls (core/ui/ui_lastcall.c, docs/plus.md "Last calls"), through
 * the real UI in upstream's simulator on the DM-32UV's display class (240 x 320, insets 10 x 5, key map M), in English
 * and in Turkish. Built by tools/test_xband.py with multi-watch, the call log, the P25 call replay and FM caller ID.
 * Each case runs in a child process (the screens' static state starts over). Covers:
 *   - calls of every mode reaching the list: DMR through the simulator's call (Recent's own feed), FM caller IDs (MDC,
 *     FleetSync, DTMF) through radio_fmid_logged as the radio task reports them, P25 / YSF / NXDN / dPMR as
 *     call_log_update hands them to Recent; one row a call, newest first, names from Recent, encrypted marked;
 *   - the watched channel's letter: multi-watch with six channels, a call on each of A..F; dual watch's A / B; none
 *     without a watch;
 *   - Tools > Last call opens the list; OK on the newest P25 call (the receiver holds its frames): the replay as before
 *     (not on a P25 channel: the toast; while it plays: stopped); OK on another row: no audio kept; empty list;
 *   - the layout of every page in both languages. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "sim/ui_sim.h"
#include "ui/ui_app.h"
#include "ui/ui_nav.h"
#include "ui/i18n.h"
#include "ui/lastcall.h"
#include "ui/ui_lastcall.h"
#include "radio/radio_fmid.h"
#include "p25/p25_lab.h"
#include "codeplug/codeplug.h"

/* --- The radio task's side, faked --- */

/* FM caller ID: Home's line shows none; the IDs logged one after another (call_log_update reads them each tick) */
static radio_fmid_id_t fm_log;
static uint32_t fm_seq;
void radio_fmid_shown(radio_fmid_id_t *out)
{
	memset(out, 0, sizeof *out);
}
uint32_t radio_fmid_logged(radio_fmid_id_t *out)
{
	if (out != NULL)
		*out = fm_log;
	return fm_seq;
}
void radio_fmid_enable(bool on)
{
	(void)on;
}
/* As core/radio/radio_fmid.c writes them */
void radio_fmid_text(uint8_t kind, uint16_t unit, uint16_t fleet, const char *dtmf, char *out, size_t size)
{
	if (kind == RADIO_FMID_MDC || kind == RADIO_FMID_MDC_EMERG)
		snprintf(out, size, "MDC %04X", unit);
	else if (kind == RADIO_FMID_FLEETSYNC)
		snprintf(out, size, "FleetSync %u-%u", fleet, unit);
	else if (kind == RADIO_FMID_DTMF)
		snprintf(out, size, "DTMF %s", dtmf != NULL ? dtmf : "");
	else if (size)
		out[0] = '\0';
}

/* The P25 receiver's last call (CONFIG_CALL_REPLAY) */
static uint16_t rp_frames, rp_tg;
static uint32_t rp_src;
static int rp_playing, rp_starts, rp_stops;
uint16_t p25_lab_replay_info(uint16_t *tg, uint32_t *src, uint16_t *heard_nac)
{
	if (tg)
		*tg = rp_tg;
	if (src)
		*src = rp_src;
	if (heard_nac)
		*heard_nac = 0x659;
	return rp_frames;
}
int p25_lab_replay(int start)
{
	if (!start) {
		rp_playing = 0;
		rp_stops++;
		return 1;
	}
	rp_starts++;
	rp_playing = 1;
	return 1;
}
int p25_lab_replaying(void)
{
	return rp_playing;
}

/* --- The harness --- */

static int fails;
static const char *name;
static bool turkish;
#define T(en, trk) (turkish ? (trk) : (en))

static void ok_run(const char *script)
{
	char err[300];
	if (!sim_run(script, err, sizeof err)) {
		printf("FAIL %s (%s): %s: %s\n", name, turkish ? "tr" : "en", script, err);
		fails++;
	}
}

static void expect(const char *e)
{
	char buf[500], *o = buf;
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

static void start(const char *n)
{
	name = n;
	sim_reset(sim_geom_find("240x320i10x5"), SIM_MAP_M);
	const char *shots = getenv("LASTCALL_SHOTS");	/* a directory: the pages as PNG files */
	if (shots != NULL)
		sim_set_shot_dir(shots, turkish ? "tr-" : "en-");
	if (turkish && !lang_set(lang_find('t' << 8 | 'r'))) {
		printf("FAIL %s: no Turkish in this build\n", name);
		fails++;
	}
	ok_run("wait:100");
}

/* A call as call_log_update hands it to Recent (P25, YSF, NXDN, dPMR: their feeds are the radio task's) */
static void feed(uint8_t mode, uint32_t src, uint32_t dst, bool group, uint16_t nac, bool enc, const char *callsign)
{
	ui_recent_t r;
	memset(&r, 0, sizeof r);
	r.mode = mode;
	r.src = src;
	r.dst = dst;
	r.group = group;
	r.nac = nac;
	r.enc = enc;
	if (callsign != NULL)
		strcpy(r.name, callsign);
	ui_lastcall_view(NULL);
	ui_lastcall_recent(&r, true);
}

static void fm_id(uint8_t kind, uint16_t unit, uint16_t fleet, const char *dtmf)
{
	memset(&fm_log, 0, sizeof fm_log);
	fm_log.kind = kind;
	fm_log.unit = unit;
	fm_log.fleet = fleet;
	if (dtmf != NULL)
		strcpy(fm_log.dtmf, dtmf);
	fm_seq++;
	ok_run("wait:300");
}

/* Tools > Last call, found by its (translated) name */
static void open_tools_entry(void)
{
	ui_push(SCR_TOOLS, 0);
	ok_run("wait:100");
	const char *want = tr("Last call");
	for (int i = 0; i < 40 && strcmp(ui_app_focus_label(), want) != 0; i++)
		ok_run("down");
	check(strcmp(ui_app_focus_label(), want) == 0, "Tools has Last call");
	ok_run("ok wait:200");
	expect("screen=lastcall");
}

/* --- The cases --- */

static void every_mode(void)
{
	start("every mode");
	ok_run("call:3101234,2862,g wait:500");				/* DMR, through Recent's own feed */
	fm_id(RADIO_FMID_MDC, 0x1A2B, 0, NULL);
	fm_id(RADIO_FMID_FLEETSYNC, 1772, 107, NULL);
	fm_id(RADIO_FMID_DTMF, 0, 0, "1234#");
	feed(RADIO_MODE_P25, 77285, 101, true, 0x659, false, NULL);
	feed(RADIO_MODE_YSF, 0, 0, false, 0, false, "TA2DOU");
	feed(RADIO_MODE_NXDN, 1234, 5, true, 3, false, NULL);
	feed(RADIO_MODE_DPMR, 4321, 99, false, 2, true, NULL);
	const lastcall_list_t *l = ui_lastcall_list();
	check(l->n == 8, "eight calls kept");
	static const uint8_t modes[] = { RADIO_MODE_DPMR, RADIO_MODE_NXDN, RADIO_MODE_YSF, RADIO_MODE_P25, RADIO_FMID_DTMF,
					 RADIO_FMID_FLEETSYNC, RADIO_FMID_MDC, RADIO_MODE_DMR };
	for (unsigned i = 0; i < l->n && i < 8; i++)
		check(l->e[i].mode == modes[i], "newest first, every mode");
	check(l->e[7].id == 3101234 && l->e[7].tg == 2862 && l->e[7].letter == 0, "the DMR call, no watch");
	check((l->e[0].flags & (LASTCALL_ENC | LASTCALL_PRIV)) == (LASTCALL_ENC | LASTCALL_PRIV), "dPMR: private, encrypted");
	check((l->e[2].flags & LASTCALL_TEXT) && l->e[6].id == 0x1A2B && l->e[5].tg == 107, "YSF's callsign, MDC, FleetSync");
	/* the oldest goes with one more */
	feed(RADIO_MODE_YSF, 0, 0, false, 0, false, "TA1XYZ");
	check(l->n == 8 && l->e[7].mode == RADIO_FMID_MDC, "the oldest dropped");
	open_tools_entry();
	expect("shows=YSF_TA1XYZ shows=dPMR_4321 shows=NXDN_1234 shows=TG5 shows=YSF_TA2DOU shows=P25_77285 "
	       "shows=TG101");
	expect(T("shows=Encrypted shows=Last_calls", "shows=Son"));
	layout();
	ok_run("shot:lastcall-modes");
	/* the oldest rows below */
	ok_run("down down down down down down down wait:100");
	expect("shows=FM_DTMF_1234# shows=FM_FleetSync_107-1772 shows=FM_MDC_1A2B");
	layout();
}

static void dmr_names_and_ages(void)
{
	start("DMR names and ages");
	ok_run("call:3101234,91,g wait:6000 call:3101234,91,g call:3102222,9,p wait:1000");
	const lastcall_list_t *l = ui_lastcall_list();
	check(l->n == 3 && l->e[0].id == 3102222 && (l->e[0].flags & LASTCALL_PRIV) && l->e[1].id == 3101234 &&
	      l->e[2].id == 3101234, "a call a row, the same station twice after a pause");
	ui_push(SCR_LASTCALL, 0);
	ok_run("wait:200");
	expect("shows=TG91 shows=DMR");
	layout();
	ok_run("wait:120000");
	expect("shows=2m");
	ok_run("ok wait:100");
	expect(T("toast=No_audio", "toast=Bu"));
}

static void letters_ring(void)
{
	start("letters A-F");
	ok_run("mwatch:6 quick 7 wait:200");
	static const char *const tune[] = { "tune:a", "tune:b", "tune:c", "tune:d", "tune:e", "tune:f" };
	for (unsigned k = 0; k < 6; k++) {
		char s[64];
		snprintf(s, sizeof s, "%s call:%u,91,g wait:500", tune[k], 3101000u + k);
		ok_run(s);
	}
	const lastcall_list_t *l = ui_lastcall_list();
	check(l->n == 6, "six calls");
	for (unsigned k = 0; k < 6 && k < l->n; k++)
		check(l->e[5 - k].letter == 'A' + k && l->e[5 - k].id == 3101000u + k, "each call with its letter");
	ui_push(SCR_LASTCALL, 0);
	ok_run("wait:200");
	expect("shows=F_DMR_3101005 shows=E_DMR shows=D_DMR shows=C_DMR shows=B_DMR shows=A_DMR_3101000");
	layout();
	ok_run("shot:lastcall-letters");
}

static void letters_dual(void)
{
	start("letters A / B (dual watch)");
	ok_run("mwatch:2 quick 7 wait:200 tune:b call:3107002,91,g wait:500 tune:a call:3107001,91,g wait:500");
	const lastcall_list_t *l = ui_lastcall_list();
	check(l->n == 2 && l->e[0].letter == 'A' && l->e[1].letter == 'B', "dual watch's panes");
	ui_push(SCR_LASTCALL, 0);
	ok_run("wait:200");
	expect("shows=A_DMR_3107001 shows=B_DMR_3107002");
	layout();
}

static void replay(void)
{
	start("P25 replay");
	rp_frames = 400;
	rp_tg = 101;
	rp_src = 77285;
	feed(RADIO_MODE_P25, 77285, 101, true, 0x659, false, NULL);
	ok_run("call:3101234,91,g wait:500");
	check(ui_lastcall_playable() == 1, "the newest P25 call can play");
	ui_push(SCR_LASTCALL, 0);
	ok_run("wait:200");
	expect("shows=>_TG101");
	layout();
	/* OK on the DMR call: no audio kept */
	ok_run("ok wait:100");
	expect(T("toast=No_audio_kept", "toast=Bu"));
	/* OK on the P25 call, here on an FM channel: the replay's own refusal; nothing started */
	ok_run("down ok wait:100");
	expect(T("toast=Last_call:_go_to_a_P25", "toast=Son_"));
	check(rp_starts == 0, "not on a P25 channel: no replay");
	/* while one plays: OK stops it */
	rp_playing = 1;
	ok_run("ok wait:100");
	expect(T("toast=Replay_stopped", "toast=Tekrar_durdu"));
	check(rp_stops == 1 && rp_playing == 0, "stopped");
	/* another P25 call since: the frames are the older one's, nothing plays */
	feed(RADIO_MODE_P25, 5555, 102, true, 0x659, false, NULL);
	check(ui_lastcall_playable() == -1, "only the newest P25 call, and only its own frames");
	/* an encrypted P25 call never plays */
	rp_tg = 102;
	rp_src = 5555;
	feed(RADIO_MODE_P25, 5555, 102, true, 0x659, true, NULL);
	check(ui_lastcall_playable() == -1, "encrypted: no replay");
}

static void empty(void)
{
	start("empty");
	open_tools_entry();
	expect(T("shows=No_calls_heard_yet", "shows=Hen"));
	layout();
	ok_run("back wait:100");
	expect("screen=tools");
	/* power off and on: gone */
	feed(RADIO_MODE_P25, 77285, 101, true, 0x659, false, NULL);
	check(ui_lastcall_list()->n == 1, "one call");
	ok_run("reboot wait:200");
	check(ui_lastcall_list()->n == 0, "RAM only");
}

typedef void (*case_fn)(void);

int main(void)
{
	static const case_fn all[] = { every_mode, dmr_names_and_ages, letters_ring, letters_dual, replay, empty };
	int bad = 0, run = 0;
	for (int lang = 0; lang < 2; lang++) {
		for (unsigned i = 0; i < sizeof all / sizeof all[0]; i++) {
			fflush(stdout);
			pid_t pid = fork();
			if (pid == 0) {
				turkish = lang == 1;
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
					printf("FAIL case %u (%s): crashed\n", i, lang ? "tr" : "en");
			}
		}
	}
	printf("Last calls screens: %d flows (English and Turkish), %d failed\n", run, bad);
	return bad != 0;
}
