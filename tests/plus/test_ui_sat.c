/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus Tools > Satellites through the real UI in the upstream simulator (-DCONFIG_SATELLITES=1): the list, the
 * live lines (range rate, Doppler in kHz, "Tracking"), OK tracking and stopping, the pass alarm page (open from a
 * satellite with nothing to track or with 0, each row's values, arm, change while armed, off, Back to the list), the
 * alarm's banner and tone on another screen (Home) and its toast over a banner that waits for the user, the screen
 * without the GPS time, and the layout check on the list and the page.
 *
 * ui_sat.c is included here (its statics start each flow afresh); the radio task's service (sat_svc.c) is a fake
 * with sat.h's contract: a view the test sets, the requests recorded, an armed alarm due 2 s later (its warning) and
 * 10 s later (the rise). The math (sat.c, the generated table) is the real one. */
#define main upstream_ui_main
#include "uiFlowTest.c"
#undef main
#include "ui/ui_sat.c"

/* --- The fake service --- */
static sat_view_t fv;
static int32_t t0;			/* UTC (seconds since 2000) when the screen first asks */
static uint32_t base_ms;		/* the sim time then (0: not yet) */
static uint8_t focus_seen;
static int track_calls, alarm_calls;
static uint8_t al_lead, al_el;
static uint32_t soon_ms, aos_ms;	/* sim times the armed alarm is due */
static int beeps_soon, beeps_aos;
static uint8_t iss, so50, ao91, po101;

static int32_t fake_now(void)
{
	if (base_ms == 0)
		base_ms = sim_now() ? sim_now() : 1u;
	return t0 + (int32_t)((sim_now() - base_ms) / 1000u);
}

void sat_view(sat_view_t *out)
{
	*out = fv;
	out->now = fake_now();
	out->live = focus_seen;
	out->live_ok = focus_seen < sat_count;
}

void sat_seen(uint8_t focus) { focus_seen = focus; }

void sat_track(uint8_t idx)
{
	track_calls++;
	fv.track = idx;
	fv.track_state = idx == SAT_NONE ? SAT_TRK_OFF : SAT_TRK_ON;
	if (idx != SAT_NONE) {
		fv.rx_hz = sat_table[idx].down_hz + 10000u;
		fv.tx_hz = sat_table[idx].up_hz - 2000u;
	}
}

void sat_arm(bool on) { fv.arm = on; }

void sat_alarm_set(uint8_t target, uint8_t lead, uint8_t min_el_deg)
{
	alarm_calls++;
	fv.alarm.target = target;
	fv.alarm.lead_min = al_lead = lead;
	fv.alarm.min_el = al_el = min_el_deg;
	fv.alarm.state = target == SAT_NONE ? SAT_AL_OFF : SAT_AL_ARMED;
	fv.alarm.soon = false;
	fv.alarm.sat = target == SAT_ALARM_ANY ? ao91 : target;
	fv.alarm.max_el = target == SAT_ALARM_ANY ? 61 : 23;
	fv.alarm.aos = fake_now() + lead * 60;
	soon_ms = target == SAT_NONE ? 0 : sim_now() + 2000u;
	aos_ms = target == SAT_NONE ? 0 : sim_now() + 10000u;
}

bool sat_alarm_take(sat_alarm_event_t *out)
{
	uint32_t now = sim_now();
	if (soon_ms && now >= soon_ms) {
		soon_ms = 0;
		fv.alarm.soon = true;
		*out = (sat_alarm_event_t){ SAT_ALARM_SOON, fv.alarm.sat, fv.alarm.max_el, (int32_t)fv.alarm.lead_min * 60 };
		return true;
	}
	if (aos_ms && now >= aos_ms) {
		aos_ms = 0;
		*out = (sat_alarm_event_t){ SAT_ALARM_AOS, fv.alarm.sat, fv.alarm.max_el, 0 };
		fv.alarm.target = SAT_NONE;
		fv.alarm.state = SAT_AL_OFF;
		return true;
	}
	return false;
}

uint8_t aprs_iss_send(uint32_t hz) { return APRS_OK; }

void radio_req_beep(uint8_t beep)
{
	beeps_soon += beep == BEEP_TOT_WARN;
	beeps_aos += beep == BEEP_SCAN_HOLD;
}

static uint8_t index_of(uint32_t catnum)
{
	for (uint8_t i = 0; i < sat_count; i++)
		if (sat_table[i].catnum == catnum)
			return i;
	return SAT_NONE;
}

/* A fresh radio task's view: the ISS due in 5 min (it can be tracked), SO-50 in 2 h, AO-91 in 5 h, PO-101 none */
static void fake_reset(bool clock)
{
	memset(&fv, 0, sizeof fv);
	fv.clock = clock ? SAT_CLOCK_OK : SAT_CLOCK_NONE;
	fv.track = SAT_NONE;
	fv.alarm.target = SAT_NONE;
	fv.az10 = 1234;
	fv.el10 = -12;
	fv.range_km = 2345;
	fv.rate_ms = -5432;
	fv.cost_us = 1500;
	t0 = (int32_t)sat_table[iss].el.epoch + 2 * 86400;
	fv.pass[iss] = (sat_pass_t){ t0 + 300, t0 + 900, 47, SAT_PASS_OK };
	fv.pass[so50] = (sat_pass_t){ t0 + 7200, t0 + 7800, 23, SAT_PASS_OK };
	fv.pass[ao91] = (sat_pass_t){ t0 + 18000, t0 + 18600, 61, SAT_PASS_OK };
	fv.pass[po101] = (sat_pass_t){ 0, 0, 0, SAT_PASS_NONE };
	focus_seen = SAT_NONE;
	base_ms = 0;
	track_calls = alarm_calls = 0;
	soon_ms = aos_ms = 0;
	beeps_soon = beeps_aos = 0;
	/* ui_sat.c's own state, as after power-on */
	sel = 0;
	tz_min = 180;
	alarm_page = false;
	a_row = a_for = a_el = 0;
	a_lead = 1;
}

#define SATS "ok 6 down down down down down down down down ok "	/* Tools > Satellites */
#define PAGE SATS "down ok "		/* SO-50 (nothing to track): the alarm page */

typedef struct {
	flow_t f;
	bool clock;
	int track;		/* -1: not checked; else the satellite tracked after (SAT_NONE: none) */
	int alarm;		/* -1: not checked; else the alarm's target after */
	int lead, el;		/* with alarm: what it was armed with */
	int soon, aos;		/* -1: not checked; else the tones asked for */
} sat_flow_t;

int main(int argc, char **argv)
{
	iss = index_of(25544u);
	so50 = index_of(27607u);
	ao91 = index_of(43017u);
	po101 = index_of(43678u);
	if (iss == SAT_NONE || so50 == SAT_NONE || ao91 == SAT_NONE || po101 == SAT_NONE) {
		printf("FAIL the shipped table lacks a satellite these flows use\n");
		return 1;
	}
	if (argc > 1) {		/* probe: run a script, print the screen's texts */
		char err[300];
		fake_reset(true);
		sim_reset(sim_geom_find("240x320i10x5"), SIM_MAP_M);
		sim_run(argv[1], err, sizeof err);
		ui_app_status_t st;
		ui_app_status(&st);
		printf("screen=%s focus=%d toast=%s banner=%s\n", st.screen, (int)st.focus, st.toast, st.banner);
		sim_run("expect:shows=@@@", err, sizeof err);
		printf("%s\n", err);
		if (argc > 2)
			sim_save_png(argv[2], 2);
		return 0;
	}
	const sat_flow_t checks[] = {
		{ { "Sat list: soonest first, the ISS can be tracked", SATS, "",
		    "screen=satellites shows=ISS shows=SO-50 shows=AO-91 shows=OK_track__#_APRS_via_ISS", 0, false },
		  true, SAT_NONE, SAT_NONE, 0, 0, -1, -1 },
		{ { "Sat live lines: Az/El, range rate, Doppler in kHz", SATS, "",
		    "shows=Az_123 shows=El_-1.2 shows=2345_km__-5.43_km/s shows=+7.9_kHz shows=OK:_track_in_the_VFO", 0, false },
		  true, SAT_NONE, -1, 0, 0, -1, -1 },
		{ { "Sat track the ISS", SATS, "ok", "toast=VFO_tracks_the_pass", 1, false },
		  true, 0, -1, 0, 0, -1, -1 },
		{ { "Sat tracking shown", SATS, "ok wait:2500", "shows=Tracking_Doppler shows=OK_stop", 1, false },
		  true, 0, -1, 0, 0, -1, -1 },
		{ { "Sat stop tracking", SATS, "ok ok", "toast=Tracking_off:_VFO_stays", 2, false },
		  true, SAT_NONE, -1, 0, 0, -1, -1 },
		{ { "Sat nothing to track: OK alarm", SATS, "down", "shows=^v_pick__OK_alarm", 1, false },
		  true, SAT_NONE, SAT_NONE, 0, 0, -1, -1 },
		{ { "Sat alarm page", SATS, "down ok",
		    "screen=satellites shows=Pass_alarm shows=For shows=SO-50 shows=Before_AOS shows=5_min "
		    "shows=Min._elevation shows=Off:_OK_arms_it shows=OK_arm", 2, false },
		  true, SAT_NONE, SAT_NONE, 0, 0, -1, -1 },
		{ { "Sat alarm armed", PAGE, "ok", "toast=Pass_alarm_armed", 1, false },
		  true, -1, 1, 5, 0, -1, -1 },
		{ { "Sat alarm armed: the pass and the tone time", PAGE, "ok wait:2100",
		    "shows=SO-50 shows=Tone_again_at_AOS shows=OK_off__<>_change", 1, false },
		  true, -1, 1, 5, 0, 1, 0 },
		{ { "Sat alarm lead and elevation", PAGE, "down right down right right ok",
		    "shows=10_min shows=20" GLYPH_DEGREE " toast=Pass_alarm_armed", 6, false },
		  true, -1, 1, 10, 20, -1, -1 },
		{ { "Sat alarm lead wraps", PAGE, "down right right", "shows=2_min", 3, false },
		  true, -1, SAT_NONE, 0, 0, -1, -1 },
		{ { "Sat alarm any satellite", PAGE, "left left ok", "shows=Any_satellite toast=Pass_alarm_armed", 3, false },
		  true, -1, SAT_ALARM_ANY, 5, 0, -1, -1 },
		{ { "Sat alarm changed while armed", PAGE, "ok down down right", "shows=10" GLYPH_DEGREE, 4, false },
		  true, -1, 1, 5, 10, -1, -1 },
		{ { "Sat alarm off", PAGE, "ok ok", "toast=Pass_alarm_off shows=Off:_OK_arms_it", 2, false },
		  true, -1, SAT_NONE, 0, 0, -1, -1 },
		{ { "Sat alarm page: Back to the list", PAGE, "back", "screen=satellites shows=Satellites hides=Pass_alarm", 1, false },
		  true, -1, SAT_NONE, 0, 0, -1, -1 },
		{ { "Sat alarm page: Back twice leaves", PAGE, "back back", "hides=Pass_alarm hides=SO-50", 2, false },
		  true, -1, SAT_NONE, 0, 0, -1, -1 },
		{ { "Sat 0: the alarm page on the ISS", SATS, "0", "shows=Pass_alarm shows=ISS", 1, false },
		  true, SAT_NONE, SAT_NONE, 0, 0, -1, -1 },
		{ { "Sat alarm on Home: the warning", PAGE "ok long:back", "wait:2500",
		    "screen=home banner=SO-50_in_5_min,_max_23" GLYPH_DEGREE, 0, false },
		  true, -1, 1, 5, 0, 1, 0 },
		{ { "Sat alarm on Home: the rise", PAGE "ok long:back", "wait:10500",
		    "screen=home banner=SO-50_AOS_now,_max_23" GLYPH_DEGREE, 0, false },
		  true, -1, SAT_NONE, 5, 0, 1, 1 },
		{ { "Sat alarm over a zone offer: a toast", "zoneoffer:3101234 " PAGE "ok long:back", "wait:2500",
		    "shows=Zone_from toast=SO-50_in_5_min", 0, false },
		  true, -1, 1, 5, 0, 1, 0 },
		{ { "Sat without the GPS time", SATS, "", "shows=Waiting_for_the_GPS_time", 0, false },
		  false, -1, SAT_NONE, 0, 0, -1, -1 },
		{ { "Sat without the GPS time: alarm armed", SATS, "0 ok", "shows=Armed:_waits_for_GPS", 2, false },
		  false, -1, 0, 5, 0, -1, -1 },
	};
	int sat_fails = 0;
	for (unsigned i = 0; i < sizeof checks / sizeof checks[0]; i++) {
		const sat_flow_t *c = &checks[i];
		fake_reset(c->clock);
		int before = fails;
		run_flow(&c->f, sim_geom_find("240x320i10x5"), SIM_MAP_M, "Plus DM32");
		if (c->track >= 0 && fv.track != c->track) {
			printf("FAIL %s: tracking %d, want %d\n", c->f.name, fv.track, c->track);
			fails++;
		}
		if (c->alarm >= 0 && (fv.alarm.target != c->alarm ||
				      (c->alarm != SAT_NONE && (al_lead != c->lead || al_el != c->el)))) {
			printf("FAIL %s: alarm %d %d min %d deg, want %d %d %d\n", c->f.name, fv.alarm.target, al_lead, al_el,
			       c->alarm, c->lead, c->el);
			fails++;
		}
		if ((c->soon >= 0 && beeps_soon != c->soon) || (c->aos >= 0 && beeps_aos != c->aos)) {
			printf("FAIL %s: tones %d / %d, want %d / %d\n", c->f.name, beeps_soon, beeps_aos, c->soon, c->aos);
			fails++;
		}
		sat_fails += fails != before;
	}
	/* The layout check on the list (tracking, alarm armed) and on the page */
	static const char *const layouts[] = { SATS "ok wait:2500", PAGE "ok wait:2500", PAGE "left left ok wait:2500",
					       SATS "down down down" };
	for (unsigned i = 0; i < sizeof layouts / sizeof layouts[0]; i++) {
		char err[300], issues[SIM_LAYOUT_MAX][160];
		fake_reset(true);
		sim_reset(sim_geom_find("240x320i10x5"), SIM_MAP_M);
		if (!sim_run(layouts[i], err, sizeof err)) {
			printf("FAIL Sat layout %u: %s\n", i, err);
			fails++;
			continue;
		}
		int n = sim_layout_issues(issues, SIM_LAYOUT_MAX);
		for (int k = 0; k < n; k++)
			printf("FAIL Sat layout %u: %s\n", i, issues[k]);
		fails += n != 0;
		flows_run++;
	}
	printf("Plus Satellites screen: %d flows, %d failures\n", flows_run, fails);
	(void)sat_fails;
	return fails != 0;
}
