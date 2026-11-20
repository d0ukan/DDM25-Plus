/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus Tools > Airfields (airfield.h): the Turkish airfields nearest first from the position APRS uses (GPS, else
 * the computer's, else the fixed one; aprs_position_now, as Tools > Repeaters), each with its distance and compass
 * direction and its first frequencies; by code without a position. Up / Down / the knob: pick (round from the end to
 * the start), OK: its details, #: the nearest one's details. In the details: Up / Down a frequency (TWR 118.100, GND,
 * APP, ATIS, ...), OK: listen to it, Back: the list. Listening is what Tools > Air band does: the VFO on that
 * frequency (radio_req_vfo + radio_req_vfo_set), received in AM because it is in the air band, and receive only: the
 * radio core refuses TX there whatever the VFO says (radio_sm_tx_check: TXDENY_AM). An 8.33 kHz channel name tunes its
 * grid point (air_freq_hz). The order is walked, never stored (air_next): the screen keeps the picked and the top
 * row's airfields and the picked frequency (6 B). */
#include "config.h"
#if CONFIG_AIRFIELDS
#include "board.h"
#include "ui_nav.h"
#include "ui_screens.h"
#include "fmt.h"
#include "i18n.h"
#include "gfx.h"
#include "radio/radio.h"
#include "radio/radio_am.h"
#include "aprs/aprs.h"
#include "ui/airfield.h"

static uint16_t sel = AIR_NONE, top = AIR_NONE;
static uint8_t fsel;
static bool info;

/* Where we are now (the position APRS would send, ambiguity and all) */
static void here_now(air_here_t *h, int32_t *lat, int32_t *lon)
{
	uint32_t err;
	bool dmr;
	bool ok = aprs_position_now(lat, lon, &err, &dmr);
	air_here_set(h, ok, ok ? *lat : 0, ok ? *lon : 0);
}

/* "118.100" (the channel name, 8.33 kHz ones as pilots say them: "121.580") */
static void sb_khz(sbuf_t *b, uint32_t khz)
{
	sb_u32(b, khz / 1000u, 1);
	sb_chr(b, '.');
	sb_u32(b, khz % 1000u, 3);
}

/* "TWR 118.100" */
static void sb_air_freq(sbuf_t *b, const air_freq_t *f)
{
	sb_str(b, air_type_name(f->type));
	sb_chr(b, ' ');
	sb_khz(b, air_freq_khz(f));
}

/* "LTAC Esenboğa" */
static void sb_field(sbuf_t *b, const air_field_t *a)
{
	sb_str(b, a->code);
	sb_chr(b, ' ');
	sb_str(b, a->name);
}

static void __attribute__((noinline)) row(int y, int h, uint16_t i, bool focus, const air_here_t *here, int32_t lat,
					  int32_t lon)
{
	char buf[48], fit[48];
	sbuf_t b;
	const air_field_t *a = &air_fields[i];
	const font_t *s = ui_layout.small, *m = ui_layout.medium;
	int w = gfx_width();
	if (focus)
		gfx_fill(0, y, w, h, ui_col.focus);
	color_t fg = focus ? ui_col.focus_fg : ui_col.fg, dim = focus ? ui_col.focus_fg : ui_col.dim;
	/* "LTAC Esenboğa", then "28 km NE  TWR 118.100" (the tower, else the first frequency; all in the details) */
	sb_init(&b, buf, sizeof buf);
	sb_field(&b, a);
	gfx_text_fit(m, buf, w - 4, fit, sizeof fit);
	gfx_text(m, 2, y + 1, fit, fg);
	sb_init(&b, buf, sizeof buf);
	if (here->ok) {
		uint32_t meters;
		uint16_t brg;
		air_way(i, lat, lon, &meters, &brg);
		sb_range(&b, meters, brg);
		sb_str(&b, "  ");
	}
	if (a->count == 0) {
		sb_str(&b, TR("No frequencies in the data"));
	} else {
		uint8_t k = 0, more = 0;
		while (k + 1u < a->count && air_freqs[a->first + k].type != AIR_TWR)
			k++;
		const air_freq_t *f = &air_freqs[a->first + (air_freqs[a->first + k].type == AIR_TWR ? k : 0u)];
		sb_air_freq(&b, f);
		/* "TWR 125.000 +1": the service has more frequencies (all of them in the details) */
		for (k = 0; k < a->count; k++)
			more = (uint8_t)(more + (air_freqs[a->first + k].type == f->type));
		if (more > 1u) {
			sb_str(&b, " +");
			sb_u32(&b, more - 1u, 1);
		}
	}
	gfx_text_fit(s, buf, w - 4, fit, sizeof fit);
	gfx_text(s, 2, y + m->h + 2, fit, dim);
}

