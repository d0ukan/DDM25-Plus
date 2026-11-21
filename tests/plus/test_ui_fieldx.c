/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus round 13 ideas 1 through the real UI in upstream's simulator (tests/plus/ui_sim_fieldx.c wraps it), on
 * the DM-32UV's display class (240 x 320, insets 10 x 5). Built with -DCONFIG_FIND_SAVE=1 -DCONFIG_NAV_FOLLOW=1
 * -DCONFIG_LOCATOR=1 -DCONFIG_SOS_KEY=1 and the screens they touch (P25 / DMR search, Go-To, map, APRS composer).
 * Each case runs in a child process (the screens' static state starts over). Covers:
 *   - item 23: # on P25 search saves every find into a new zone "Bulunanlar 1" through the zone editor's path (the
 *     channels as findsave.h says: names, NAC, Normal squelch, TX only with a talkgroup and our ID), the toast with
 *     the count; again: "Bulunanlar 2"; Save this one (OK on a picked find) into the newest such zone, a duplicate
 *     skipped ("Already in"), a duplicate within one save counted; DMR search's # and OK held (CC, slot, RX group
 *     Any, RX only) while OK still tunes; a full zone, a full codeplug and no room for a zone end it gracefully;
 *   - item 28: Stations heard's OK and the map's 5 follow a station in Go-To: its name, age, a new position live, stale
 *     from 10 minutes, Back / Up end it, Tools > Go-To opened again is not following;
 *   - item 30: Go-To's # opens the locator page; # types a locator with the keypad letters; centre, distance and
 *     bearing, our own locator; OK saves the waypoint; a bad locator is refused;
 *   - item 33: the SOS key's actions (rows, on / off), the SOS key's page with nothing chosen (nothing sent), Back cancels
 *     (nothing sent), OK and the key held confirm, the toasts, the key again offers Stop, the key works with the keys
 *     locked; nothing is asked to transmit before the confirmation. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "sim/ui_sim.h"
#include "ui/ui_app.h"
#include "ui/ui_nav.h"
#include "ui/nav.h"
#include "ui/rpt.h"
#include "ui/findsave.h"
#include "ui/follow.h"
#include "ui/sos.h"
#include "codeplug/codeplug.h"
#include "ui_sim_fieldx.h"

/* Tools > Repeaters' table (the map draws it) */
#define P(la, lo) (int32_t)((la) * 1e5 + 0.5), (int32_t)((lo) * 1e5 + 0.5)
const rpt_entry_t rpt_table[] = {
	{ P(39.93, 32.86), 145600000, -480, 885, RPT_FM, 0, "TA2RAK", "Ankara", 6 },
};
const rpt_info_t rpt_info = { sizeof rpt_table / sizeof rpt_table[0], 0, "2026-11-20", "test" };

static int fails;
static const char *name;

