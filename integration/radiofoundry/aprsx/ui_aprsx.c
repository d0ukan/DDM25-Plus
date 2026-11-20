/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus APRS station page (ui_aprsx.h, docs/plus.md round 14 item 49). Installed as core/ui/ui_aprsx.c.
 * OK on a station in Tools > APRS > Stations heard opens it: the callsign and its symbol by name, the distance and
 * direction from us (our GPS position), its position (degrees, and the Maidenhead locator with CONFIG_LOCATOR), its
 * altitude, speed and heading when it sent them, its comment, when it was last heard (direct or through a
 * digipeater) and how many packets; then the last messages from it in our inbox and the one we sent it, newest
 * first, as many as fit. OK: Go-To follows it (ui_follow_start, as the list's OK did before), 0: Tools > Map centred
 * on it with it picked, #: a message to it (CONFIG_APRS_MSG), Up / Down: the station before / after it in the list.
 * The page names the station by its callsign's hash (mapnav.h, 2 B of RAM) and looks it up in the engine's list at
 * each draw (no copy kept); a station that drops out of the list says so. Its copies live in the tool screens'
 * shared buffer (ui_scratch.h) during a draw or a key: no other static RAM, and the UI task's stack stays light. */
#include "config.h"
#if CONFIG_APRS_STATION
#include <string.h>
#include "ui_nav.h"
#include "ui_screens.h"
#include "ui_svc.h"
#include "ui_svc_aprs.h"
#include "fmt.h"
#include "i18n.h"
#include "gfx.h"
#include "aprs/aprs_fmt.h"
#include "ui/mapnav.h"
#include "ui/follow.h"
#include "ui/ui_scratch.h"
#include "ui/ui_aprsx.h"
#if CONFIG_LOCATOR
#include "ui/locator.h"
#endif
#if CONFIG_APRS_MSG
#include "aprs/aprs_plus.h"
#endif

#define SX	(ui_scratch.aprsx)

static uint16_t hash;		/* the station shown, mapnav_call_hash of its callsign */
static uint32_t ticked;		/* the last tick that asked for a draw */

/* APRS101 appendix 2: the primary table's common symbols by code; on the alternate table and with an overlay the
 * same name only for the codes in ALT_CODES (the digipeater, gateway, house, car and weather station there) */
static const char sym_codes[] = "!#&'-<>OPRUXY[^_abfhjklrsuv";
static const char *const sym_names[] = { TRN("Police"), TRN("Digipeater"), TRN("Gateway"), TRN("Aircraft"),
					 TRN("House"), TRN("Motorcycle"), TRN("Car"), TRN("Balloon"), TRN("Police"),
					 TRN("Camper"), TRN("Bus"), TRN("Helicopter"), TRN("Boat"), TRN("Person"),
					 TRN("Aircraft"), TRN("Weather"), TRN("Ambulance"), TRN("Bicycle"),
					 TRN("Fire truck"), TRN("Hospital"), TRN("Jeep"), TRN("Truck"), TRN("Laptop"),
					 TRN("Repeater"), TRN("Boat"), TRN("Truck"), TRN("Van") };
_Static_assert(sizeof sym_codes - 1u == sizeof sym_names / sizeof sym_names[0], "one name per code");
#define ALT_CODES	"#&->_"

const char *ui_aprsx_symbol(char table, char code)
{
	const char *p = code != '\0' ? strchr(sym_codes, code) : NULL;
	if (p == NULL || (table != '/' && strchr(ALT_CODES, code) == NULL))
		return NULL;
	return tr(sym_names[p - sym_codes]);
}

/* The station's place in the heard list, a copy in the shared buffer; -1 when it is no longer there (one without a
 * position too: mapnav_station_find skips those) */
static int find_index(void)
{
	ui_scratch_take(UI_SCRATCH_APRSX);
	for (uint8_t i = 0; hash != 0 && i < ui_svc_aprs_station_count(); i++)
		if (ui_svc_aprs_station(i, &SX.st) && mapnav_call_hash(SX.st.call) == hash)
			return i;
	return -1;
}

static bool find(void)
{
	return find_index() >= 0;
}

void ui_aprsx_open(const char *call)
{
	hash = mapnav_call_hash(call);
	ui_push(SCR_APRS_STATION, 0);
}

bool ui_aprsx_tick(uint8_t scr, uint32_t now_ms)
{
	if (scr != SCR_APRS_STATION || now_ms - ticked < 1000u)
		return false;
	ticked = now_ms;
	return true;
}

/* One line in the small font, cut to the screen's width; the next line's y */
static int line(int y, const char *s, color_t c)
{
	const font_t *f = ui_layout.small;
	gfx_text_fit(f, s, gfx_width(), SX.fit, sizeof SX.fit);
	gfx_text(f, 0, y, SX.fit, c);
	return y + f->h + 2;
}

/* The way to it from our GPS position: "4.2 km NE", or why there is none */
static void way(sbuf_t *b, const aprs_station_t *st)
{
	ui_position_t p;
	if (!st->has_pos)
		sb_str(b, TR("No position heard"));
	else if (!ui_svc_position(&p) || !p.fix)
		sb_str(b, TR("No GPS fix"));
	else
		sb_range(b, aprs_distance_m(p.lat_e7, p.lon_e7, st->lat, st->lon),
			 aprs_bearing_deg(p.lat_e7, p.lon_e7, st->lat, st->lon));
}

#if CONFIG_APRS_MSG
/* The last messages from it (our inbox, newest first) and the one we sent it, as many lines as fit above the hint */
static void __attribute__((noinline)) messages(int y, const char *call)
{
	const font_t *s = ui_layout.small;
	int bottom = ui_layout.hint_y - s->h;
	sbuf_t b;
	const char *text;
	const char *to = ui_aprsmsg_sent(&text);
	gfx_hline(0, y, gfx_width(), ui_col.line);
	y += 3;
	bool any = false;
	if (to != NULL && !strcmp(to, call) && y <= bottom) {	/* ours: "> text (delivered)" */
		aprs_plus_t ps;
		aprs_plus_state(&ps);
		sb_init(&b, SX.line, sizeof SX.line);
		sb_str(&b, "> ");
		sb_str(&b, text);
		sb_str(&b, ps.out == APRS_OUT_DELIVERED ? TR(" (delivered)") : ps.out == APRS_OUT_FAILED ?
			   TR(" (not delivered)") : ps.out == APRS_OUT_SENDING ? TR(" (sending)") : "");
		y = line(y, SX.line, ui_col.dim);
		any = true;
	}
#if CONFIG_APRS_RX
	for (uint8_t i = 0; i < aprs_msg_count() && y <= bottom; i++) {	/* theirs: "< text", unread ones marked */
		if (!aprs_msg_peek(i, &SX.msg) || strcmp(SX.msg.from, call))
			continue;
		sb_init(&b, SX.line, sizeof SX.line);
		sb_str(&b, "< ");
		sb_str(&b, SX.msg.text);
		y = line(y, SX.line, SX.msg.read ? ui_col.fg : ui_col.warn);
		any = true;
	}
#endif
	if (!any && y <= bottom)
		line(y, TR("No messages with it"), ui_col.dim);
}
#endif

static void __attribute__((noinline)) details(int y)
{
	const font_t *md = ui_layout.medium;
	const aprs_station_t *st = &SX.st;
	sbuf_t b;
	gfx_text(md, 0, y, st->call, ui_col.fg);
	y += md->h + 2;
	/* "Car /> 4.2 km NE": the symbol and the way to it */
	sb_init(&b, SX.line, sizeof SX.line);
	const char *sym = ui_aprsx_symbol(st->sym_table, st->sym_code);
	if (sym != NULL) {
		sb_str(&b, sym);
		sb_chr(&b, ' ');
	}
	if (st->has_pos) {
		sb_chr(&b, st->sym_table);
		sb_chr(&b, st->sym_code);
		sb_str(&b, "  ");
	}
	way(&b, st);
	y = line(y, SX.line, ui_col.fg);
	if (st->has_pos) {
		/* "39.9208 32.8541 KM69kw" */
		sb_init(&b, SX.line, sizeof SX.line);
		sb_degrees(&b, st->lat);
		sb_chr(&b, ' ');
		sb_degrees(&b, st->lon);
#if CONFIG_LOCATOR
		char loc[LOC_LEN_MAX + 1];
		loc_encode(st->lat, st->lon, loc);
		sb_chr(&b, ' ');
		sb_str(&b, loc);
#endif
		y = line(y, SX.line, ui_col.dim);
		/* "120 m  35 km/h W": what it sent of these */
		sb_init(&b, SX.line, sizeof SX.line);
		if (st->alt_m != MAPNAV_ALT_NONE) {
			sb_i32(&b, st->alt_m);
			sb_str(&b, " m  ");
		}
		if (st->speed_kn != APRS_SPEED_NONE) {
			sb_u32(&b, ((uint32_t)st->speed_kn * 1852u + 500u) / 1000u, 1);
			sb_str(&b, " km/h ");
		}
		if (st->course != APRS_COURSE_NONE && st->course != 0)
			sb_compass(&b, st->course);
		if (SX.line[0] != '\0')
			y = line(y, SX.line, ui_col.dim);
	}
	/* The comment, without its altitude ("/A=") */
	mapnav_comment(st->text, SX.line, sizeof SX.line);
	if (SX.line[0] != '\0')
		y = line(y, SX.line, ui_col.fg);
	/* "Heard: 3m ago", "Direct, 5 packets" */
	sb_init(&b, SX.line, sizeof SX.line);
	sb_str(&b, TR("Heard: "));
	sb_age_long(&b, ui_st.now_ms - st->heard_ms);
	y = line(y, SX.line, ui_col.dim);
	sb_init(&b, SX.line, sizeof SX.line);
	sb_str(&b, st->direct ? TR("Direct, ") : TR("Via digi, "));
	sb_u32(&b, st->count, 1);
	sb_str(&b, st->count == 1u ? TR(" packet") : TR(" packets"));
	y = line(y, SX.line, ui_col.dim);
#if CONFIG_APRS_MSG
	messages(y + 2, st->call);
#endif
}

static void station_draw(void)
{
	gfx_clear(ui_col.bg);
	int y = ui_draw_top(ui_view(), &ui_st, "APRS", "") + 4;
	const char *hint = TR("Back");
	if (find()) {
		details(y);
#if CONFIG_APRS_MSG
		hint = TR(TEXT_ALT_OPEN "OK Go-To  0 map  # message" TEXT_ALT_NEXT "OK Go-To  0 map  # msg" TEXT_ALT_NEXT
			  "OK Go-To  0 map" TEXT_ALT_CLOSE);
#else
		hint = TR(TEXT_ALT_OPEN "OK Go-To  0 map  Back" TEXT_ALT_NEXT "OK Go-To  0 map" TEXT_ALT_CLOSE);
#endif
	} else {
		gfx_text(ui_layout.medium, 0, y, TR("Not heard any more"), ui_col.warn);
	}
	ui_draw_bottom(&ui_st, hint);
	ui_draw_banner(&ui_st);
}

/* Up / Down: the station before / after this one in the heard list (round) */
static void step(int dir)
{
	uint8_t n = ui_svc_aprs_station_count();
	int i = find_index();
	if (n == 0)
		return;
	i = i < 0 ? 0 : (i + dir + n) % n;
	if (ui_svc_aprs_station((uint8_t)i, &SX.st))
		hash = mapnav_call_hash(SX.st.call);
}

static bool station_key(const input_event_t *ev)
{
	bool press = ev->gesture == GESTURE_PRESS;
	if (ev->action == ACT_NAV_UP || ev->action == ACT_NAV_DOWN) {
		if (press || ev->gesture == GESTURE_REPEAT) {
			step(ev->action == ACT_NAV_UP ? -1 : 1);
			ui_dirty();
		}
		return true;
	}
	if (ev->action != ACT_SELECT && ev->action != ACT_DIGIT_0 && ev->action != ACT_CONTEXT)
		return false;
	if (!press)
		return true;
	if (!find())
		return true;
	if (ev->action == ACT_SELECT) {
		ui_follow_start(SX.st.call);	/* Go-To follows it */
	} else if (ev->action == ACT_DIGIT_0) {
		if (SX.st.has_pos)
			ui_map_show_station(SX.st.call);
		else
			ui_toast(TR("No position heard"));
	}
#if CONFIG_APRS_MSG
	else {
		ui_aprsmsg_compose_to(SX.st.call);
	}
#endif
	return true;
}

static void station_enter(void)
{
	ui_dirty();
}

const ui_scr_def_t ui_aprsx_screen = {
	.name = "aprs-station", .flags = SF_FIELD, .key = station_key, .draw = station_draw, .enter = station_enter,
};
#endif