static void __attribute__((noinline)) draw_list(int y, const air_here_t *h, int32_t lat, int32_t lon)
{
	const font_t *s = ui_layout.small, *m = ui_layout.medium;
	gfx_text(s, 0, y, h->ok ? TR("Nearest first") : TR("No position: by code"), h->ok ? ui_col.dim : ui_col.warn);
	y += s->h + 3;
	int rh = m->h + s->h + 4;
	int rows = (ui_layout.hint_y - y) / rh;
	if (rows < 1)
		rows = 1;
	/* Keep the picked row on the page: the top row moves only when the pick would leave it */
	uint16_t rs = air_rank(h, sel), rt = top < air_field_count ? air_rank(h, top) : 0;
	if (top >= air_field_count || rs < rt) {
		top = sel;
	} else if (rs >= rt + (uint16_t)rows) {
		top = sel;
		for (int k = 1; k < rows; k++) {
			uint16_t p = air_next(h, top, -1);
			if (p == AIR_NONE)
				break;
			top = p;
		}
	}
	uint16_t i = top;
	for (int k = 0; k < rows && i != AIR_NONE; k++) {
		row(y, rh, i, i == sel, h, lat, lon);
		y += rh;
		i = air_next(h, i, 1);
	}
}

static void __attribute__((noinline)) draw_info(int y, uint16_t i, const air_here_t *h, int32_t lat, int32_t lon)
{
	static const char *const kinds[] = { TRN("Large airport"), TRN("Airport"), TRN("Small airfield") };
	char buf[48], fit[48];
	sbuf_t b;
	const air_field_t *a = &air_fields[i];
	const font_t *s = ui_layout.small, *m = ui_layout.medium;
	int w = gfx_width();
	sb_init(&b, buf, sizeof buf);
	sb_field(&b, a);
	gfx_text_fit(m, buf, w, fit, sizeof fit);
	gfx_text(m, 0, y, fit, ui_col.fg);
	y += m->h + 3;
	sb_init(&b, buf, sizeof buf);
	sb_str(&b, tr(kinds[a->kind < 3u ? a->kind : 2u]));
	if (h->ok) {
		uint32_t meters;
		uint16_t brg;
		air_way(i, lat, lon, &meters, &brg);
		sb_str(&b, "  ");
		sb_range(&b, meters, brg);
		sb_chr(&b, ' ');
		sb_u32(&b, brg, 1);
		sb_str(&b, GLYPH_DEGREE);
	}
	gfx_text_fit(s, buf, w, fit, sizeof fit);
	gfx_text(s, 0, y, fit, ui_col.dim);
	y += s->h + 4;
	if (a->count == 0) {
		gfx_text_fit(s, TR(TEXT_ALT_OPEN "No frequencies in the data" TEXT_ALT_NEXT "No frequencies" TEXT_ALT_CLOSE), w,
			     fit, sizeof fit);	/* the long one is wider than the panel */
		gfx_text(s, 0, y, fit, ui_col.warn);
		return;
	}
	if (fsel >= a->count)
		fsel = 0;
	int rh = m->h + 4;
	int rows = (ui_layout.hint_y - y - 2 * (s->h + 2) - 2) / rh;
	if (rows < 1)
		rows = 1;
	int first = fsel >= rows ? fsel - rows + 1 : 0;
	for (int k = first; k < a->count && k < first + rows; k++) {
		const air_freq_t *f = &air_freqs[a->first + k];
		bool focus = k == fsel;
		if (focus)
			gfx_fill(0, y, w, rh, ui_col.focus);
		color_t fg = focus ? ui_col.focus_fg : ui_col.fg;
		sb_init(&b, buf, sizeof buf);
		sb_air_freq(&b, f);
		gfx_text(m, 2, y + 2, buf, fg);
		if (air_freq_833(f))
			gfx_text(s, w - gfx_text_width(s, "8.33") - 2, y + 2 + (m->h - s->h) / 2, "8.33",
				 focus ? fg : ui_col.dim);
		y += rh;
	}
	gfx_text(s, 0, y + 2, TR("AM, RX only"), ui_col.rx);
	gfx_text(s, 0, y + 4 + s->h, TR("Not for navigation"), ui_col.dim);
}

