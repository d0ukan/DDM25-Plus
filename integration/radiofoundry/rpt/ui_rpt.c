/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus Tools > Repeaters (rpt.h): the built-in repeater list, nearest first from the position APRS uses (GPS,
 * else the computer's, else the fixed one; aprs_position_now) with the distance and compass direction, or by
 * frequency without a position. Up / Down: pick, OK: the details, #: the nearest one's details. In the details:
 * Up / Down: the next / previous, OK: tune the VFO to it (RX, TX = RX + offset, FM with the access tone and its
 * bandwidth, or DMR with its colour code and slot; FM and DMR only), Back: the list. The order is walked, never
 * stored (rpt_next): the screen keeps only the picked and the top row's entries. Opened from Tools > Map
 * (ui_rpt_open, the frame's arg is the entry + 1): that entry's details, Back returns to the map. The auto repeater's
 * offer (ui_mapnav.c, # on its banner) tunes through the same tune (ui_rpt_tune).
 *
 * By province (CONFIG_RPT_PROV): from Tools the screen opens on a first level, "All" (the whole list as above), then
 * the provinces that have repeaters in plate order ("06 Ankara (8)": the map's names, turkey_map_data.h), then
 * "Unknown" when some entries have no province. OK opens that province's list (nearest first or by frequency, the
 * same rows, details, tune and #, all within the province); Back returns to the first level, Back there leaves.
 * # on the first level: the nearest repeater's details in "All". Which groups have entries is a bit each (one pass
 * when the screen opens); a row's count is one pass when it is drawn. */
#include "config.h"
#if CONFIG_REPEATERS
#include "board.h"
#include "ui_nav.h"
#include "ui_screens.h"
#include "fmt.h"
#include "i18n.h"
#include "gfx.h"
#include "radio/radio.h"
#include "aprs/aprs.h"
#include "ui/rpt.h"
#if CONFIG_RPT_PROV
#include <string.h>
#include "ui/map.h"
#endif

static uint16_t sel = RPT_NONE, top = RPT_NONE;
static bool info;

#if CONFIG_RPT_PROV
#define GRP_UNKNOWN	(RPT_PROVS + 1u)	/* the first level's groups: 0 All, 1..81 the plates, then Unknown */
#define GRPS		(RPT_PROVS + 2u)
static uint8_t present[(GRPS + 7u) / 8u];	/* group g has entries (bit g; All always) */
static uint8_t filt = RPT_PROV_ALL;		/* the list's province: RPT_PROV_ALL, a plate or RPT_PROV_UNKNOWN */
static uint8_t gfocus;				/* the first level's picked row */
static uint16_t gtop;				/* and its top row (ui_draw_list) */
static bool groups;				/* the first level is shown */
#define FILT	filt
#else
#define FILT	RPT_PROV_ALL
#endif

/* Where we are now (the position APRS would send, ambiguity and all) */
static void here_now(rpt_here_t *h, int32_t *lat, int32_t *lon)
{
	uint32_t err;
	bool dmr;
	bool ok = aprs_position_now(lat, lon, &err, &dmr);
	rpt_here_set(h, ok, ok ? *lat : 0, ok ? *lon : 0);
}

/* "439.1625 DMR CC1 TS1+2" / "145.6000 FM 88.5" */
static void sb_what(sbuf_t *b, const rpt_entry_t *e)
{
	sb_freq(b, e->rx_hz);
	sb_chr(b, ' ');
	sb_str(b, rpt_mode_name(e));
	uint8_t m = e->mode & RPT_MODE_MASK;
	if (m == RPT_DMR) {
		sb_str(b, " CC");
		sb_u32(b, e->dmr & 0x0Fu, 1);
		uint8_t ts = (uint8_t)(e->dmr >> 4);
		if (ts) {
			sb_str(b, " TS");
			sb_str(b, ts == 1u ? "1" : ts == 2u ? "2" : "1+2");
		}
	} else if (e->ctcss10) {
		subtone_t t = { TONE_CTCSS, e->ctcss10 };
		sb_chr(b, ' ');
		sb_tone(b, &t);
	}
}

/* "12.3 km NE" from here, "" without either position */
static void sb_way(sbuf_t *b, const rpt_entry_t *e, const rpt_here_t *h, int32_t lat, int32_t lon, bool degrees)
{
	uint32_t m;
	uint16_t brg;
	if (!h->ok || !rpt_way(e, lat, lon, &m, &brg))
		return;
	sb_range(b, m, brg);
	if (degrees) {
		sb_chr(b, ' ');
		sb_u32(b, brg, 1);
		sb_str(b, GLYPH_DEGREE);
	}
}

static void __attribute__((noinline)) row(int y, int h, const rpt_entry_t *e, bool focus, const rpt_here_t *here,
					  int32_t lat, int32_t lon)
{
	char buf[32];
	sbuf_t b;
	const font_t *s = ui_layout.small, *m = ui_layout.medium;
	int w = gfx_width();
	if (focus)
		gfx_fill(0, y, w, h, ui_col.focus);
	color_t fg = focus ? ui_col.focus_fg : ui_col.fg, dim = focus ? ui_col.focus_fg : ui_col.dim;
	gfx_text(m, 2, y + 1, e->name, fg);
	sb_init(&b, buf, sizeof buf);
	sb_way(&b, e, here, lat, lon, false);
	gfx_text(m, w - gfx_text_width(m, buf) - 2, y + 1, buf, dim);
	sb_init(&b, buf, sizeof buf);
	sb_what(&b, e);
	if (e->city[0]) {
		sb_str(&b, "  ");
		sb_str(&b, e->city);
	}
	gfx_text(s, 2, y + m->h + 2, buf, dim);
}

static void __attribute__((noinline)) draw_list(int y, const rpt_here_t *h, int32_t lat, int32_t lon)
{
	const font_t *s = ui_layout.small, *m = ui_layout.medium;
	gfx_text(s, 0, y, rpt_info.sample ? TR("Sample list: not real repeaters")
				 : h->ok ? TR("Nearest first") : TR("No position: sorted by frequency"),
		 rpt_info.sample || !h->ok ? ui_col.warn : ui_col.dim);
	y += s->h + 3;
	int rh = m->h + s->h + 4;
	int rows = (ui_layout.hint_y - y) / rh;
	if (rows < 1)
		rows = 1;
	/* Keep the picked row on the page: the top row moves only when the pick would leave it */
	uint16_t rs = rpt_rank_in(h, sel, FILT), rt = top < rpt_info.count ? rpt_rank_in(h, top, FILT) : 0;
	if (top >= rpt_info.count || rs < rt) {
		top = sel;
	} else if (rs >= rt + (uint16_t)rows) {
		top = sel;
		for (int k = 1; k < rows; k++) {
			uint16_t p = rpt_next_in(h, top, -1, FILT);
			if (p == RPT_NONE)
				break;
			top = p;
		}
	}
	uint16_t i = top;
	for (int k = 0; k < rows && i != RPT_NONE; k++) {
		row(y, rh, &rpt_table[i], i == sel, h, lat, lon);
		y += rh;
		i = rpt_next_in(h, i, 1, FILT);
	}
}

#if CONFIG_RPT_PROV
static bool grp_has(uint8_t g)
{
	return (present[g >> 3] >> (g & 7u)) & 1u;
}

/* The province filter of group g */
static uint8_t grp_filter(uint8_t g)
{
	return g == 0 ? (uint8_t)RPT_PROV_ALL : g == GRP_UNKNOWN ? (uint8_t)RPT_PROV_UNKNOWN : g;
}

/* Which groups have entries: one pass over the table */
static void grp_scan(void)
{
	memset(present, 0, sizeof present);
	present[0] = 1u;	/* All */
	for (uint16_t i = 0; i < rpt_info.count; i++) {
		uint8_t p = rpt_table[i].prov;
		uint8_t g = p >= 1u && p <= RPT_PROVS ? p : (uint8_t)GRP_UNKNOWN;
		present[g >> 3] |= (uint8_t)(1u << (g & 7u));
	}
}

static uint8_t grp_count(void)
{
	uint8_t n = 0;
	for (uint8_t g = 0; g < GRPS; g++)
		n += grp_has(g);
	return n;
}

/* The group on row r of the first level */
static uint8_t grp_at(uint8_t r)
{
	for (uint8_t g = 0; g < GRPS; g++)
		if (grp_has(g) && r-- == 0)
			return g;
	return 0;
}

/* "All (252)", "06 Ankara (8)", "Unknown (2)" */
static void grp_item(uint16_t r, ui_item_t *it)
{
	uint8_t g = grp_at((uint8_t)r);
	sbuf_t b;
	sb_init(&b, it->label, sizeof it->label);
	if (g == 0) {
		sb_str(&b, TR("All repeaters"));
	} else if (g == GRP_UNKNOWN) {
		sb_str(&b, TR("Unknown"));
	} else {
		sb_u32(&b, g, 2);
		sb_chr(&b, ' ');
		sb_str(&b, map_province_name((uint8_t)(g - 1u)));
	}
	sb_str(&b, " (");
	sb_u32(&b, rpt_count_in(grp_filter(g)), 1);
	sb_chr(&b, ')');
}

static void __attribute__((noinline)) draw_groups(void)
{
	ui_list_t l = {
		.prefix = TR("Repeaters"), .header = rpt_info.sample ? TR("Sample list: not real repeaters") : NULL,
		.count = grp_count(), .focus = gfocus, .top = &gtop, .item = grp_item,
		.hint = TR("^v pick  OK list  # nearest"),
	};
	ui_draw_list(ui_view(), &ui_st, &l);
}

/* The province list's title: the province's name, "Unknown", or nothing for All */
static const char *filt_name(void)
{
	return filt == RPT_PROV_ALL ? "" : filt == RPT_PROV_UNKNOWN ? TR("Unknown") : map_province_name((uint8_t)(filt - 1u));
}
#endif

static void __attribute__((noinline)) draw_info(int y, const rpt_entry_t *e, const rpt_here_t *h, int32_t lat,
						int32_t lon)
{
	char buf[40];
	sbuf_t b;
	const font_t *s = ui_layout.small, *m = ui_layout.medium;
	sb_init(&b, buf, sizeof buf);
	sb_str(&b, e->name);
	sb_str(&b, "  ");
	sb_str(&b, rpt_mode_name(e));
	gfx_text(m, 0, y, buf, ui_col.fg);
	y += m->h + 3;
	sb_init(&b, buf, sizeof buf);
	sb_str(&b, e->city);
	char loc[8];
	rpt_locator(e, loc);
	if (loc[0]) {
		sb_str(&b, e->city[0] ? "  " : "");
		sb_str(&b, loc);
	}
	gfx_text(s, 0, y, buf, ui_col.dim);
	y += s->h + 2;
	sb_init(&b, buf, sizeof buf);
	sb_str(&b, "RX ");
	sb_freq(&b, e->rx_hz);
	sb_str(&b, "  TX ");
	sb_freq(&b, (uint32_t)((int32_t)e->rx_hz + e->off * (int32_t)RPT_STEP_HZ));
	gfx_text(s, 0, y, buf, ui_col.fg);
	y += s->h + 2;
	sb_init(&b, buf, sizeof buf);
	if (e->off == 0) {
		sb_str(&b, TR("Simplex"));
	} else {
		/* "Offset -7.600 MHz" */
		int32_t khz = e->off * (int32_t)RPT_STEP_HZ / 1000;
		sb_str(&b, TR("Offset "));
		sb_chr(&b, khz < 0 ? '-' : '+');
		uint32_t a = (uint32_t)(khz < 0 ? -khz : khz);
		sb_u32(&b, a / 1000u, 1);
		sb_chr(&b, '.');
		sb_u32(&b, a % 1000u, 3);
		sb_str(&b, " MHz");
	}
	gfx_text(s, 0, y, buf, ui_col.fg);
	y += s->h + 2;
	sb_init(&b, buf, sizeof buf);
	uint8_t mode = e->mode & RPT_MODE_MASK;
	if (mode == RPT_DMR) {
		uint8_t ts = (uint8_t)(e->dmr >> 4);
		sb_str(&b, "CC");
		sb_u32(&b, e->dmr & 0x0Fu, 1);
		sb_str(&b, "  ");
		sb_str(&b, ts == 0u ? TR("slot not known") : ts == 1u ? "TS1" : ts == 2u ? "TS2" : "TS1+2");
	} else if (mode == RPT_FM) {
		sb_str(&b, TR("Tone "));
		if (e->ctcss10) {
			subtone_t t = { TONE_CTCSS, e->ctcss10 };
			sb_tone(&b, &t);
		} else {
			sb_str(&b, TR("none"));
		}
		sb_str(&b, (e->mode & RPT_NARROW) ? "  12.5 kHz" : "  25 kHz");
	}
	gfx_text(s, 0, y, buf, ui_col.fg);
	y += s->h + 2;
	sb_init(&b, buf, sizeof buf);
	sb_way(&b, e, h, lat, lon, true);
	if (!buf[0])
		sb_str(&b, h->ok ? TR("Its position is not known") : TR("No position"));
	gfx_text(s, 0, y, buf, ui_col.fg);
	y += s->h + 4;
	bool tunable = mode == RPT_FM || mode == RPT_DMR;
	gfx_text(s, 0, y, tunable ? TR("OK: tune the VFO") : TR("Info only: not a mode of this radio"),
		 tunable ? ui_col.rx : ui_col.dim);
}

static void rpt_draw(void)
{
	rpt_here_t h;
	int32_t lat = 0, lon = 0;
#if CONFIG_RPT_PROV
	if (groups && rpt_info.count != 0) {
		draw_groups();
		return;
	}
	const char *name = filt_name();
#else
	const char *name = "";
#endif
	gfx_clear(ui_col.bg);
	int y = ui_draw_top(ui_view(), &ui_st, TR("Repeaters"), name) + 2;
	here_now(&h, &lat, &lon);
	if (rpt_info.count == 0) {
		gfx_text(ui_layout.small, 0, y, TR("No repeaters in this build"), ui_col.warn);
		ui_draw_bottom(&ui_st, TR("Back"));
		ui_draw_banner(&ui_st);
		return;
	}
	if (sel >= rpt_info.count)
		sel = rpt_next_in(&h, RPT_NONE, 1, FILT);
	if (info)
		draw_info(y, &rpt_table[sel], &h, lat, lon);
	else
		draw_list(y, &h, lat, lon);
	ui_draw_bottom(&ui_st, info ? TR("OK tune VFO  ^v next") : TR("^v pick  OK info  # nearest"));
	ui_draw_banner(&ui_st);
}

/* OK in the details: the VFO to the repeater, through the same requests the VFO's own settings use. The radio
 * task takes them in order: RX (opens the VFO), the mode, its tone or colour code and slot, then the split */
static void __attribute__((noinline)) tune(const rpt_entry_t *e)
{
	rpt_tune_t t;
	if (!rpt_tune_plan(e, &t)) {
		ui_toast(TR("Info only: not a mode of this radio"));
		return;
	}
	if (t.dmr ? !CONFIG_VFO_DMR : !CONFIG_VFO_FM) {
		ui_toast(TR("No VFO for this mode"));
		return;
	}
	if (ui_view()->state == RS_TX) {
		ui_toast(TR("Can't tune while transmitting"));
		return;
	}
	if (!board_in_band(t.rx_hz, false)) {
		ui_toast(TR("Not in the radio's bands"));
		return;
	}
	bool tx_ok = board_in_band(t.tx_hz, true);
	radio_req_vfo_set(t.rx_hz);
	radio_req_mode(t.dmr ? RADIO_MODE_DMR : RADIO_MODE_FM);
	if (t.dmr) {
		radio_req_dmr_color_code(t.cc);
		radio_req_dmr_timeslot(t.ts);
	} else {
		subtone_t tone = { t.ctcss10 ? TONE_CTCSS : TONE_NONE, t.ctcss10 }, none = { TONE_NONE, 0 };
		radio_req_bandwidth(t.narrow ? RADIO_BW_12K5 : RADIO_BW_25K);
		radio_req_tone(RADIO_TONE_TX, tone);
		radio_req_tone(RADIO_TONE_RX, none);
	}
	radio_req_vfo_tx(tx_ok && t.tx_hz != t.rx_hz ? t.tx_hz : 0);
	ui_toast(tx_ok ? TR("VFO tuned to the repeater") : TR("VFO tuned: its TX is outside the bands"));
	ui_go_home();
}

#if CONFIG_RPT_PROV
/* The first level: Up / Down pick (round), OK opens the group's list, # the nearest repeater's details (All), Back
 * leaves the screen */
static bool grp_key(const input_event_t *ev, bool press, int dir)
{
	if (press && dir != 0) {
		uint8_t n = grp_count();
		gfocus = (uint8_t)((gfocus + n + dir) % n);
		ui_dirty();
		return true;
	}
	if ((ev->action == ACT_SELECT || ev->action == ACT_CONTEXT) && ev->gesture == GESTURE_PRESS) {
		bool nearest = ev->action == ACT_CONTEXT;
		if (nearest)
			gfocus = 0;
		filt = grp_filter(grp_at(gfocus));
		groups = false;
		sel = top = RPT_NONE;	/* the group's nearest (rpt_draw) */
		info = nearest;
		if (nearest) {
			rpt_here_t h;
			int32_t lat, lon;
			here_now(&h, &lat, &lon);
			sel = rpt_nearest(&h);
		}
		ui_dirty();
		return true;
	}
	return false;
}
#endif

static bool rpt_key(const input_event_t *ev)
{
	bool press = ev->gesture == GESTURE_PRESS || ev->gesture == GESTURE_REPEAT;
	int dir = !press ? 0 : ev->action == ACT_ENC_STEP ? (ev->value > 0 ? 1 : -1) : ev->action == ACT_NAV_DOWN ? 1 :
		ev->action == ACT_NAV_UP ? -1 : 0;
	if (rpt_info.count == 0)
		return false;
#if CONFIG_RPT_PROV
	if (groups)
		return grp_key(ev, press, dir);
#endif
	if (dir != 0) {
		rpt_here_t h;
		int32_t lat, lon;
		here_now(&h, &lat, &lon);
		uint16_t n = sel < rpt_info.count ? rpt_next_in(&h, sel, dir, FILT) : RPT_NONE;
		sel = n != RPT_NONE ? n : rpt_next_in(&h, RPT_NONE, dir, FILT);	/* round from the end to the start */
		ui_dirty();
		return true;
	}
	if (ev->action == ACT_SELECT && ev->gesture == GESTURE_PRESS) {
		if (sel < rpt_info.count) {
			if (info)
				tune(&rpt_table[sel]);
			else
				info = true;
		}
		ui_dirty();
		return true;
	}
	if (ev->action == ACT_CONTEXT && ev->gesture == GESTURE_PRESS) {
		rpt_here_t h;
		int32_t lat, lon;
		here_now(&h, &lat, &lon);
		sel = rpt_next_in(&h, RPT_NONE, 1, FILT);
		info = true;
		ui_dirty();
		return true;
	}
	/* Back from the details: to the list, or (opened from the map) straight back to the map; from a list: to the
	 * first level (by province) */
	if (ev->action == ACT_BACK && ev->gesture == GESTURE_PRESS && ui_frame()->arg == 0) {
		if (info) {
			info = false;
			ui_dirty();
			return true;
		}
#if CONFIG_RPT_PROV
		groups = true;
		ui_dirty();
		return true;
#endif
	}
	return false;
}

static void rpt_enter(void)
{
	uint16_t arg = ui_frame()->arg;		/* 0: from Tools; i + 1: entry i's details (ui_rpt_open) */
	sel = top = RPT_NONE;	/* the nearest first, each time the screen opens */
	info = false;
#if CONFIG_RPT_PROV
	grp_scan();
	filt = RPT_PROV_ALL;	/* from the map: its entry in the whole list's order, as before */
	groups = arg == 0;
	gfocus = 0;
	gtop = 0;
#endif
	if (arg != 0 && arg <= rpt_info.count) {
		sel = (uint16_t)(arg - 1u);
		info = true;
	}
	ui_dirty();
}

void ui_rpt_open(uint16_t i)
{
	if (i < rpt_info.count)
		ui_push(SCR_REPEATERS, (uint16_t)(i + 1u));
}

void ui_rpt_tune(uint16_t i)
{
	if (i < rpt_info.count)
		tune(&rpt_table[i]);
}

const ui_scr_def_t ui_rpt_screen = {
	.name = "repeaters", .flags = SF_FIELD, .key = rpt_key, .draw = rpt_draw, .enter = rpt_enter,
};
#endif
