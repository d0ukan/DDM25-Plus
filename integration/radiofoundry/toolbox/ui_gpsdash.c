/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus Tools > GPS (tripm.h, docs/plus.md "GPS dashboard"): the speed large, the heading as a compass point and
 * degrees (while moving), the altitude, the trip distance and the top speed since the reset, the fix and the
 * satellites used. OK held resets the trip and the top speed. The trip counts whatever screen is open: the GPS is
 * read once a second (ui_gpsdash_tick, from ui_app.c). RAM only (the trip meter's 20 bytes and the poll time).
 * Installed as core/ui/ui_gpsdash.c. */
#include "config.h"
#if CONFIG_GPS_DASH
#include <string.h>
#include "ui_nav.h"
#include "ui_screens.h"
#include "ui_svc.h"
#include "fmt.h"
#include "i18n.h"
#include "gfx.h"
#include "font.h"
#include "ui/tripm.h"
#include "ui/ui_gpsdash.h"

#define POLL_MS		1000u

static trip_t trip;
static uint32_t polled_ms;

bool ui_gpsdash_tick(uint8_t top, uint32_t now_ms)
{
	if (now_ms - polled_ms < POLL_MS)
		return false;
	polled_ms = now_ms;
	ui_position_t p;
	if (!ui_svc_position(&p))
		return false;
	trip_feed(&trip, p.fix, p.lat_e7, p.lon_e7, p.speed_kmh, now_ms);
	return top == SCR_GPSDASH;
}

const trip_t *ui_gpsdash_trip(void)
{
	return &trip;
}

/* "850 m", "1.25 km", "125.4 km" */
static void sb_dist(sbuf_t *b, uint32_t m)
{
	if (m < 1000u) {
		sb_u32(b, m, 1);
		sb_str(b, " m");
	} else if (m < 100000u) {
		sb_u32(b, m / 1000u, 1);
		sb_chr(b, '.');
		sb_u32(b, (m % 1000u) / 10u, 2);
		sb_str(b, " km");
	} else {
		sb_u32(b, m / 1000u, 1);
		sb_chr(b, '.');
		sb_u32(b, (m % 1000u) / 100u, 1);
		sb_str(b, " km");
	}
}

/* "Label value" with the label dim; returns the next row's y */
static int row(int y, const char *label, const char *value, color_t c)
{
	const font_t *s = ui_layout.small;
	int x = gfx_text(s, 0, y, label, ui_col.dim);
	gfx_text(s, x + s->w, y, value, c);
	return y + s->h + 3;
}

static void __attribute__((noinline)) dash_draw(void)
{
	ui_position_t p;
	char buf[24];
	sbuf_t b;
	gfx_clear(ui_col.bg);
	int y = ui_draw_top(ui_view(), &ui_st, "GPS", "") + 4;
	bool have = ui_svc_position(&p);
	bool fix = have && p.fix;
	const font_t *L = ui_layout.large, *s = ui_layout.small;
	/* The speed, large */
	sb_init(&b, buf, sizeof buf);
	if (fix && p.speed_kmh != 0xFFFFu)
		sb_u32(&b, p.speed_kmh, 1);
	else
		sb_chr(&b, '-');
	int x = gfx_text(L, 0, y, buf, fix ? ui_col.fg : ui_col.dim);
	gfx_text(s, x + s->w, y + L->h - s->h, "km/h", ui_col.dim);
	y += L->h + 6;
	/* Heading: "NE 45°" while moving */
	sb_init(&b, buf, sizeof buf);
	if (fix && p.course != 0xFFFFu && p.speed_kmh != 0xFFFFu && p.speed_kmh >= TRIP_MOVING_KMH) {
		sb_str(&b, trip_compass16(p.course));
		sb_chr(&b, ' ');
		sb_u32(&b, p.course % 360u, 1);
		sb_str(&b, GLYPH_DEGREE);
	} else {
		sb_chr(&b, '-');
	}
	y = row(y, TR("Heading"), buf, ui_col.fg);
	sb_init(&b, buf, sizeof buf);
	if (fix && p.alt_ok) {
		sb_i32(&b, p.alt_m);
		sb_str(&b, " m");
	} else {
		sb_chr(&b, '-');
	}
	y = row(y, TR("Altitude"), buf, ui_col.fg);
	sb_init(&b, buf, sizeof buf);
	sb_dist(&b, trip.meters);
	y = row(y, TR("Trip"), buf, ui_col.fg);
	sb_init(&b, buf, sizeof buf);
	sb_u32(&b, trip.max_kmh, 1);
	sb_str(&b, " km/h");
	y = row(y, TR("Top speed"), buf, ui_col.fg);
	/* The fix: "Fix 3D, 8 satellites" */
	sb_init(&b, buf, sizeof buf);
	if (!have) {
		sb_str(&b, TR("GPS off"));
	} else if (!fix) {
		sb_str(&b, TR("No fix"));
	} else {
		sb_str(&b, TR("Fix"));
		sb_str(&b, p.mode == 2u ? " 2D" : p.mode == 3u ? " 3D" : "");
		sb_str(&b, ", ");
		sb_u32(&b, p.sats, 1);
		sb_str(&b, TR(" satellites"));
	}
	gfx_text(s, 0, y + 2, buf, fix ? ui_col.rx : ui_col.warn);
	ui_draw_bottom(&ui_st, TR("Hold OK reset  Back"));
	ui_draw_banner(&ui_st);
}

static bool dash_key(const input_event_t *ev)
{
	if (ev->action == ACT_SELECT && ev->gesture == GESTURE_LONG) {
		trip_reset(&trip);
		ui_toast(TR("Trip reset"));
		ui_dirty();
		return true;
	}
	if (ev->action == ACT_SELECT && ev->gesture == GESTURE_PRESS) {
		ui_toast(TR("Hold OK to reset the trip"));
		return true;
	}
	return false;
}

static void dash_enter(void)
{
	ui_dirty();
}

const ui_scr_def_t ui_gpsdash_screen = {
	.name = "gpsdash", .flags = SF_FIELD, .key = dash_key, .draw = dash_draw, .enter = dash_enter,
};
#endif
