/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus Tools > Map (map.h, docs/map.md): the 81 provinces of Turkey (Natural Earth, public domain) and the 973
 * districts (GeoNames, CC BY 4.0) with the GPS position, the saved waypoints (nav.h), the APRS stations heard with a
 * position, the built-in repeaters (rpt.h, CONFIG_REPEATERS) and our track (mapnav.h, CONFIG_MAP_TRACK) drawn on top,
 * and the province (il) and district (ilce) the position is in under the map. Up / Down / Left / Right (or 2 / 8 /
 * 4 / 6): pan, the knob or 1 / 3: zoom out / in, OK: follow the GPS again (panning stops following).
 *   #: the repeater nearest the map's centre (# again: the next nearest), its callsign, frequency and way from the
 *      centre under the map; OK then opens its details in Tools > Repeaters (Back returns here), Back drops the pick.
 *   0: the APRS station nearest the centre (0 again: the next), "TA1ABC-9 4.2 km NE 3m ago" under the map; OK: its
 *      details there (symbol, altitude, comment), OK again: a message to it (CONFIG_APRS_MSG), Back: the way again,
 *      Back again drops the pick. # / 0 switch between repeaters and stations.
 *   9: Back to start: the track's first point saved as a waypoint and Tools > Go-To opened on it. 7 held: the track
 *      cleared (7 alone says so).
 * The stations are upstream's heard list (no copy): a dot each, the callsign from zoom 3 (no two labels overlap).
 *
 * Zoom 0..6. Province names from zoom 2, district dots from zoom 3 and their names from zoom 4; no two labels overlap
 * (map_occ_t: the first drawn wins, provinces first, then stations). */
#include "config.h"
#if CONFIG_MAP
#include <stdlib.h>
#include <string.h>
#include "ui_nav.h"
#include "ui_screens.h"
#include "ui_svc.h"
#include "ui_svc_aprs.h"
#include "fmt.h"
#include "i18n.h"
#include "gfx.h"
#include "aprs/aprs_fmt.h"
#include "ui/map.h"
#include "ui/mapnav.h"
#include "ui/nav.h"
#if CONFIG_REPEATERS
#include "ui/rpt.h"
#endif

#define PROVINCE_NEAR_M		3000u	/* a fix this close to the simplified shore still names the province */
#define DISTRICT_NEAR_M		30000u	/* the nearest district centre in the province, at most this far */
#define ZOOM_PROV_LABELS	2u
#define ZOOM_DISTRICTS		3u	/* dots */
#define ZOOM_STATION_LABELS	3u	/* APRS callsigns */
#define ZOOM_DISTRICT_LABELS	4u

/* Static RAM: about 40 bytes */
static map_view_t view;
static bool follow = true;
static bool ready;
static bool details;		/* the picked station's details under the map (OK) */
static uint8_t zoom;		/* 0: all of Turkey */
static int8_t here_province = MAP_NONE;
static uint16_t spick;		/* the picked APRS station (0), its callsign's hash, 0 none */
#if CONFIG_REPEATERS
static uint16_t pick = RPT_NONE;	/* the picked repeater (#), RPT_NONE none */
#endif

typedef struct {
	int8_t hi;		/* province drawn brighter (the one we are in), MAP_NONE for none */
} draw_ctx_t;

typedef struct {
	map_occ_t *occ;
	bool labels;
} dist_ctx_t;

