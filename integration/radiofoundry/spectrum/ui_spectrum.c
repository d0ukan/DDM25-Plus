/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus Tools > Spectrum (docs/plus.md): the sweep as bars around the frequency in use, the peak of each point as a
 * mark, the floor as a line, and the close call hit (the strongest signal well above the floor) named under it.
 * Up / Down (or the knob) move the center by a quarter span, Left / Right change the span, OK tunes the VFO to the
 * hit (else the strongest point) and ends the sweep, Back ends it and leaves. */
#include "config.h"
#if CONFIG_SPECTRUM
#include <string.h>
#include "ui_nav.h"
#include "ui_screens.h"
#include "fmt.h"
#include "i18n.h"
#include "gfx.h"
#include "radio/radio.h"
#include "radio/spectrum.h"

static const uint32_t spans[] = { 250000u, 500000u, 1000000u, 2500000u };
static uint8_t span_i = 1;
static uint32_t center_hz;
static spectrum_view_t sv;

static void restart(void)
{
	radio_req_spectrum(center_hz, spans[span_i]);
	ui_dirty();
}

static void spectrum_enter(void)
{
	const radio_view_t *v = ui_view();
	center_hz = v->ch.rx_hz;
	memset(&sv, 0, sizeof sv);
	restart();
}

/* The point OK tunes to: the close call hit, else the strongest reading */
static int target_point(void)
{
	if (sv.hit < sv.points)
		return sv.hit;
	int best = -1;
	for (uint8_t i = 0; i < sv.points; i++) {
		if (sv.dbm[i] != 0 && (best < 0 || sv.dbm[i] > sv.dbm[best]))
			best = i;
	}
	return best;
}

static void sb_mhz(sbuf_t *b, uint32_t hz)
{
	sb_u32(b, hz / 1000000u, 1);
	sb_chr(b, '.');
	sb_u32(b, (hz / 1000u) % 1000u, 3);
}

static int level_y(int16_t dbm, int top, int base)
{
	/* -130 dBm at the base, -50 dBm at the top */
	int h = base - top;
	int v = dbm <= -130 ? 0 : dbm >= -50 ? h : (dbm + 130) * h / 80;
	return base - v;
}

static void spectrum_draw(void)
{
	char buf[32];
	sbuf_t b;
	gfx_clear(ui_col.bg);
	int y = ui_draw_top(ui_view(), &ui_st, TR("Spectrum"), "") + 2;
	const font_t *f = ui_layout.small;
	bool have = spectrum_snapshot(&sv);
	sb_init(&b, buf, sizeof buf);
	sb_mhz(&b, center_hz);
	sb_str(&b, " +-");
	sb_u32(&b, spans[span_i] / 2000u, 1);
	sb_chr(&b, 'k');
	gfx_text(f, 0, y, buf, ui_col.fg);
	y += f->h + 2;
	int w = gfx_width(), base = ui_layout.hint_y - 2 * f->h - 6, top = y;
	gfx_hline(0, base, w, ui_col.line);
	if (!have || sv.points == 0) {
		gfx_text(f, 0, top + 4, TR("Sweeping..."), ui_col.dim);
	} else {
		int bw = w / sv.points;
		int x0 = (w - bw * sv.points) / 2;
		int fy = level_y(sv.floor_dbm, top, base);
		for (uint8_t i = 0; i < sv.points; i++) {
			int x = x0 + i * bw;
			if (sv.dbm[i] != 0) {
				int by = level_y(sv.dbm[i], top, base);
				color_t c = i == sv.hit ? ui_col.rx : sv.dbm[i] - sv.floor_dbm >= SPECTRUM_HIT_DB / 2 ? ui_col.fg : ui_col.dim;
				gfx_fill(x, by, bw > 1 ? bw - 1 : 1, base - by, c);
			}
			if (sv.peak_dbm[i] != 0)
				gfx_hline(x, level_y(sv.peak_dbm[i], top, base), bw > 1 ? bw - 1 : 1, ui_col.warn);
		}
		if (sv.sweeps)
			gfx_hline(0, fy, w, ui_col.idle);
		gfx_vline(x0 + (sv.points / 2) * bw + bw / 2, top, base - top, ui_col.line);
		int ty = base + 2;
		sb_init(&b, buf, sizeof buf);
		int t = target_point();
		if (sv.hit < sv.points) {
			sb_str(&b, TR("Hit "));
		} else {
			sb_str(&b, TR("Peak "));
		}
		if (t >= 0) {
			sb_mhz(&b, spectrum_point_hz(&sv, (uint8_t)t));
			sb_chr(&b, ' ');
			sb_i32(&b, sv.dbm[t]);
			sb_str(&b, "dBm");
		}
		gfx_text(f, 0, ty, buf, sv.hit < sv.points ? ui_col.rx : ui_col.fg);
		sb_init(&b, buf, sizeof buf);
		sb_str(&b, TR("Floor "));
		sb_i32(&b, sv.floor_dbm);
		sb_str(&b, "dBm");
		gfx_text(f, 0, ty + f->h + 1, buf, ui_col.dim);
	}
	ui_draw_bottom(&ui_st, TR("OK tune  <> span  Back"));
	ui_draw_banner(&ui_st);
}

static bool spectrum_key(const input_event_t *ev)
{
	bool press = ev->gesture == GESTURE_PRESS || ev->gesture == GESTURE_REPEAT;
	int dir = 0;
	if (ev->action == ACT_ENC_STEP)
		dir = ev->value > 0 ? 1 : ev->value < 0 ? -1 : 0;
	else if (press && ev->action == ACT_NAV_UP)
		dir = 1;
	else if (press && ev->action == ACT_NAV_DOWN)
		dir = -1;
	if (dir != 0) {
		center_hz = (uint32_t)((int32_t)center_hz + dir * (int32_t)(spans[span_i] / 4u));
		restart();
		return true;
	}
	if (press && (ev->action == ACT_NAV_LEFT || ev->action == ACT_NAV_RIGHT)) {
		uint8_t n = (uint8_t)(sizeof spans / sizeof spans[0]);
		span_i = (uint8_t)((span_i + n + (ev->action == ACT_NAV_RIGHT ? 1 : -1)) % n);
		restart();
		return true;
	}
	if (ev->gesture == GESTURE_PRESS && ev->action == ACT_SELECT) {
		int t = target_point();
		radio_req_spectrum(0, 0);
		if (t >= 0) {
			uint32_t hz = spectrum_point_hz(&sv, (uint8_t)t);
			radio_req_vfo(true);
			radio_req_vfo_set(hz);
			ui_go_home();
		}
		return true;
	}
	if (ev->gesture == GESTURE_PRESS && ev->action == ACT_BACK)
		radio_req_spectrum(0, 0);	/* and the list's own Back leaves the screen */
	return false;
}

const ui_scr_def_t ui_spectrum_screen = {
	.name = "spectrum", .flags = SF_FIELD, .key = spectrum_key, .draw = spectrum_draw, .enter = spectrum_enter,
};
#endif
