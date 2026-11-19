/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus satellites, the service (sat.h): runs in the radio task at its poll, and only while Tools > Satellites
 * draws (sat_seen), a pass is tracked or the pass alarm is armed. One job a poll at most, each one SGP4 initialization
 * and one or two propagations (a millisecond or so each with soft float, docs/plus.md): once a second the live look at
 * the satellite in focus (or the one tracked) and the tracking's retune, in between the pass search a little at a time.
 *
 * Time: the GPS's last UTC fix (aprs_gps) carried on by hal_millis, so a fix lost later keeps the clock going.
 * Tracking: OK on a pass under way or due within 10 minutes. The VFO goes to the downlink (RX) and the uplink (TX),
 * FM, wide, the uplink CTCSS, each corrected for Doppler and moved again once the correction is more than half a step
 * off (1 kHz VHF, 2.5 kHz UHF, with a margin: sat_follow_hz); an uplink move alone leaves the receiver alone. Never
 * while transmitting. A VFO changed by hand (or a channel chosen) ends it and the VFO stays where it was put; OK
 * again ends it and the VFO stays; the set ends it (30 s after) and puts the VFO back on the nominal downlink and
 * uplink. Nothing here keys the transmitter.
 *
 * The pass alarm (sat_alarm_set): with the screen closed only the clock (once a second) and the alarm's own pass
 * search run (one propagation a poll, until the pass is found; the screen's result is taken when it has one), then a
 * comparison of the time once a second until the alarm is due; no SGP4 then. */
#include "config.h"
#if CONFIG_SATELLITES
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "hal_time.h"
#include "aprs/aprs.h"
#include "aprs/aprs_fmt.h"
#include "radio/radio.h"
#include "radio/sat.h"

#define SEEN_MS		4000u	/* the screen's draws keep the service on this long */
#define LIVE_MS		1000u
#define CLOCK_MS	200u
#define SEARCH_EVALS	2	/* propagations a radio poll for the pass search (1 while the native receiver owns) */
#define SOON_S		600	/* a pass due this soon can be tracked (the VFO is set before it rises) */
#define TRACK_AFTER_S	30	/* tracking ends this long after the set */
#define MOVED_E7	2000000	/* 0.2 degree (about 20 km): the passes are searched again */
#define JUMP_S		60	/* the GPS time against the running clock: a bigger jump searches again */
#define ALARM_GAP_S	5	/* the pass after one is looked for from this long after its set (found to 2 s) */

/* The radio task writes, the UI copies; both in a critical section */
static sat_view_t view = { .live = SAT_NONE, .track = SAT_NONE, .alarm = { .target = SAT_NONE } };
static volatile uint32_t seen_ms;
static volatile bool seen_any;
static volatile uint8_t want_focus = SAT_NONE;
static volatile uint8_t want_track;
static volatile bool want_track_new, want_arm;
/* The alarm asked for (UI) and the last one due (radio task); critical sections */
static uint8_t want_al_target, want_al_lead, want_al_el;
static volatile bool want_al_new;
static sat_alarm_event_t al_ev;
static volatile uint8_t al_ev_seq;
static uint8_t al_ev_taken;	/* UI task */

/* Radio task only */
static struct {
	int32_t anchor_s;	/* GPS time at anchor_ms */
	uint32_t anchor_ms, clock_ms, live_ms;
	uint32_t gps_hms, gps_date;
	bool clock_ok, pos_ok, searching;
	int32_t lat, lon, alt;	/* where the observer was set up */
	int32_t pass_lat, pass_lon;	/* where the passes were searched from */
	sat_obs_t obs;
	uint8_t search_sat;
	sat_search_t search;
	/* tracking */
	uint8_t track;
	uint8_t grace;		/* seconds the VFO may still differ (the first tuning settling) */
	bool setup, arm;
	int32_t track_los;
	uint32_t rx_set, tx_set, rx_prev, tx_prev;
} e = { .track = SAT_NONE };

/* The alarm's pass search (radio task) */
static struct {
	bool restart;		/* look for the pass (again) at the next poll with a clock */
	bool searching;
	uint8_t k;		/* the satellite searched */
	uint8_t best_sat;
	int32_t from_s;		/* when the looking started: passes up to SAT_SEARCH_S later */
	sat_pass_t best;	/* the soonest pass found that will do */
	sat_search_t s;
} al;

void sat_view(sat_view_t *out)
{
	taskENTER_CRITICAL();
	*out = view;
	taskEXIT_CRITICAL();
}

void sat_seen(uint8_t focus)
{
	want_focus = focus;
	seen_ms = hal_millis();
	seen_any = true;
}

void sat_track(uint8_t idx)
{
	want_track = idx;
	want_arm = false;
	want_track_new = true;
}

void sat_arm(bool on)
{
	want_arm = on;
}

void sat_alarm_set(uint8_t target, uint8_t lead_min, uint8_t min_el)
{
	taskENTER_CRITICAL();
	want_al_target = target;
	want_al_lead = lead_min;
	want_al_el = min_el;
	want_al_new = true;
	taskEXIT_CRITICAL();
}

bool sat_alarm_take(sat_alarm_event_t *out)
{
	if (al_ev_seq == al_ev_taken)
		return false;
	taskENTER_CRITICAL();
	*out = al_ev;
	al_ev_taken = al_ev_seq;
	taskEXIT_CRITICAL();
	return true;
}

static void research_all(void)
{
	e.searching = false;
	e.pass_lat = e.lat;
	e.pass_lon = e.lon;
	taskENTER_CRITICAL();
	for (uint8_t i = 0; i < SAT_MAX; i++)
		view.pass[i].state = SAT_PASS_UNKNOWN;
	taskEXIT_CRITICAL();
	if (view.alarm.target != SAT_NONE)
		al.restart = true;	/* another place or time: the alarm's pass again too */
}

static int32_t now_s(uint32_t now_ms)
{
	return e.anchor_s + (int32_t)((now_ms - e.anchor_ms) / 1000u);
}

static double now_exact(uint32_t now_ms)
{
	return (double)e.anchor_s + (double)(now_ms - e.anchor_ms) / 1000.0;
}

/* The GPS's time and position, when they change */
static void clock_update(uint32_t now_ms)
{
	aprs_gps_t g;
	if (!aprs_gps(&g, now_ms))
		return;
	if (g.date != 0 && g.utc_hms != 0xFFFFFFFFu && (g.utc_hms != e.gps_hms || g.date != e.gps_date)) {
		int32_t s = sat_s2000(g.date, g.utc_hms);
		if (s >= 0) {
			bool jump = !e.clock_ok || s - now_s(now_ms) > JUMP_S || now_s(now_ms) - s > JUMP_S;
			e.gps_hms = g.utc_hms;
			e.gps_date = g.date;
			e.anchor_s = s;
			e.anchor_ms = now_ms;
			e.clock_ok = true;
			if (jump)
				research_all();
		}
	}
	if (g.fix) {
		int32_t alt = g.alt_m != APRS_ALT_NONE ? g.alt_m : 0;
		int32_t dlat = g.lat - e.lat, dlon = g.lon - e.lon;
		if (!e.pos_ok || dlat > 10000 || dlat < -10000 || dlon > 10000 || dlon < -10000) {	/* 0.001 deg */
			e.lat = g.lat;
			e.lon = g.lon;
			e.alt = alt;
			sat_observer(e.lat, e.lon, e.alt, &e.obs);
			dlat = e.lat - e.pass_lat;
			dlon = e.lon - e.pass_lon;
			if (!e.pos_ok || dlat > MOVED_E7 || dlat < -MOVED_E7 || dlon > MOVED_E7 || dlon < -MOVED_E7) {
				e.pos_ok = true;
				research_all();
			}
		}
	}
}

/* The live look at one satellite (the SGP4 state on this frame only, not under the requests below) */
static __attribute__((noinline)) bool live_look(uint8_t i, double t, sat_look_t *l)
{
	sgp4_t p;
	if (sgp4_init(&p, &sat_table[i].el) != SGP4_OK)
		return false;
	uint32_t t0 = hal_micros();
	bool ok = sat_eval(&p, &sat_table[i], t, &e.obs, l) == SGP4_OK;
	uint32_t us = hal_micros() - t0;
	if (us > view.cost_us)
		view.cost_us = (uint16_t)(us > 0xFFFFu ? 0xFFFFu : us);
	return ok;
}

/* The screen's pass search: false when there was nothing to search */
static __attribute__((noinline)) bool search_job(int32_t now, int evals)
{
	if (!e.searching) {
		uint8_t i = 0;
		while (i < sat_count && view.pass[i].state != SAT_PASS_UNKNOWN)
			i++;
		if (i >= sat_count)
			return false;
		e.search_sat = i;
		e.searching = true;
		sat_search_start(&e.search, now);
	}
	sgp4_t p;
	const sat_info_t *s = &sat_table[e.search_sat];
	bool done = sgp4_init(&p, &s->el) != SGP4_OK;
	if (done)
		e.search.out.state = SAT_PASS_BAD;
	for (int k = 0; k < evals && !done; k++)
		done = sat_search_step(&e.search, &p, s, &e.obs);
	if (done) {
		sat_pass_t out = e.search.out;
		if (out.state == SAT_PASS_NONE)
			out.los = e.search.start + SAT_SEARCH_S;	/* looked up to here: again half way */
		e.searching = false;
		taskENTER_CRITICAL();
		view.pass[e.search_sat] = out;
		taskEXIT_CRITICAL();
	}
	return true;
}

/* --- The pass alarm (the radio task is view.alarm's only writer: it reads it without a critical section) --- */

static void alarm_state(uint8_t state)
{
	taskENTER_CRITICAL();
	view.alarm.state = state;
	if (state == SAT_AL_ARMED) {
		view.alarm.sat = al.best_sat;
		view.alarm.max_el = al.best.max_el;
		view.alarm.aos = al.best.aos;
		view.alarm.soon = false;
	}
	taskEXIT_CRITICAL();
}

static bool alarm_fits(const sat_pass_t *p, int32_t now)
{
	return p->state == SAT_PASS_OK && p->aos > now && p->max_el >= (int8_t)view.alarm.min_el;
}

static void alarm_found(uint8_t k, const sat_pass_t *p)
{
	if (al.best.state != SAT_PASS_OK || p->aos < al.best.aos) {
		al.best = *p;
		al.best_sat = k;
	}
}

/* Satellite k and those after it (any satellite) or k alone: a pass from the screen's result when that is the next
 * one from now, else a search started on it; once every satellite is done the alarm is armed for the soonest pass
 * found, or has none */
static void alarm_from(uint8_t k, int32_t now)
{
	uint8_t last = view.alarm.target == SAT_ALARM_ANY ? sat_count : (uint8_t)(view.alarm.target + 1u);
	for (; k < last; k++) {
		const sat_pass_t *p = &view.pass[k];
		int32_t t = now;
		if (p->state == SAT_PASS_OK && p->los >= now) {
			if (alarm_fits(p, now)) {
				alarm_found(k, p);
				continue;
			}
			t = p->los + ALARM_GAP_S;	/* under way, or too low: the pass after it */
		}
		if (al.best.state == SAT_PASS_OK && t >= al.best.aos)
			continue;		/* nothing sooner can come of it */
		al.k = k;
		al.searching = true;
		sat_search_start(&al.s, t);
		return;
	}
	al.searching = false;
	alarm_state(al.best.state == SAT_PASS_OK ? SAT_AL_ARMED : SAT_AL_NONE);
}

static void alarm_start(int32_t now)
{
	al.restart = false;
	al.best.state = SAT_PASS_UNKNOWN;
	al.from_s = now;
	alarm_state(SAT_AL_FINDING);
	alarm_from(view.alarm.target == SAT_ALARM_ANY ? 0 : view.alarm.target, now);
}

static __attribute__((noinline)) void alarm_job(int32_t now)
{
	sgp4_t p;
	const sat_info_t *s = &sat_table[al.k];
	bool done = sgp4_init(&p, &s->el) != SGP4_OK;
	if (done)
		al.s.out.state = SAT_PASS_BAD;
	else
		done = sat_search_step(&al.s, &p, s, &e.obs);
	if (!done)
		return;
	sat_pass_t out = al.s.out;
	if (alarm_fits(&out, now)) {
		alarm_found(al.k, &out);
	} else if (out.state == SAT_PASS_OK && out.los + ALARM_GAP_S - al.from_s < SAT_SEARCH_S &&
		   (al.best.state != SAT_PASS_OK || out.los + ALARM_GAP_S < al.best.aos)) {
		sat_search_start(&al.s, out.los + ALARM_GAP_S);	/* too low (or under way): the one after it */
		return;
	}
	alarm_from((uint8_t)(al.k + 1u), now);
}

static void alarm_fire(uint8_t kind, int32_t now)
{
	taskENTER_CRITICAL();
	al_ev.kind = kind;
	al_ev.sat = view.alarm.sat;
	al_ev.max_el = view.alarm.max_el;
	al_ev.in_s = view.alarm.aos - now;
	al_ev_seq++;
	if (kind == SAT_ALARM_SOON) {
		view.alarm.soon = true;
	} else {
		view.alarm.target = SAT_NONE;	/* the rise: done */
		view.alarm.state = SAT_AL_OFF;
	}
	taskEXIT_CRITICAL();
	if (kind == SAT_ALARM_AOS)
		al.searching = false;
}

/* The UI's request: taken at once (the screen shows it), the pass looked for once there is a clock */
static void alarm_request(void)
{
	taskENTER_CRITICAL();
	uint8_t target = want_al_target;
	view.alarm.lead_min = want_al_lead;
	view.alarm.min_el = want_al_el;
	want_al_new = false;
	if (target != SAT_ALARM_ANY && target >= sat_count)
		target = SAT_NONE;
	view.alarm.target = target;
	view.alarm.state = target == SAT_NONE ? SAT_AL_OFF : SAT_AL_FINDING;
	taskEXIT_CRITICAL();
	al.searching = false;
	al.restart = target != SAT_NONE;
}

/* Once a second: integer comparisons only */
static void alarm_check(int32_t now)
{
	const sat_alarm_t *a = &view.alarm;
	if (a->state == SAT_AL_ARMED) {
		if (now >= a->aos + 60)
			al.restart = true;		/* the clock jumped past it: the next one */
		else if (now >= a->aos)
			alarm_fire(SAT_ALARM_AOS, now);
		else if (!a->soon && now >= a->aos - (int32_t)a->lead_min * 60)
			alarm_fire(SAT_ALARM_SOON, now);
	} else if (a->state == SAT_AL_NONE && now - al.from_s >= SAT_SEARCH_S / 2) {
		al.restart = true;			/* none in the span looked at: again half way */
	}
}

/* --- Tracking --- */

static void track_end(uint8_t why)
{
	e.track = SAT_NONE;
	taskENTER_CRITICAL();
	view.track = SAT_NONE;
	view.track_state = why;
	taskEXIT_CRITICAL();
}

static void track_request(int32_t now)
{
	uint8_t i = want_track;
	want_track_new = false;
	if (i >= sat_count) {
		if (e.track != SAT_NONE)
			track_end(SAT_TRK_OFF);
		return;
	}
	sat_pass_t p = view.pass[i];
	if (p.state != SAT_PASS_OK || p.aos - now > SOON_S || now > p.los)
		return;		/* nothing to follow now (the screen says so) */
	e.track = i;
	e.track_los = p.los;
	e.setup = true;
	e.arm = false;
	taskENTER_CRITICAL();
	view.track = i;
	view.track_state = SAT_TRK_ON;
	view.arm = false;
	taskEXIT_CRITICAL();
}

static subtone_t uplink_tone(const sat_info_t *s, bool arm)
{
	uint16_t v = arm && s->arm10 ? s->arm10 : s->ctcss10;
	subtone_t t = { v ? TONE_CTCSS : TONE_NONE, v };
	return t;
}

/* RX (which moves a split's TX along: so TX after it) only when it changes, or again (force) */
static void retune(uint32_t rx, uint32_t tx, bool force)
{
	if (force || rx != e.rx_set)
		radio_req_vfo_set(rx);
	radio_req_vfo_tx(tx);
	e.rx_prev = e.rx_set;
	e.tx_prev = e.tx_set;
	e.rx_set = rx;
	e.tx_set = tx;
}

static void track_service(int32_t now, const sat_look_t *l, bool vfo, uint32_t rx_hz, uint32_t tx_hz, bool transmitting)
{
	const sat_info_t *s = &sat_table[e.track];
	if (transmitting)
		return;		/* the VFO can't move now: at the next second */
	if (now > e.track_los + TRACK_AFTER_S) {
		/* Over: the VFO back on the nominal downlink and uplink, if it is still where the tracking put it */
		if (!e.setup && !e.grace && vfo && rx_hz == e.rx_set && tx_hz == e.tx_set)
			retune(s->down_hz, s->up_hz, false);
		track_end(SAT_TRK_LOS);
		return;
	}
	int32_t drx = sat_doppler_rx(s->down_hz, l->rate_kms), dtx = sat_doppler_tx(s->up_hz, l->rate_kms);
	if (e.setup) {
		/* Into the VFO (RX first: it opens it), FM, wide, the uplink tone and no receive tone, then the split */
		static const subtone_t none = { TONE_NONE, 0 };
		uint32_t rx = sat_tune_hz(s->down_hz, drx), tx = sat_tune_hz(s->up_hz, dtx);
		e.setup = false;
		e.grace = 3;
		radio_req_vfo_set(rx);
		radio_req_mode(RADIO_MODE_FM);
		radio_req_bandwidth(RADIO_BW_25K);
		radio_req_tone(RADIO_TONE_TX, uplink_tone(s, false));
		radio_req_tone(RADIO_TONE_RX, none);
		radio_req_vfo_tx(tx);
		e.rx_prev = e.tx_prev = 0;
		e.rx_set = rx;
		e.tx_set = tx;
	} else if (e.grace) {
		e.grace--;
	} else if (!vfo || rx_hz != e.rx_set || tx_hz != e.tx_set) {
		if (vfo && rx_hz == e.rx_prev && tx_hz == e.tx_prev) {
			retune(e.rx_set, e.tx_set, true);	/* the last retune didn't land (refused for a moment): again */
		} else {
			track_end(SAT_TRK_LEFT);	/* changed by hand: the VFO is the user's again */
			return;
		}
	} else {
		uint32_t rx = sat_follow_hz(s->down_hz, drx, e.rx_set), tx = sat_follow_hz(s->up_hz, dtx, e.tx_set);
		if (rx != e.rx_set || tx != e.tx_set)
			retune(rx, tx, false);
	}
	bool arm = want_arm && s->arm10 != 0;
	if (arm != e.arm) {
		e.arm = arm;
		radio_req_tone(RADIO_TONE_TX, uplink_tone(s, arm));
	}
	taskENTER_CRITICAL();
	view.rx_hz = e.rx_set;
	view.tx_hz = e.tx_set;
	view.arm = e.arm;
	taskEXIT_CRITICAL();
}

void sat_service(bool vfo, uint32_t rx_hz, uint32_t tx_hz, bool transmitting, bool light, uint32_t now_ms)
{
	bool looking = seen_any && now_ms - seen_ms < SEEN_MS;
	bool busy = looking || e.track != SAT_NONE || want_track_new;	/* the screen or the tracking */
	if (want_al_new)
		alarm_request();
	if (!busy && view.alarm.target == SAT_NONE)
		return;
	if (now_ms - e.clock_ms >= (busy ? CLOCK_MS : LIVE_MS)) {
		e.clock_ms = now_ms;
		clock_update(now_ms);
	}
	uint8_t clock = !e.clock_ok ? SAT_CLOCK_NONE : !e.pos_ok ? SAT_CLOCK_NO_POS : SAT_CLOCK_OK;
	view.clock = clock;
	if (clock != SAT_CLOCK_OK)
		return;
	int32_t now = now_s(now_ms);
	if (want_track_new)
		track_request(now);
	if (al.restart && view.alarm.target != SAT_NONE)
		alarm_start(now);
	if (now_ms - e.live_ms >= LIVE_MS) {
		e.live_ms = now_ms;
		/* Passes over (or looked for long enough ago) are looked for again */
		taskENTER_CRITICAL();
		view.now = now;
		for (uint8_t i = 0; i < sat_count; i++) {
			sat_pass_t *p = &view.pass[i];
			if ((p->state == SAT_PASS_OK && now > p->los) || (p->state == SAT_PASS_NONE && now > p->los - SAT_SEARCH_S / 2))
				p->state = SAT_PASS_UNKNOWN;
		}
		taskEXIT_CRITICAL();
		alarm_check(now);
		if (!busy)
			return;		/* the alarm alone: no SGP4 */
		uint8_t i = e.track != SAT_NONE ? e.track : want_focus;
		sat_look_t l;
		bool ok = i < sat_count && live_look(i, now_exact(now_ms), &l);
		taskENTER_CRITICAL();
		view.live = ok ? i : SAT_NONE;
		view.live_ok = ok;
		if (ok) {
			view.az10 = (int16_t)(l.az * 10.0f + 0.5f);
			view.el10 = (int16_t)(l.el * 10.0f + (l.el < 0.0f ? -0.5f : 0.5f));
			view.range_km = (uint16_t)(l.range_km > 65535.0f ? 65535.0f : l.range_km);
			view.rate_ms = (int16_t)(l.rate_kms * 1000.0f);
		}
		taskEXIT_CRITICAL();
		if (e.track != SAT_NONE) {
			if (ok)
				track_service(now, &l, vfo, rx_hz, tx_hz, transmitting);
			else
				track_end(SAT_TRK_OFF);
		}
		return;		/* one job a poll */
	}
	if (transmitting)
		return;
	if (looking && search_job(now, light ? 1 : SEARCH_EVALS))
		return;
	if (al.searching)
		alarm_job(now);
}
#endif