static void airfield_draw(void)
{
	air_here_t h;
	int32_t lat = 0, lon = 0;
	gfx_clear(ui_col.bg);
	int y = ui_draw_top(ui_view(), &ui_st, TR("Airfields"), "") + 2;
	here_now(&h, &lat, &lon);
	if (sel >= air_field_count)
		sel = air_nearest(&h);
	if (info)
		draw_info(y, sel, &h, lat, lon);
	else
		draw_list(y, &h, lat, lon);
	ui_draw_bottom(&ui_st, info ? TR("OK listen  ^v pick  Back") : TR("^v pick  OK info  # nearest"));
	ui_draw_banner(&ui_st);
}

void ui_airfield_tune(uint16_t i, uint8_t k)
{
	static char done[48];
	if (i >= air_field_count || k >= air_fields[i].count)
		return;
	const air_field_t *a = &air_fields[i];
	const air_freq_t *f = &air_freqs[a->first + k];
	uint32_t hz = air_freq_hz(f);
	if (!radio_am_capable()) {
		ui_toast(TR("No AM receive on this radio"));
		return;
	}
	if (ui_view()->state == RS_TX) {
		ui_toast(TR("Can't tune while transmitting"));
		return;
	}
	if (!radio_am_band(hz) || !board_in_band(hz, false)) {
		ui_toast(TR("Not in the radio's bands"));
		return;
	}
	/* Tools > Air band's path: the VFO there; AM and no TX follow from the band (radio_am.h) */
	radio_req_vfo(true);
	radio_req_vfo_set(hz);
	sbuf_t b;
	sb_init(&b, done, sizeof done);
	sb_str(&b, a->code);
	sb_chr(&b, ' ');
	sb_air_freq(&b, f);
	sb_str(&b, TR(": AM, RX only"));
	ui_toast(done);
	ui_go_home();
}

static bool airfield_key(const input_event_t *ev)
{
	bool press = ev->gesture == GESTURE_PRESS || ev->gesture == GESTURE_REPEAT;
	if (air_field_count == 0)
		return false;
	if (press && (ev->action == ACT_NAV_UP || ev->action == ACT_NAV_DOWN || ev->action == ACT_ENC_STEP)) {
		int dir = ev->action == ACT_ENC_STEP ? (ev->value > 0 ? 1 : -1) : ev->action == ACT_NAV_DOWN ? 1 : -1;
		if (info) {
			uint8_t n = sel < air_field_count ? air_fields[sel].count : 0;
			if (n)
				fsel = (uint8_t)((fsel + n + (dir > 0 ? 1 : n - 1)) % n);
		} else {
			air_here_t h;
			int32_t lat, lon;
			here_now(&h, &lat, &lon);
			uint16_t nx = sel < air_field_count ? air_next(&h, sel, dir) : AIR_NONE;
			sel = nx != AIR_NONE ? nx : air_next(&h, AIR_NONE, dir);	/* round from the end to the start */
		}
		ui_dirty();
		return true;
	}
	if (ev->action == ACT_SELECT && ev->gesture == GESTURE_PRESS) {
		if (sel < air_field_count) {
			if (!info) {
				info = true;
				fsel = 0;
			} else if (air_fields[sel].count == 0) {
				ui_toast(TR("No frequencies in the data"));
			} else {
				ui_airfield_tune(sel, fsel);
			}
		}
		ui_dirty();
		return true;
	}
	if (ev->action == ACT_CONTEXT && ev->gesture == GESTURE_PRESS) {
		air_here_t h;
		int32_t lat, lon;
		here_now(&h, &lat, &lon);
		sel = air_nearest(&h);
		info = true;
		fsel = 0;
		ui_dirty();
		return true;
	}
	if (ev->action == ACT_BACK && ev->gesture == GESTURE_PRESS && info) {
		info = false;
		ui_dirty();
		return true;
	}
	return false;
}

static void airfield_enter(void)
{
	sel = top = AIR_NONE;	/* the nearest first, each time the screen opens */
	info = false;
	fsel = 0;
	ui_dirty();
}

const ui_scr_def_t ui_airfield_screen = {
	.name = "airfields", .flags = SF_FIELD, .key = airfield_key, .draw = airfield_draw, .enter = airfield_enter,
};
#endif
