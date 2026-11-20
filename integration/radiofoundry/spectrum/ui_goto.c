/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus Tools > Go-To (nav.h): a compass rose with an arrow to the target, its distance, bearing and the time to
 * get there. Targets: the saved waypoints, then every APRS station heard with a position. North is up while
 * standing; heading up (the GPS course) while moving. Up / Down: target, OK: save here as a waypoint, OK held:
 * delete the waypoint shown. Opened with arg n (Tools > Map's "Back to start"): target n - 1 shown first.
 * CONFIG_NAV_FOLLOW (ui/follow.h): opened with FOLLOW_GOTO_ARG it follows one APRS station (Tools > Map's 5, Tools >
 * APRS > Stations heard's OK): its newest position heard, its age, "stale" from 10 minutes. Up / Down (another
 * target), OK (a new waypoint) or Back ends following. CONFIG_LOCATOR: # opens the locator calculator. */
#include "config.h"
#if CONFIG_NAV
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "ui_nav.h"
#include "ui_screens.h"
#include "ui_svc.h"
#include "ui_svc_aprs.h"
#include "fmt.h"
#include "i18n.h"
#include "gfx.h"
#include "ui/nav.h"
#if CONFIG_NAV_FOLLOW
#include "ui/follow.h"
#endif

static uint16_t sel;
#if CONFIG_NAV_FOLLOW
static follow_t fw;		/* the station followed (call "" = none) */
static uint32_t fw_polled_ms;

/* The station's newest from the APRS engine's heard list (no copy of the list) */
static bool __attribute__((noinline)) follow_poll(void)
{
	aprs_station_t s;
	bool changed = false;
	for (uint8_t i = 0; i < ui_svc_aprs_station_count(); i++)
		if (ui_svc_aprs_station(i, &s))
			changed |= follow_update(&fw, s.call, s.has_pos, s.lat, s.lon, s.heard_ms);
	return changed;
}

void ui_follow_start(const char *call)
{
	follow_set(&fw, call);
	follow_poll();
	fw_polled_ms = ui_st.now_ms;
	ui_push(SCR_GOTO, FOLLOW_GOTO_ARG);
}

bool ui_follow_tick(uint8_t scr, uint32_t now_ms)
{
	if (!follow_on(&fw) || now_ms - fw_polled_ms < FOLLOW_POLL_MS)
		return false;
	fw_polled_ms = now_ms;
	follow_poll();
	return scr == SCR_GOTO;	/* the age moves on: draw again every second */
}

static void follow_end(bool say)
{
	if (!follow_on(&fw))
		return;
	follow_clear(&fw);
	if (say)
		ui_toast(TR("Following ended"));
}
#endif

typedef struct {
	char name[16];
	int32_t lat, lon;
	bool waypoint;
} target_t;

static uint16_t target_count(void)
{
	uint16_t n = nav_count();
	aprs_station_t s;
	for (uint8_t i = 0; i < ui_svc_aprs_station_count(); i++)
		if (ui_svc_aprs_station(i, &s) && s.has_pos)
			n++;
	return n;
}

static bool target_get(uint16_t want, target_t *t)
{
	nav_point_t p;
	if (want < nav_count() && nav_get((uint8_t)want, &p)) {
		memcpy(t->name, p.name, sizeof p.name);
		t->lat = p.lat_e7;
		t->lon = p.lon_e7;
		t->waypoint = true;
		return true;
	}
	uint16_t k = nav_count();
	aprs_station_t s;
	for (uint8_t i = 0; i < ui_svc_aprs_station_count(); i++) {
		if (!ui_svc_aprs_station(i, &s) || !s.has_pos)
			continue;
		if (k++ == want) {
			str_copy(t->name, sizeof t->name, s.call);
			t->lat = s.lat;
			t->lon = s.lon;
			t->waypoint = false;
			return true;
		}
	}
	return false;
}

static void line(int x0, int y0, int x1, int y1, color_t c)
{
	int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, e = dx + dy;
	for (;;) {
		gfx_pixel(x0, y0, c);
		if (x0 == x1 && y0 == y1)
			break;
		int e2 = 2 * e;
		if (e2 >= dy) {
			e += dy;
			x0 += sx;
		}
		if (e2 <= dx) {
			e += dx;
			y0 += sy;
		}
	}
}

/* The rose: a ring, N where north is, the arrow to the target (deg clockwise from the top of the screen) */
static void rose(int cx, int cy, int r, int north_deg, int arrow_deg, bool have)
{
	for (int a = 0; a < 360; a += 4) {
		float t = (float)a * 0.017453292f;
		gfx_pixel(cx + (int)lrintf((float)r * sinf(t)), cy - (int)lrintf((float)r * cosf(t)), ui_col.line);
	}
	float n = (float)north_deg * 0.017453292f;
	const font_t *f = ui_layout.small;
	int nx = cx + (int)lrintf((float)(r + f->h / 2 + 2) * sinf(n)), ny = cy - (int)lrintf((float)(r + f->h / 2 + 2) * cosf(n));
	gfx_text(f, nx - f->w / 2, ny - f->h / 2, "N", ui_col.warn);
	if (!have)
		return;
	float a = (float)arrow_deg * 0.017453292f;
	int tx = cx + (int)lrintf((float)(r - 3) * sinf(a)), ty = cy - (int)lrintf((float)(r - 3) * cosf(a));
	for (int w = -1; w <= 1; w++)
		line(cx + w, cy, tx + w, ty, ui_col.rx);
	float l = a + 2.6f, rr = a - 2.6f;	/* the head */
	line(tx, ty, tx + (int)lrintf(8.0f * sinf(l)), ty - (int)lrintf(8.0f * cosf(l)), ui_col.rx);
	line(tx, ty, tx + (int)lrintf(8.0f * sinf(rr)), ty - (int)lrintf(8.0f * cosf(rr)), ui_col.rx);
}

static void goto_draw(void)
{
	char buf[32];
	sbuf_t b;
	ui_position_t p;
	target_t t;
	gfx_clear(ui_col.bg);
	int y = ui_draw_top(ui_view(), &ui_st, TR("Go-To"), "") + 2;
	const font_t *s = ui_layout.small, *m = ui_layout.medium;
	int w = gfx_width();
	uint16_t n = target_count();
	if (sel >= n)
		sel = n ? (uint16_t)(n - 1u) : 0;
	bool here = ui_svc_position(&p) && p.fix;
	bool have = n != 0 && target_get(sel, &t);
	bool fol = false;
#if CONFIG_NAV_FOLLOW
	fol = follow_on(&fw);
	bool stale = fol && follow_stale(&fw, ui_st.now_ms);
	if (fol) {
		str_copy(t.name, sizeof t.name, fw.call);
		t.lat = fw.lat;
		t.lon = fw.lon;
		t.waypoint = false;
		have = fw.has_pos;
	}
#endif
	sb_init(&b, buf, sizeof buf);
	if (fol) {
		sb_str(&b, TR("Follow "));
		sb_str(&b, t.name);
	} else if (have) {
		sb_u32(&b, (uint32_t)sel + 1u, 1);
		sb_chr(&b, '/');
		sb_u32(&b, n, 1);
		sb_chr(&b, ' ');
		sb_str(&b, t.name);
	} else {
		sb_str(&b, TR("No targets: OK saves here"));
	}
#if CONFIG_NAV_FOLLOW
	if (fol)
		gfx_text(m, 0, y, buf, stale || !have ? ui_col.warn : ui_col.rx);
	else
#endif
	gfx_text(m, 0, y, buf, have && t.waypoint ? ui_col.fg : ui_col.rx);
	y += m->h + 2;
	nav_way_t way = { 0 };
	if (here && have)
		nav_way(p.lat_e7, p.lon_e7, t.lat, t.lon, p.speed_kmh, p.course, &way);
	int r = (ui_layout.hint_y - y - 3 * s->h - 12 - (fol ? s->h + 1 : 0)) / 2;
	if (r > 70)
		r = 70;
	if (r >= 16) {
		int north = way.moving ? -(int)(p.course % 360u) : 0;
		rose(w / 2, y + r + s->h, r, north, way.moving ? way.rel : way.bearing, here && have);
		y += 2 * r + s->h + 6;
	}
	sb_init(&b, buf, sizeof buf);
	if (!here) {
		sb_str(&b, TR("No GPS fix"));
		gfx_text(m, 0, y, buf, ui_col.warn);
		y += m->h + 1;
	} else if (have) {
		sb_range(&b, way.meters, way.bearing);
		sb_str(&b, "  ");
		sb_u32(&b, way.bearing, 1);
		sb_str(&b, GLYPH_DEGREE);
		gfx_text(m, 0, y, buf, ui_col.fg);
		y += m->h + 1;
		sb_init(&b, buf, sizeof buf);
		if (way.moving) {
			sb_str(&b, TR("ETA "));
			sb_mmss(&b, way.eta_s);
			sb_str(&b, "  ");
			sb_u32(&b, p.speed_kmh, 1);
			sb_str(&b, " km/h");
		} else {
			sb_str(&b, TR("North up (standing)"));
		}
		gfx_text(s, 0, y, buf, ui_col.dim);
		y += s->h + 1;
	}
	const char *hint = TR("^v target  OK save  hold del");
#if CONFIG_LOCATOR
	hint = TR(TEXT_ALT_OPEN "^v target  OK save  hold del  # locator" TEXT_ALT_NEXT "^v target  OK save  # locator"
		  TEXT_ALT_NEXT "OK save  # locator" TEXT_ALT_CLOSE);
#endif
#if CONFIG_NAV_FOLLOW
	if (fol) {
		/* "Heard 3m ago", "STALE: heard 12m ago" (10 minutes and more), or no position yet */
		sb_init(&b, buf, sizeof buf);
		if (!fw.has_pos) {
			sb_str(&b, TR("No position from it yet"));
		} else {
			sb_str(&b, stale ? TR("STALE: heard ") : TR("Heard "));
			sb_age_long(&b, follow_age_ms(&fw, ui_st.now_ms));
		}
		gfx_text(s, 0, y, buf, stale || !fw.has_pos ? ui_col.warn : ui_col.dim);
		hint = TR(TEXT_ALT_OPEN "Following: Back or ^v ends it" TEXT_ALT_NEXT "Back ends following" TEXT_ALT_CLOSE);
	}
#endif
	ui_draw_bottom(&ui_st, hint);
	ui_draw_banner(&ui_st);
}

static bool goto_key(const input_event_t *ev)
{
	bool press = ev->gesture == GESTURE_PRESS || ev->gesture == GESTURE_REPEAT;
	uint16_t n = target_count();
#if CONFIG_LOCATOR
	if (ev->action == ACT_CONTEXT && ev->gesture == GESTURE_PRESS) {
		ui_push(SCR_LOCATOR, 0);	/* the locator calculator; following goes on under it */
		return true;
	}
#endif
#if CONFIG_NAV_FOLLOW
	if (follow_on(&fw)) {
		if (ev->action == ACT_BACK && ev->gesture == GESTURE_PRESS) {
			follow_end(true);
			return false;	/* and the screen's own Back leaves */
		}
		if (ev->action == ACT_SELECT && ev->gesture == GESTURE_LONG)
			return true;	/* no waypoint shown to delete */
		if (press && (ev->action == ACT_NAV_UP || ev->action == ACT_NAV_DOWN || ev->action == ACT_ENC_STEP))
			follow_end(true);	/* another target: the list again, from where it was */
		else if (ev->action == ACT_SELECT && ev->gesture == GESTURE_PRESS)
			follow_end(false);	/* a new waypoint here becomes the target */
	}
#endif
	if (press && (ev->action == ACT_NAV_UP || ev->action == ACT_NAV_DOWN || ev->action == ACT_ENC_STEP)) {
		int dir = ev->action == ACT_ENC_STEP ? (ev->value > 0 ? 1 : -1) : ev->action == ACT_NAV_DOWN ? 1 : -1;
		if (n)
			sel = (uint16_t)((sel + n + dir) % n);
		ui_dirty();
		return true;
	}
	if (ev->action == ACT_SELECT && ev->gesture == GESTURE_LONG) {
		target_t t;
		if (sel < n && target_get(sel, &t) && t.waypoint && nav_delete((uint8_t)sel))
			ui_toast(TR("Waypoint deleted"));
		ui_dirty();
		return true;
	}
	if (ev->action == ACT_SELECT && ev->gesture == GESTURE_PRESS) {
		ui_position_t p;
		if (ui_svc_position(&p) && p.fix) {
			nav_save(p.lat_e7, p.lon_e7);
			sel = 0;
			ui_toast(TR("Saved here"));
		} else {
			ui_toast(TR("No GPS fix"));
		}
		ui_dirty();
		return true;
	}
	return false;
}

static void goto_enter(void)
{
	uint16_t arg = ui_frame()->arg;	/* 0: as it was; n: target n - 1 (Tools > Map's "Back to start": 1, the newest) */
#if CONFIG_NAV_FOLLOW
	if (arg == FOLLOW_GOTO_ARG) {	/* ui_follow_start: the station set is the target */
		ui_dirty();
		return;
	}
	follow_end(false);	/* opened any other way: not following */
#endif
	if (arg != 0)
		sel = (uint16_t)(arg - 1u);
	ui_dirty();
}

const ui_scr_def_t ui_goto_screen = {
	.name = "goto", .flags = SF_FIELD, .key = goto_key, .draw = goto_draw, .enter = goto_enter,
};
#endif