static void ok_run(const char *script)
{
	char err[300];
	if (!sim_run(script, err, sizeof err)) {
		printf("FAIL %s: %s: %s\n", name, script, err);
		fails++;
		if (getenv("FX_DUMP"))
			fx_sim_dump();
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

static void key(uint8_t action, uint8_t gesture)
{
	input_event_t ev = { action, gesture, 0 };
	ui_app_key(&ev);
	ok_run("wait:100");
}

static int zone_named(const char *n, zone_t *z)
{
	for (uint16_t i = 0; i < codeplug_zone_count(); i++)
		if (codeplug_zone(i, z) && strcmp(z->name, n) == 0)
			return i;
	return -1;
}

static bool zone_channel(const zone_t *z, uint16_t pos, channel_t *ch)
{
	return pos < z->count && codeplug_channel(z->channels[pos], ch);
}

static void p25_find(uint32_t hz, uint16_t nac, uint16_t tg, uint32_t src)
{
	p25s_view_t *v = &fx_sim.p25;
	memmove(&v->found[1], &v->found[0], sizeof v->found[0] * (P25S_FOUND_MAX - 1u));
	memset(&v->found[0], 0, sizeof v->found[0]);
	v->found[0].hz = hz;
	v->found[0].nac = nac;
	v->found[0].tg = tg;
	v->found[0].src = src;
	v->found[0].heard_ms = 100;
	if (v->found_n < P25S_FOUND_MAX)
		v->found_n++;
}

static void dmr_find(uint32_t hz, uint16_t cc, uint8_t ts, uint8_t sync)
{
	dmrs_view_t *v = &fx_sim.dmr;
	memmove(&v->found[1], &v->found[0], sizeof v->found[0] * (DMRS_FOUND_MAX - 1u));
	memset(&v->found[0], 0, sizeof v->found[0]);
	v->found[0].hz = hz;
	v->found[0].cc = cc;
	v->found[0].ts_mask = ts;
	v->found[0].sync_mask = sync;
	v->found[0].heard_ms = 100;
	if (v->found_n < DMRS_FOUND_MAX)
		v->found_n++;
}

static void gps(bool fix, double la, double lo)
{
	fx_sim.gps = true;
	fx_sim.fix = fix;
	fx_sim.lat_e7 = (int32_t)(la * 1e7 + (la < 0 ? -0.5 : 0.5));
	fx_sim.lon_e7 = (int32_t)(lo * 1e7 + (lo < 0 ? -0.5 : 0.5));
}

static void station(const char *call, double la, double lo, uint32_t heard_ms)
{
	aprs_station_t *s = &fx_sim.st[fx_sim.nst++];
	memset(s, 0, sizeof *s);
	strcpy(s->call, call);
	s->has_pos = true;
	s->lat = (int32_t)(la * 1e7 + 0.5);
	s->lon = (int32_t)(lo * 1e7 + 0.5);
	s->sym_table = '/';
	s->sym_code = '>';
	s->heard_ms = heard_ms;
	s->direct = true;
	s->alt_m = INT16_MIN;
}

static void start(const char *n)
{
	name = n;
	sim_reset(sim_geom_find("240x320i10x5"), SIM_MAP_K);
	fx_sim_clear();
	gps(true, 39.93, 32.86);
	fx_sim.stations = true;
	station("TA2XYZ-7", 40.20, 32.86, 500);
	station("TA1ABC-9", 39.95, 32.88, 1000);
}

/* --- item 23: P25 / DMR search finds as channels --- */

static void p25_three(void)
{
	p25_find(145600000, 0x293, 101, 3101234);	/* oldest */
	p25_find(433525000, 0x659, 0, 0);
	p25_find(433500000, 0x1C8, 655, 77285);	/* newest */
}

static void fs_save_all(void)
{
	p25_three();
	ui_push(SCR_P25SEARCH, 0);
	ok_run("wait:200");
	expect("screen=p25search shows=#_save_all");
	uint16_t zones = codeplug_zone_count();
	ok_run("ctx wait:600");
	expect("screen=p25search toast=Bulunanlar_1:_3_saved");
	zone_t z;
	channel_t ch;
	int zi = zone_named("Bulunanlar 1", &z);
	check(zi >= 0 && codeplug_zone_count() == zones + 1u && z.count == 3, "a new zone with the three");
	/* in the list's order (newest first); names, NAC, Normal squelch, the TX rule */
	check(zone_channel(&z, 0, &ch) && !strcmp(ch.name, "P25 433.500") && ch.mode == RADIO_MODE_P25 &&
	      ch.p25_nac == 0x1C8 && ch.p25_squelch == 0 && ch.p25_tg == 655 && !ch.rx_only && ch.p25_id != 0,
	      "433.500: NAC $1C8, TG 655 heard, our ID: may transmit");
	check(zone_channel(&z, 1, &ch) && !strcmp(ch.name, "P25 433.525") && ch.p25_nac == 0x659 && ch.p25_tg == 0 &&
	      ch.rx_only, "433.525: no talkgroup heard: RX only");
	check(zone_channel(&z, 2, &ch) && !strcmp(ch.name, "P25 145.600") && ch.p25_nac == 0x293 && ch.p25_tg == 101,
	      "145.600");
	check(!ui_findsave_busy(), "done");
	/* again: a new zone, one number on */
	ok_run("ctx wait:600");
	expect("toast=Bulunanlar_2:_3_saved");
	check(zone_named("Bulunanlar 2", &z) >= 0 && z.count == 3 && codeplug_zone_count() == zones + 2u, "Bulunanlar 2");
	expect("tx=0");
}

static void fs_save_one(void)
{
	p25_three();
	ui_push(SCR_P25SEARCH, 0);
	ok_run("wait:200");
	/* no find zone yet: Save this one makes Bulunanlar 1 */
	ok_run("down down");
	expect("shows=OK_save");
	ok_run("ok wait:600");
	expect("toast=Saved_to_Bulunanlar_1");
	zone_t z;
	channel_t ch;
	check(zone_named("Bulunanlar 1", &z) >= 0 && z.count == 1 && zone_channel(&z, 0, &ch) &&
	      !strcmp(ch.name, "P25 433.525"), "the picked one (the second row)");
	/* the same again: already there, nothing written */
	ok_run("ok wait:600");
	expect("toast=Already_in_Bulunanlar_1");
	check(zone_named("Bulunanlar 1", &z) >= 0 && z.count == 1, "not twice");
	/* OK held on another: saved too, into the same (newest) zone */
	ok_run("up long:ok wait:600");
	expect("toast=Saved_to_Bulunanlar_1");
	check(zone_named("Bulunanlar 1", &z) >= 0 && z.count == 2 && zone_channel(&z, 1, &ch) &&
	      !strcmp(ch.name, "P25 433.500"), "appended");
	/* Save all after that: a new zone with all three (the duplicate rule is per zone) */
	ok_run("up ctx wait:600");
	expect("toast=Bulunanlar_2:_3_saved");
	/* Save this one now goes to Bulunanlar 2, where it is: skipped */
	ok_run("down ok wait:600");
	expect("toast=Already_in_Bulunanlar_2");
}

static void fs_dup_in_list(void)
{
	p25_find(433500000, 0x1C8, 0, 0);
	p25_find(433500000, 0x1C8, 655, 0);	/* the same channel again (another talkgroup on the same NAC) */
	p25_find(433500000, 0x1C9, 0, 0);	/* another NAC: another channel */
	ui_push(SCR_P25SEARCH, 0);
	ok_run("wait:200 ctx wait:600");
	expect("toast=Bulunanlar_1:_2_saved,_1_there");
	zone_t z;
	check(zone_named("Bulunanlar 1", &z) >= 0 && z.count == 2, "two channels");
}

static void fs_dmr(void)
{
	dmr_find(438700000, 3, 2, DMRS_SYNC_BS);
	dmr_find(439100000, DMRS_CC_NONE, 3, DMRS_SYNC_MS);
	ui_push(SCR_DMRSEARCH, 0);
	ok_run("wait:200");
	expect("screen=dmrsearch shows=#_save_all");
	ok_run("ctx wait:600");
	expect("toast=Bulunanlar_1:_2_saved");
	zone_t z;
	channel_t ch;
	check(zone_named("Bulunanlar 1", &z) >= 0 && z.count == 2, "both");
	check(zone_channel(&z, 0, &ch) && !strcmp(ch.name, "DMR 439.100") && ch.mode == RADIO_MODE_DMR &&
	      ch.color_code == CODEPLUG_CC_ANY && ch.timeslot == CODEPLUG_TS_ANY && ch.listen_list == CODEPLUG_LIST_ANY &&
	      ch.rx_only && ch.contact == 0, "no CC named, both slots: Any, RX group Any, RX only");
	check(zone_channel(&z, 1, &ch) && !strcmp(ch.name, "DMR 438.700") && ch.color_code == 3 && ch.timeslot == 2 &&
	      ch.rx_only, "CC 3, slot 2");
	/* OK held on a picked find: saved into the newest find zone (here already: skipped); OK still tunes */
	ok_run("wait:3000 down");
	expect("shows=hold_OK_save");
	ok_run("long:ok wait:600");
	expect("screen=dmrsearch toast=Already_in_Bulunanlar_1");
	dmr_find(434000000, 1, 1, DMRS_SYNC_MS);
	ok_run("wait:100 up up down long:ok wait:600");
	expect("toast=Saved_to_Bulunanlar_1");
	check(zone_named("Bulunanlar 1", &z) >= 0 && z.count == 3 && zone_channel(&z, 2, &ch) &&
	      !strcmp(ch.name, "DMR 434.000") && ch.color_code == 1 && ch.timeslot == 1, "appended");
	ok_run("ok");
	expect("screen=home freq=434000000");
	expect("tx=0");
}

static void fs_zone_full(void)
{
	fx_sim.zone_cap = 2;
	p25_three();
	ui_push(SCR_P25SEARCH, 0);
	ok_run("wait:200 ctx wait:600");
	expect("toast=Zone_full:_2_saved");
	zone_t z;
	check(zone_named("Bulunanlar 1", &z) >= 0 && z.count == 2, "two of three");
	/* Save this one into it: full, so a new zone */
	ok_run("down down down ok wait:600");
	expect("toast=Saved_to_Bulunanlar_2");
}

static void fs_codeplug_full(void)
{
	/* the simulator holds 8 written channels: 8 finds fill it, the next save stops after none */
	for (uint32_t i = 0; i < 8; i++)
		p25_find(433000000 + i * 25000u, 0x100 + (uint16_t)i, 0, 0);
	ui_push(SCR_P25SEARCH, 0);
	ok_run("wait:200 ctx wait:1500");
	expect("toast=Bulunanlar_1:_8_saved");
	fx_sim.p25.found[0].nac = 0x200;	/* a new find */
	ok_run("ctx wait:1500");
	expect("toast=Stopped:_0_saved");
	check(!ui_findsave_busy(), "not stuck");
	zone_t z;
	check(zone_named("Bulunanlar 2", &z) >= 0 && z.count == 0, "the zone stays, empty");
}

static void fs_no_zone_room(void)
{
	/* the simulator holds six zones: three built in, three made here; the fourth is refused */
	p25_find(433500000, 0x1C8, 655, 77285);
	ui_push(SCR_P25SEARCH, 0);
	ok_run("wait:200 ctx wait:600 ctx wait:600 ctx wait:600");
	expect("toast=Bulunanlar_3:_1_saved");
	ok_run("ctx wait:600");
	expect("toast=Stopped:_0_saved");
	check(!ui_findsave_busy() && codeplug_zone_count() == 6, "nothing more");
}

static void fs_nothing(void)
{
	ui_push(SCR_P25SEARCH, 0);
	ok_run("wait:200 ctx");
	expect("toast=Nothing_found_to_save");
	check(codeplug_zone_count() == 3, "no zone made");
}

/* --- item 28: find my friend --- */

static void fw_heard(void)
{
	ui_push(SCR_APRS_HEARD, 0);
	ok_run("wait:200");
	expect("screen=aprs-heard shows=OK_follow");
	ok_run("down ok wait:200");	/* the second row: TA1ABC-9 */
	expect("screen=goto shows=Follow_TA1ABC-9 shows=Heard shows=Back_ends_following");
	layout();
	/* it moves: the new position within a second */
	ok_run("wait:1100");
	expect("shows=2.8_km_NE");
	fx_sim.st[1].lat = 400000000;
	fx_sim.st[1].lon = 328600000;
	fx_sim.st[1].heard_ms = ui_st.now_ms;
	ok_run("wait:1100");
	expect("shows=7.8_km_N");
	/* not heard for 10 minutes: stale, the last position kept */
	ok_run("wait:601000");
	expect("shows=STALE:_heard shows=7.8_km_N");
	/* heard again: fresh */
	fx_sim.st[1].heard_ms = ui_st.now_ms;
	ok_run("wait:1100");
	expect("hides=STALE shows=Heard");
	/* Back ends it */
	ok_run("back");
	expect("screen=aprs-heard toast=Following_ended");
	ui_push(SCR_GOTO, 0);
	ok_run("wait:200");
	expect("screen=goto hides=Follow_TA1ABC");
}

static void fw_up_ends(void)
{
	ui_push(SCR_APRS_HEARD, 0);
	ok_run("wait:200 ok wait:200");
	expect("screen=goto shows=Follow_TA2XYZ-7");
	ok_run("down");
	expect("screen=goto toast=Following_ended hides=Follow_TA2XYZ");
}

static void fw_station_gone(void)
{
	ui_push(SCR_APRS_HEARD, 0);
	ok_run("wait:200 down ok wait:200");
	expect("shows=Follow_TA1ABC-9");
	fx_sim.nst = 1;	/* dropped from the heard list: the last position stays */
	ok_run("wait:2200");
	expect("shows=Follow_TA1ABC-9 shows=2.8_km_NE");
}

static void fw_map(void)
{
	ui_push(SCR_MAP, 0);
	ok_run("wait:200 0");	/* the station nearest the centre */
	expect("screen=map shows=TA1ABC-9");
	ok_run("5 wait:200");
	expect("screen=goto shows=Follow_TA1ABC-9");
	ok_run("back");
	expect("screen=map");
	ok_run("back 5");	/* nothing picked */
	expect("toast=0_picks_a_station_to_follow");
}

/* --- item 30: locator calculator --- */

static void loc_flow(void)
{
	ui_push(SCR_GOTO, 0);
	ok_run("wait:200");
	expect("shows=#_locator");
	ok_run("ctx");
	expect("screen=locator shows=#_types_a_locator shows=Here_KM69kw");
	/* K (5 twice), M (6), 6 and 9 held, k (5 twice), w (9) */
	ok_run("ctx wait:100");
	expect("screen=aprs-text");
	ok_run("5 5 6 long:6 long:9 5 5 9 ok wait:100");
	expect("screen=locator shows=KM69kw shows=39.9375_N__32.8750_E shows=Here_KM69kw");
	layout();
	ok_run("ok");
	expect("toast=Saved_as_a_waypoint");
	nav_point_t p;
	check(nav_count() == 1 && nav_get(0, &p) && !strcmp(p.name, "KM69kw") && labs(p.lat_e7 - 399375000) < 2 &&
	      labs(p.lon_e7 - 328750000) < 2, "the waypoint KM69kw at its centre");
	/* Back: Go-To, the new waypoint its target */
	ok_run("back");
	expect("screen=goto shows=KM69kw");
}

static void loc_far(void)
{
	ui_push(SCR_LOCATOR, 0);
	ok_run("wait:100 ctx wait:100 5 6 6 long:5 long:8 ok wait:100");
	expect("screen=locator shows=JN58 shows=48.5000_N__11.0000_E shows=1976_km_NW");
	/* a bad one */
	ok_run("ctx wait:100 back back back back 5 6 long:6 ok wait:100");
	expect("screen=locator toast=Not_a_locator:_like_KM69_or_KM69kw");
	ok_run("ok");
	expect("toast=#_types_a_locator");
	check(nav_count() == 0, "nothing saved");
	/* Back to Go-To */
	ui_push(SCR_GOTO, 0);
	ok_run("wait:100 ctx back");
	expect("screen=goto");
}

/* --- item 33: one-key SOS --- */

static int asks(void)
{
	return fx_sim.emerg_on + fx_sim.aprs_on;
}

static void sos_settings(void)
{
	ui_push(SCR_SOS_SET, 0);
	ok_run("wait:100");
	expect("screen=sos-set count=2 shows=APRS_SOS shows=DMR_emergency shows=Off");
	check(sos_mask() == 0, "all off at power on");
	ok_run("ok down ok");
	check(sos_mask() == (SOS_APRS | SOS_DMR), "both on");
	expect("shows=On");
	ok_run("ok");
	check(sos_mask() == SOS_APRS, "DMR off again");
	layout();
	check(asks() == 0, "nothing sent from the settings");
}

/* Round 14 item 48: the actions are in Settings > Keys (its last row), no longer in Tools */
static void sos_from_keys(void)
{
	ok_run("ok 7 8 up");
	expect("screen=group shows=SOS_key");
	layout();
	ok_run("ok");
	expect("screen=sos-set shows=APRS_SOS");
	ok_run("back");
	expect("screen=group");
}

static void sos_none(void)
{
	key(ACT_SOS, GESTURE_PRESS);
	expect("screen=sos shows=No_SOS_actions shows=Pick:_Settings_>_Keys");
	layout();
	ok_run("ok");
	expect("screen=sos toast=No_SOS_actions:_Settings_>_Keys");
	key(ACT_SOS, GESTURE_LONG);
	ok_run("wait:3000");
	check(asks() == 0 && !sos_active(), "nothing sent with every action off");
}

static void sos_cancel_confirm(void)
{
	sos_mask_set(SOS_APRS | SOS_DMR);
	key(ACT_SOS, GESTURE_PRESS);
	expect("screen=sos shows=Send_SOS_now? shows=APRS_SOS shows=DMR_emergency shows=It_TRANSMITS");
	layout();
	ok_run("wait:5000 back");
	expect("screen=home");
	check(asks() == 0, "Back: nothing sent");
	/* OK confirms */
	key(ACT_SOS, GESTURE_PRESS);
	ok_run("ok wait:200");
	expect("screen=sos shows=SOS_is_on");
	check(fx_sim.emerg_on == 1 && fx_sim.aprs_on == 1, "both asked once");
	expect("toast=DMR_emergency_sent");
	ok_run("wait:5000");
	check(asks() == 2, "not asked again");
	/* the key again: Stop SOS? (pressed on the page, or from elsewhere) */
	key(ACT_SOS, GESTURE_PRESS);
	expect("screen=sos shows=Stop_SOS?");
	ok_run("back");
	key(ACT_SOS, GESTURE_PRESS);
	expect("screen=sos shows=Stop_SOS?");
	ok_run("ok");
	expect("toast=SOS_stopped shows=SOS_ended");
	check(fx_sim.emerg_off == 1 && fx_sim.aprs_off == 1 && !sos_active(), "stopped");
	check(asks() == 2, "nothing new");
}

static void sos_long_and_locked(void)
{
	sos_mask_set(SOS_APRS);
	/* the keys locked: the SOS key still opens its page; OK is locked, the key held confirms */
	ok_run("lockkey");
	key(ACT_SOS, GESTURE_PRESS);
	expect("screen=sos shows=Send_SOS_now?");
	ok_run("ok");
	check(asks() == 0, "OK locked");
	key(ACT_SOS, GESTURE_LONG);
	expect("screen=sos shows=SOS_is_on");
	check(fx_sim.aprs_on == 1 && fx_sim.emerg_on == 0, "the APRS SOS only");
	/* the key held on the Stop page stops */
	key(ACT_SOS, GESTURE_PRESS);
	expect("shows=Stop_SOS?");
	key(ACT_SOS, GESTURE_LONG);
	check(fx_sim.aprs_off == 1 && !sos_active(), "stopped with the key held");
}

static void sos_refused(void)
{
	sos_mask_set(SOS_APRS);
	fx_sim.aprs_why = APRS_NO_POSITION;
	key(ACT_SOS, GESTURE_PRESS);
	ok_run("ok wait:200");
	expect("toast=SOS_not_sent:_no_position shows=APRS_SOS:_no_position shows=SOS_ended");
	check(!sos_active(), "nothing running");
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

int main(void)
{
	static const struct { const char *name; case_fn fn; } cases[] = {
		{ "I23 save all", fs_save_all },
		{ "I23 save this one", fs_save_one },
		{ "I23 duplicate in the list", fs_dup_in_list },
		{ "I23 DMR", fs_dmr },
		{ "I23 zone full", fs_zone_full },
		{ "I23 codeplug full", fs_codeplug_full },
		{ "I23 no room for a zone", fs_no_zone_room },
		{ "I23 nothing found", fs_nothing },
		{ "I28 follow from heard", fw_heard },
		{ "I28 Up ends", fw_up_ends },
		{ "I28 station gone", fw_station_gone },
		{ "I28 follow from the map", fw_map },
		{ "I30 locator", loc_flow },
		{ "I30 locator far and bad", loc_far },
		{ "I33 settings", sos_settings },
		{ "R14 SOS key in Settings > Keys", sos_from_keys },
		{ "I33 nothing chosen", sos_none },
		{ "I33 cancel, confirm, stop", sos_cancel_confirm },
		{ "I33 key held, keys locked", sos_long_and_locked },
		{ "I33 refused", sos_refused },
	};
	int failed = 0;
	unsigned n = sizeof cases / sizeof cases[0];
	for (unsigned i = 0; i < n; i++)
		failed += run_case(cases[i].name, cases[i].fn);
	printf("Plus round 13 ideas 1 UI: %u flows, %d failures\n", n, failed);
	return failed != 0;
}