static void line(int x0, int y0, int x1, int y1, color_t c, uint8_t dots)
{
	int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, e = dx + dy;
	for (unsigned k = 0;; k++) {
		if (dots == 0 || k % dots == 0)
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

static void border(void *ctx, int x0, int y0, int x1, int y1, uint8_t province)
{
	const draw_ctx_t *d = ctx;
	line(x0, y0, x1, y1, d->hi == (int8_t)province ? ui_col.fg : ui_col.line, 0);
}

static bool inside_view(int x, int y)
{
	return x >= view.x && x < view.x + view.w && y >= view.y && y < view.y + view.h;
}

static void marker_cross(int x, int y, int r, color_t c)
{
	for (int i = -r; i <= r; i++) {
		if (inside_view(x + i, y))
			gfx_pixel(x + i, y, c);
		if (inside_view(x, y + i))
			gfx_pixel(x, y + i, c);
	}
}

static void marker_box(int x, int y, color_t c)
{
	if (inside_view(x - 2, y - 2) && inside_view(x + 2, y + 2))
		gfx_rect(x - 2, y - 2, 5, 5, c);
}

/* A filled square 2r + 1 wide at (x, y), when it is inside the view (an APRS station) */
static void marker_dot(int x, int y, int r, color_t c)
{
	if (inside_view(x - r, y - r) && inside_view(x + r, y + r))
		gfx_fill(x - r, y - r, 2 * r + 1, 2 * r + 1, c);
}

/* A filled triangle pointing up, its tip r pixels above (x, y) (a repeater) */
static void marker_triangle(int x, int y, int r, color_t c)
{
	for (int i = 0; i <= 2 * r; i++) {
		int yy = y - r + i, half = i / 2;
		for (int xx = x - half; xx <= x + half; xx++)
			if (inside_view(xx, yy))
				gfx_pixel(xx, yy, c);
	}
}

/* A label of font f at (x, y) (top-left) if it fits in the view and no other label is there */
static void label(map_occ_t *occ, const font_t *f, int x, int y, const char *s, color_t c)
{
	int w = gfx_text_width(f, s);
	if (x < view.x || x + w > view.x + view.w || y < view.y || y + f->h > view.y + view.h)
		return;
	if (map_occ_take(occ, x, y, w, f->h))
		gfx_text(f, x, y, s, c);
}

/* The picked marker's name right of it, in the foreground colour, when it fits in the view */
static void pick_label(int x, int y, const char *s)
{
	const font_t *t = ui_layout.tiny;
	if (y - t->h / 2 >= view.y && y + t->h < view.y + view.h && x + 5 + gfx_text_width(t, s) < view.x + view.w)
		gfx_text(t, x + 5, y - t->h / 2, s, ui_col.fg);
}

static void district(void *ctx, int x, int y, uint16_t d, const char *name)
{
	const dist_ctx_t *dc = ctx;
	(void)d;
	gfx_fill(x, y, 2, 2, ui_col.dim);
	if (dc->labels)
		label(dc->occ, ui_layout.tiny, x + 3, y - ui_layout.tiny->h / 2, name, ui_col.dim);
}

/* The viewport: under the title, above the info line and the hint */
static void layout(int top)
{
	const font_t *s = ui_layout.small;
	int bottom = ui_layout.hint_y - s->h - 2;
	if (!ready || view.y != top || view.h != bottom - top) {
		int32_t la = 0, lo = 0;
		if (ready)
			map_view_center(&view, &la, &lo);
		map_view_init(&view, 0, top, gfx_width(), bottom - top);
		if (ready)
			map_view_set(&view, la, lo, zoom);
		ready = true;
	}
}

/* The APRS stations' callsigns (not the picked one's: draw_stations writes it on top) */
static void __attribute__((noinline)) station_labels(map_occ_t *occ)
{
	aprs_station_t st;
	const font_t *t = ui_layout.tiny;
	for (uint8_t i = 0; i < ui_svc_aprs_station_count(); i++) {
		int x, y;
		if (ui_svc_aprs_station(i, &st) && st.has_pos && mapnav_call_hash(st.call) != spick &&
		    map_project(&view, st.lat, st.lon, &x, &y))
			label(occ, t, x + 3, y - t->h / 2, st.call, ui_col.rx);
	}
}

/* Province names, then the stations' callsigns, then the districts (dots, names): labels never overlap */
static void __attribute__((noinline)) draw_names(void)
{
	map_occ_t occ;
	map_occ_init(&occ, view.x, view.y, view.w, view.h);
	if (view.zoom >= ZOOM_PROV_LABELS) {
		color_t c = view.zoom >= ZOOM_DISTRICT_LABELS ? ui_col.fg : ui_col.dim;
		for (uint8_t i = 0; i < 81u; i++) {
			int x, y;
			if (!map_label(&view, i, &x, &y))
				continue;
			const char *name = map_province_name(i);
			label(&occ, ui_layout.tiny, x - gfx_text_width(ui_layout.tiny, name) / 2, y, name, c);
		}
	}
	if (view.zoom >= ZOOM_STATION_LABELS)
		station_labels(&occ);
	if (view.zoom >= ZOOM_DISTRICTS) {
		/* CC BY 4.0: the source of the district points, in the view's bottom right corner (a name: not translated) */
		const font_t *t = ui_layout.tiny;
		label(&occ, t, view.x + view.w - gfx_text_width(t, "GeoNames") - 1, view.y + view.h - t->h, "GeoNames",
		      ui_col.dim);
		dist_ctx_t dc = { .occ = &occ, .labels = view.zoom >= ZOOM_DISTRICT_LABELS };
		map_districts_draw(&view, district, &dc);
	}
}

#if CONFIG_MAP_TRACK
static void track_seg(void *ctx, int x0, int y0, int x1, int y1, uint8_t province)
{
	(void)ctx;
	(void)province;
	line(x0, y0, x1, y1, ui_col.tx, 2);	/* dotted */
}
#endif

#if CONFIG_REPEATERS
/* The built-in repeaters with a position: a triangle each, the picked one larger with its callsign */
static void __attribute__((noinline)) draw_repeaters(void)
{
	for (uint16_t i = 0; i < (uint16_t)rpt_info.count; i++) {
		const rpt_entry_t *e = &rpt_table[i];
		int x, y;
		if (i == pick || e->lat_e5 == RPT_NOPOS || e->lon_e5 == RPT_NOPOS ||
		    !map_project(&view, e->lat_e5 * 100, e->lon_e5 * 100, &x, &y))
			continue;
		marker_triangle(x, y, 2, ui_col.chnum);
	}
	if (pick < rpt_info.count) {
		const rpt_entry_t *e = &rpt_table[pick];
		int x, y;
		if (e->lat_e5 != RPT_NOPOS && e->lon_e5 != RPT_NOPOS &&
		    map_project(&view, e->lat_e5 * 100, e->lon_e5 * 100, &x, &y)) {
			marker_triangle(x, y, 4, ui_col.fg);
			pick_label(x, y, e->name);
		}
	}
}

/* "TA1ABC 439.1625 12.3 km NE" (the way from the map's centre) */
static void pick_line(sbuf_t *b)
{
	const rpt_entry_t *e = &rpt_table[pick];
	int32_t la, lo;
	uint32_t m;
	uint16_t brg;
	map_view_center(&view, &la, &lo);
	sb_str(b, e->name);
	sb_chr(b, ' ');
	sb_freq(b, e->rx_hz);
	if (rpt_way(e, la, lo, &m, &brg)) {
		sb_chr(b, ' ');
		sb_range(b, m, brg);
	}
}

/* # : the repeater nearest the map's centre, again: the next nearest (round from the last to the nearest) */
static void pick_next(void)
{
	rpt_here_t h;
	int32_t la, lo;
	map_view_center(&view, &la, &lo);
	rpt_here_set(&h, true, la, lo);
	uint16_t n = pick < rpt_info.count ? rpt_next(&h, pick, 1) : RPT_NONE;
	pick = n != RPT_NONE ? n : rpt_nearest(&h);
	spick = 0;
	details = false;
}
#endif

/* APRS stations heard with a position, then waypoints, then the track and us on top: the picked station larger with
 * its callsign */
static void __attribute__((noinline)) draw_marks(bool here, const ui_position_t *p)
{
	aprs_station_t st;
	int x, y;
	for (uint8_t i = 0; i < ui_svc_aprs_station_count(); i++) {
		if (!ui_svc_aprs_station(i, &st) || !st.has_pos || !map_project(&view, st.lat, st.lon, &x, &y))
			continue;
		if (spick != 0 && mapnav_call_hash(st.call) == spick) {
			marker_dot(x, y, 2, ui_col.fg);
			pick_label(x, y, st.call);
		} else {
			marker_dot(x, y, 1, ui_col.rx);
		}
	}
	nav_point_t wp;
	for (uint8_t i = 0; i < nav_count(); i++) {
		if (nav_get(i, &wp) && map_project(&view, wp.lat_e7, wp.lon_e7, &x, &y)) {
			marker_box(x, y, ui_col.warn);
			if (view.zoom >= 3 && y + 3 + ui_layout.tiny->h < view.y + view.h)
				gfx_text(ui_layout.tiny, x + 4, y - ui_layout.tiny->h / 2, wp.name, ui_col.warn);
		}
	}
#if CONFIG_MAP_TRACK
	track_draw(&view, &ui_mapnav_track, track_seg, NULL);
#endif
	if (here && map_project(&view, p->lat_e7, p->lon_e7, &x, &y))
		marker_cross(x, y, 3, ui_col.tx);
	if (!here || !follow)	/* the centre: what # and 0 measure from */
		marker_cross(view.x + view.w / 2, view.y + view.h / 2, 2, ui_col.dim);
}

/* The picked station: "TA1ABC-9 4.2 km NE 3m ago" (the way from the map's centre), or its details "/> 120 m comment"
 * (symbol, altitude, the comment without its "/A="); false when it is no longer in the list (the pick is dropped) */
static bool station_line(sbuf_t *b)
{
	aprs_station_t st;
	if (mapnav_station_find(ui_svc_aprs_station, ui_svc_aprs_station_count(), spick, &st) < 0) {
		spick = 0;
		details = false;
		return false;
	}
	if (details) {	/* the callsign is beside its mark */
		char c[sizeof st.text];
		sb_chr(b, st.sym_table);
		sb_chr(b, st.sym_code);
		if (st.alt_m != MAPNAV_ALT_NONE) {
			sb_chr(b, ' ');
			sb_i32(b, st.alt_m);
			sb_str(b, " m");
		}
		mapnav_comment(st.text, c, sizeof c);
		if (c[0]) {
			sb_chr(b, ' ');
			sb_str(b, c);
		}
	} else {
		int32_t la, lo;
		sb_str(b, st.call);
		sb_chr(b, ' ');
		map_view_center(&view, &la, &lo);
		sb_range(b, aprs_distance_m(la, lo, st.lat, st.lon), aprs_bearing_deg(la, lo, st.lat, st.lon));
		sb_chr(b, ' ');
		sb_age(b, ui_st.now_ms - st.heard_ms);	/* "3m ago", "3m" where that does not fit */
	}
	return true;
}

/* Under the map: the province (plate and name) and the nearest district, or why there is none */
static void here_line(sbuf_t *b, bool here, const ui_position_t *p)
{
	if (!here) {
		sb_str(b, TR("No GPS fix"));
	} else if (here_province == MAP_NONE) {
		sb_str(b, TR("Outside Turkey"));
	} else {
		sb_u32(b, map_province_plate((uint8_t)here_province), 2);
		sb_chr(b, ' ');
		sb_str(b, map_province_name((uint8_t)here_province));
		uint16_t d = map_district_near(p->lat_e7, p->lon_e7, here_province, DISTRICT_NEAR_M);
		if (d != MAP_DISTRICT_NONE && strcmp(map_district_name(d), map_province_name((uint8_t)here_province))) {
			sb_str(b, " / ");
			sb_str(b, map_district_name(d));
		}
	}
}

/* Under the map: the pick (station or repeater) on the whole line, else where we are and the scale on the right; the
 * hint */
static void __attribute__((noinline)) draw_info(bool here, const ui_position_t *p)
{
	char buf[48], fit[48];
	sbuf_t b;
	const font_t *s = ui_layout.small;
	int y = view.y + view.h + 1, w = gfx_width();
	sb_init(&b, buf, sizeof buf);
	color_t c = ui_col.fg;
	const char *hint;
	if (spick != 0 && station_line(&b)) {
#if CONFIG_APRS_MSG
		hint = details ? TR("OK message  0 next  Back") : TR("OK details  0 next  # repeater  Back clear");
#else
		hint = details ? TR("0 next  Back") : TR("OK details  0 next  # repeater  Back clear");
#endif
		if (details) {	/* call, symbol, altitude, comment: cut at the end */
			size_t max = (size_t)(w / s->w);
			if (strlen(buf) > max)
				buf[max] = '\0';
		}
	}
#if CONFIG_REPEATERS
	else if (pick < rpt_info.count) {
		pick_line(&b);
		hint = TR("OK details  # next  0 APRS  Back clear");
	}
#endif
	else {
		here_line(&b, here, p);
		c = here && here_province != MAP_NONE ? ui_col.fg : ui_col.warn;
#if CONFIG_REPEATERS
		hint = TR("# repeater  0 APRS  ^v<> pan  knob zoom  OK follow");
#else
		hint = TR("0 APRS  ^v<> pan  knob zoom  OK follow");
#endif
		char scale[12];
		sbuf_t sc;
		sb_init(&sc, scale, sizeof scale);
		sb_u32(&sc, (map_view_width_m(&view) + 500u) / 1000u, 1);
		sb_str(&sc, follow ? " km" : " km *");
		int sw = gfx_text_width(s, scale);
		gfx_text(s, w - sw, y, scale, ui_col.dim);
		w -= sw + s->w;
	}
	gfx_text_fit(s, buf, w, fit, sizeof fit);
	gfx_text(s, 0, y, fit, c);
	ui_draw_bottom(&ui_st, hint);
}

static void map_draw_screen(void)
{
	ui_position_t p;
	gfx_clear(ui_col.bg);
	int top = ui_draw_top(ui_view(), &ui_st, TR("Map"), "") + 1;
	layout(top);
	bool here = ui_svc_position(&p) && p.fix;
	if (here && follow) {
		map_view_set(&view, p.lat_e7, p.lon_e7, zoom);
	} else {
		int32_t la, lo;
		map_view_center(&view, &la, &lo);
		map_view_set(&view, la, lo, zoom);
	}
	here_province = here ? (int8_t)map_province_near(p.lat_e7, p.lon_e7, PROVINCE_NEAR_M) : (int8_t)MAP_NONE;

	draw_ctx_t d = { .hi = here_province };
	map_draw(&view, border, &d);
	draw_names();
#if CONFIG_REPEATERS
	draw_repeaters();
#endif
	draw_marks(here, &p);
	draw_info(here, &p);
	ui_draw_banner(&ui_st);
}

/* 0: the APRS station nearest the centre, again: the next */
static void station_next(void)
{
	int32_t la, lo;
	map_view_center(&view, &la, &lo);
	spick = mapnav_station_next(ui_svc_aprs_station, ui_svc_aprs_station_count(), la, lo, spick);
	details = false;
	if (spick == 0) {
		ui_toast(TR("No APRS station with a position"));
		return;
	}
#if CONFIG_REPEATERS
	pick = RPT_NONE;
#endif
}

/* OK on the picked station: its details, then (CONFIG_APRS_MSG) a message to it */
static void station_ok(void)
{
	aprs_station_t st;
	if (!details) {
		details = true;
		return;
	}
#if CONFIG_APRS_MSG
	if (mapnav_station_find(ui_svc_aprs_station, ui_svc_aprs_station_count(), spick, &st) >= 0)
		ui_aprsmsg_compose_to(st.call);
#else
	(void)st;
	details = false;
#endif
}

#if CONFIG_MAP_TRACK
/* 9: the track's first point as the waypoint START (once), and Tools > Go-To on it (target 0: the newest waypoint) */
static void back_to_start(void)
{
	int32_t la, lo;
	nav_point_t wp;
	if (!track_start(&ui_mapnav_track, &la, &lo)) {
		ui_toast(TR("No track yet"));
		return;
	}
	bool saved = !nav_get(0, &wp) || wp.lat_e7 != la || wp.lon_e7 != lo;
	if (saved) {
		nav_save(la, lo);
		nav_rename(0, "START");
	}
	ui_push(SCR_GOTO, 1);
	if (saved)
		ui_toast(TR("Start saved as a waypoint"));
}
#endif

static bool map_key(const input_event_t *ev)
{
	bool press = ev->gesture == GESTURE_PRESS || ev->gesture == GESTURE_REPEAT;
	int step_x = view.w / 4, step_y = view.h / 4;
	if (ev->action == ACT_ENC_STEP && ev->value != 0) {
		int z = (int)zoom + (ev->value > 0 ? 1 : -1);
		zoom = (uint8_t)(z < 0 ? 0 : z >= (int)MAP_ZOOMS ? (int)MAP_ZOOMS - 1 : z);
		ui_dirty();
		return true;
	}
#if CONFIG_MAP_TRACK
	if (ev->action == ACT_DIGIT_7 && ev->gesture == GESTURE_LONG) {
		track_clear(&ui_mapnav_track);
		ui_toast(TR("Track cleared"));
		ui_dirty();
		return true;
	}
#endif
	if (!press)
		return ev->action == ACT_SELECT && ev->gesture == GESTURE_LONG;
	int dx = 0, dy = 0;
	switch (ev->action) {
	case ACT_NAV_UP: case ACT_DIGIT_2: dy = -step_y; break;
	case ACT_NAV_DOWN: case ACT_DIGIT_8: dy = step_y; break;
	case ACT_NAV_LEFT: case ACT_DIGIT_4: dx = -step_x; break;
	case ACT_NAV_RIGHT: case ACT_DIGIT_6: dx = step_x; break;
	case ACT_DIGIT_1:
		if (zoom > 0)
			zoom--;
		ui_dirty();
		return true;
	case ACT_DIGIT_3:
		if (zoom + 1u < MAP_ZOOMS)
			zoom++;
		ui_dirty();
		return true;
	case ACT_DIGIT_0:
		if (ev->gesture == GESTURE_PRESS)
			station_next();
		ui_dirty();
		return true;
#if CONFIG_MAP_TRACK
	case ACT_DIGIT_7:
		if (ev->gesture == GESTURE_PRESS)
			ui_toast(TR("Hold 7 to clear the track"));
		return true;
	case ACT_DIGIT_9:
		if (ev->gesture == GESTURE_PRESS)
			back_to_start();
		return true;
#endif
#if CONFIG_REPEATERS
	case ACT_CONTEXT:
		if (ev->gesture != GESTURE_PRESS)
			return true;
		if (rpt_info.count == 0) {
			ui_toast(TR("No repeaters in this build"));
			return true;
		}
		pick_next();
		ui_dirty();
		return true;
#endif
	case ACT_BACK:
		if (ev->gesture != GESTURE_PRESS)
			return false;
		if (spick != 0) {
			if (details)
				details = false;
			else
				spick = 0;
		}
#if CONFIG_REPEATERS
		else if (pick < rpt_info.count)
			pick = RPT_NONE;
#endif
		else
			return false;
		ui_dirty();
		return true;
	case ACT_SELECT:
		if (ev->gesture != GESTURE_PRESS)
			return true;
		if (spick != 0) {
			station_ok();
			ui_dirty();
			return true;
		}
#if CONFIG_REPEATERS
		if (pick < rpt_info.count) {
			ui_rpt_open(pick);
			return true;
		}
#endif
		follow = true;
		ui_toast(TR("Following GPS"));
		ui_dirty();
		return true;
	default:
		return false;
	}
	map_view_pan(&view, dx, dy);
	follow = false;
	ui_dirty();
	return true;
}

static void map_enter(void)
{
	ui_dirty();
}

const ui_scr_def_t ui_map_screen = {
	.name = "map", .flags = SF_FIELD, .key = map_key, .draw = map_draw_screen, .enter = map_enter,
};
#endif
