/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus round 14 APRS job through the real UI in upstream's simulator (tests/plus/ui_sim_aprsx.c wraps it), on
 * the DM-32UV's display class (240 x 320, insets 10 x 5), in English and in Turkish, every page checked for layout.
 * Each case runs in a child process (the screens' static state starts over). Covers:
 *   - item 47: Settings > APRS (the last group): the beacon format, Standard by default; Mic-E shows the message row,
 *     En route first; the messages M0-M6 round (never the emergency); the standard format again hides it;
 *   - item 49: Stations heard's OK opens the station's page (symbol by name, distance and bearing from us, position,
 *     altitude, speed and heading, comment without /A=, heard direct or through a digipeater, packets, its messages
 *     from the inbox and not another's); Up / Down step through the list; no GPS fix says so; OK opens Go-To
 *     following it (Back returns); 0 opens the map centred on it with its details (Back returns), not without a
 *     position; # writes a message (the one sent shows on the page, sending then delivered); a station that drops
 *     out of the list says so and its keys do nothing. */
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

/* --- item 49: the APRS station page --- */

static aprs_station_t *station(const char *call, double la, double lo, char code, uint32_t heard_ms)
{
	aprs_station_t *s = &ax_sim.st[ax_sim.nst++];
	memset(s, 0, sizeof *s);
	strcpy(s->call, call);
	s->has_pos = true;
	s->lat = (int32_t)(la * 1e7 + 0.5);
	s->lon = (int32_t)(lo * 1e7 + 0.5);
	s->sym_table = '/';
	s->sym_code = code;
	s->heard_ms = heard_ms;
	s->direct = true;
	s->count = 1;
	s->course = APRS_COURSE_NONE;
	s->speed_kn = APRS_SPEED_NONE;
	s->alt_m = INT16_MIN;
	return s;
}

static void inbox(const char *from, const char *text, bool read)
{
	aprs_msg_t *m = &ax_sim.msg[ax_sim.nmsg++];
	memset(m, 0, sizeof *m);
	strcpy(m->from, from);
	strcpy(m->text, text);
	m->ms = 500;
	m->read = read;
}

/* Us in Ankara; a car 2.8 km NE (altitude, speed, heading, a comment, 5 packets), a house through a digipeater;
 * messages from the car and from someone else */
static void heard_two(void)
{
	ax_sim.fix = true;
	ax_sim.lat_e7 = 399300000;
	ax_sim.lon_e7 = 328600000;
	aprs_station_t *s = station("TA1ABC-9", 39.95, 32.88, '>', 1000);
	s->alt_m = 938;
	s->speed_kn = 27;	/* 50 km/h */
	s->course = 270;
	s->count = 5;
	strcpy(s->text, "QRV 145.500 /A=003077");
	s = station("TA2XYZ-7", 40.20, 32.86, '-', 500);
	s->direct = false;
	strcpy(s->text, "Home");
	inbox("TA9OTH", "Not for this page", true);
	inbox("TA1ABC-9", "On my way", false);
	ui_push(SCR_APRS_HEARD, 0);
	ok_run("wait:100");
	expect(T("screen=aprs-heard shows=OK_details", "screen=aprs-heard"));
}

static void page_details(void)
{
	name = "I49 the station's page";
	heard_two();
	ok_run("ok wait:200");
	expect("screen=aprs-station shows=TA1ABC-9 shows=2.8_km_NE shows=39.9500_32.8800 shows=938_m shows=50_km/h_W "
	       "shows=QRV_145.500 shows=<_On_my_way hides=Not_for_this_page hides=/A=");
	expect(T("shows=Car shows=Direct,_5_packets", "shows=Ara shows=Do"));
	layout();
	ok_run("shot:station");
	/* Down: the next station in the list (a house through a digipeater, no messages) */
	ok_run("down wait:100");
	expect("screen=aprs-station shows=TA2XYZ-7 shows=Home hides=TA1ABC-9");
	expect(T("shows=House shows=Via_digi,_1_packet shows=No_messages", "shows=Ev shows=Digi_ile,_1_paket"));
	layout();
	ok_run("up wait:100");
	expect("shows=TA1ABC-9");
	/* no GPS fix: the way to it says so */
	ax_sim.fix = false;
	ok_run("wait:1200");
	expect(T("shows=No_GPS_fix", "shows=GPS_konumu_yok"));
	layout();
}

static void page_goto(void)
{
	name = "I49 Go-To from the page";
	heard_two();
	ok_run("ok wait:200 ok wait:200");
	expect("screen=goto shows=TA1ABC-9");
	ok_run("back wait:100");
	expect("screen=aprs-station shows=TA1ABC-9");
}

static void page_map(void)
{
	name = "I49 Show on map";
	heard_two();
	ok_run("ok wait:200 0 wait:300");
	expect("screen=map shows=TA1ABC-9 shows=/>_938_m_QRV");
	layout();
	ok_run("shot:map");
	ok_run("back wait:100");	/* the details off, then the pick, then the page */
	ok_run("back back wait:100");
	expect("screen=aprs-station shows=TA1ABC-9");
	/* a station without a position: no map */
	ax_sim.st[0].has_pos = false;
	ok_run("0 wait:100");
	expect(T("screen=aprs-station toast=No_position_heard", "screen=aprs-station"));
	expect(T("shows=No_position_heard", "screen=aprs-station"));
	layout();
}

static void page_message(void)
{
	name = "I49 a message from the page";
	heard_two();
	ok_run("ok wait:200 ctx wait:100");
	expect("screen=aprs-compose shows=TA1ABC-9");
	ok_run("3 wait:100 1 wait:100 4 wait:200");	/* Canned text: QSL; Send */
	check(!strcmp(ax_sim.sent_to, "TA1ABC-9") && !strcmp(ax_sim.sent_text, "QSL"), "QSL sent to TA1ABC-9");
	ok_run("back wait:200");
	expect(T("screen=aprs-station shows=>_QSL_(sending)", "screen=aprs-station shows=>_QSL"));
	ax_sim.aprs.out = APRS_OUT_DELIVERED;
	ok_run("wait:1200");
	expect(T("shows=>_QSL_(delivered)", "shows=>_QSL"));
	layout();
	/* the other station: not ours */
	ok_run("down wait:100");
	expect("hides=QSL");
}

static void page_gone(void)
{
	name = "I49 the station drops out of the list";
	heard_two();
	ok_run("ok wait:200");
	ax_sim.nst = 0;
	ok_run("wait:1200");
	expect(T("screen=aprs-station shows=Not_heard_any_more", "screen=aprs-station"));
	layout();
	ok_run("ok 0 ctx wait:100");
	expect("screen=aprs-station");
}

typedef void (*case_fn)(void);
static const case_fn all[] = {
	aprs_settings, page_details, page_goto, page_map, page_message, page_gone,
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
