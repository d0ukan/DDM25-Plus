/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus Tools > Spectrum (docs/plus.md): the sweep as bars around the frequency in use, the peak of each point as a
 * mark, the floor as a line, and the close call hit (the strongest signal well above the floor) named under it.
 * Up / Down (or the knob) move the center by a quarter span, Left / Right change the span, OK tunes the VFO to the
 * hit (else the strongest point) and ends the sweep, Back ends it and leaves.
 * CONFIG_SPECTRUM_WF (DDM32 Plus, ui/wfall.h): the last sweeps as a waterfall under the bars, newest on top; * turns it
 * off and on (RAM only, on at power on). Its buffer comes from the RTOS heap while the screen is on top with it on
 * (ui_spectrum_wf_tick gives it back once another screen is on top, Back and OK at once; back on top, the next draw
 * takes it again and the rows start over); when the heap has no room the bars are drawn alone and the screen says so
 * (tried again on the next entry or *).
 * CONFIG_SPECTRUM_WF3D (DDM32 Plus, item 45, ui/wf3d.h): a third view, the same rows as a ridge plot in perspective;
 * * cycles waterfall > 3D > bars alone > waterfall (RAM only, the waterfall at power on); the 3D view keeps the
 * waterfall's buffer and rows (no new RAM). */
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
#include "ui/ui_scratch.h"
#if CONFIG_SPECTRUM_WF
#include "ui/wfall.h"
#if CONFIG_SPECTRUM_WF3D
#include "ui/wf3d.h"
#endif
#ifdef UI_HOST_HEAP
/* The host tests: their own counting allocator, which can be made to fail */
void *ui_host_malloc(size_t n);
void ui_host_free(void *p);
#define wf_malloc	ui_host_malloc
#define wf_free		ui_host_free
#else
#include "FreeRTOS.h"
#define wf_malloc	pvPortMalloc
#define wf_free		vPortFree
#endif
#endif

static const uint32_t spans[] = { 250000u, 500000u, 1000000u, 2500000u };
static uint8_t span_i = 1;
static uint32_t center_hz;
/* The sweep as last copied: the tool screens' shared buffer (ui_scratch.h), refilled by snap() in each draw and key */
#define sv	(ui_scratch.spectrum)

#if CONFIG_SPECTRUM_WF
static wf_t wf;			/* the rows; wf.buf from the heap while the screen is open with the waterfall on */
#if CONFIG_SPECTRUM_WF3D
enum { WF_VIEW_FLAT, WF_VIEW_3D, WF_VIEW_OFF, WF_VIEWS };
static uint8_t wf_view;		/* the user's choice (*): the waterfall, the 3D view or the bars alone */
#define wf_on	(wf_view != WF_VIEW_OFF)
#else
static bool wf_on = true;	/* the user's choice (*) */
#endif
static bool wf_no_room;		/* the last try found no room in the heap */

static void wf_open(void)
{
	if (wf.buf == NULL)
		wf.buf = wf_malloc(WF_BYTES);
	wf_no_room = wf.buf == NULL;
	wf_reset(&wf);
}

static void wf_close(void)
{
	if (wf.buf != NULL)
		wf_free(wf.buf);
	wf.buf = NULL;
	wf_reset(&wf);
}

void ui_spectrum_wf_tick(uint8_t top)
{
	if (top != SCR_SPECTRUM && wf.buf != NULL)
		wf_close();
}

bool ui_spectrum_wf_held(void)
{
	return wf.buf != NULL;
}
#endif

static bool snap(void)
{
	ui_scratch_take(UI_SCRATCH_SPECTRUM);
	return spectrum_snapshot(&sv);
}

static void restart(void)
{
	radio_req_spectrum(center_hz, spans[span_i]);
#if CONFIG_SPECTRUM_WF
	wf_reset(&wf);	/* another center or span: the rows start over */
#endif
	ui_dirty();
}

static void spectrum_enter(void)
{
	const radio_view_t *v = ui_view();
	center_hz = v->ch.rx_hz;
	memset(ui_scratch_take(UI_SCRATCH_SPECTRUM), 0, sizeof sv);
#if CONFIG_SPECTRUM_WF
	if (wf_on)
		wf_open();
#endif
	restart();
}

/* The point OK tunes to: the close call hit, else the strongest reading (of the sweep in sv: a fresh copy in the key
 * handler, the draw's own in the draw) */
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

#if CONFIG_SPECTRUM_WF
/* The rows from top down, newest first: each point's level as one of the screen's colours, runs of one colour drawn
 * as one box */
static void __attribute__((noinline)) wf_draw(int x0, int bw, int top, int h)
{
	const color_t slot[WF_SLOTS] = { ui_col.bg, ui_col.line, ui_col.idle, ui_col.dim, ui_col.rx, ui_col.warn,
					 ui_col.warn, ui_col.tx };
	int rh = h / (int)WF_ROWS;
	if (rh < 1)
		rh = 1;
	for (uint8_t r = 0; r < wf.n && (r + 1) * rh <= h; r++) {
		int y = top + r * rh;
		uint8_t i = 0;
		while (i < wf.points) {
			uint8_t s = wf_slot(wf_get(&wf, r, i)), k = (uint8_t)(i + 1u);
			while (k < wf.points && wf_slot(wf_get(&wf, r, k)) == s)
				k++;
			if (s != 0)
				gfx_fill(x0 + i * bw, y, (k - i) * bw, rh, slot[s]);
			i = k;
		}
	}
}
#if CONFIG_SPECTRUM_WF3D
/* The 3D view: the rows as wf3d draws them, in the waterfall's colours */
static void __attribute__((noinline)) wf3d_view(int x0, int bw, int top, int h)
{
	const color_t slot[WF_SLOTS] = { ui_col.bg, ui_col.line, ui_col.idle, ui_col.dim, ui_col.rx, ui_col.warn,
					 ui_col.warn, ui_col.tx };
	wf3d_draw(&wf, x0, bw, top, h, slot, ui_col.bg);
}
#endif
#endif

static void spectrum_draw(void)
{
	char buf[32];
	sbuf_t b;
	gfx_clear(ui_col.bg);
	int y = ui_draw_top(ui_view(), &ui_st, TR("Spectrum"), "") + 2;
	const font_t *f = ui_layout.small;
	bool have = snap();
	sb_init(&b, buf, sizeof buf);
	sb_mhz(&b, center_hz);
	sb_str(&b, " +-");
	sb_u32(&b, spans[span_i] / 2000u, 1);
	sb_chr(&b, 'k');
	gfx_text(f, 0, y, buf, ui_col.fg);
	y += f->h + 2;
	int w = gfx_width(), base = ui_layout.hint_y - 2 * f->h - 6, top = y;
#if CONFIG_SPECTRUM_WF
	/* With the waterfall: the bars above, the rows below them */
	int wf_top = 0, wf_h = 0;
	if (wf_on && wf.buf == NULL && !wf_no_room)
		wf_open();	/* back on top after another screen (which gave the buffer back): the rows start over */
	if (wf_on && wf.buf != NULL) {
#if CONFIG_SPECTRUM_WF3D
		wf_h = (base - top) * (wf_view == WF_VIEW_3D ? 13 : 11) / 20;	/* the 3D view: a little more of it */
#else
		wf_h = (base - top) * 11 / 20;
#endif
		wf_top = base - wf_h;
		base = wf_top - 3;
		if (have && sv.sweeps != 0 && sv.sweeps != wf.sweeps && sv.center_hz == center_hz)
			wf_push(&wf, sv.dbm, sv.points, sv.floor_dbm, sv.sweeps);	/* a sweep finished */
	} else if (wf_on && wf_no_room) {
		gfx_text(f, w - gfx_text_width(f, TR("No room")), y - f->h - 2, TR("No room"), ui_col.warn);
	}
#endif
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
#if CONFIG_SPECTRUM_WF
		if (wf_h > 0 && wf.points == sv.points) {
#if CONFIG_SPECTRUM_WF3D
			if (wf_view == WF_VIEW_3D)
				wf3d_view(x0, bw, wf_top, wf_h);
			else
#endif
			wf_draw(x0, bw, wf_top, wf_h);
			gfx_vline(x0 + (sv.points / 2) * bw + bw / 2, wf_top, 2, ui_col.line);	/* the center */
		}
		base = wf_h > 0 ? wf_top + wf_h : base;
#endif
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
#if CONFIG_SPECTRUM_WF
	ui_draw_bottom(&ui_st, TR(TEXT_ALT_OPEN "OK tune  <> span  * waterfall" TEXT_ALT_NEXT "OK tune  <> span  *"
				  TEXT_ALT_CLOSE));
#else
	ui_draw_bottom(&ui_st, TR("OK tune  <> span  Back"));
#endif
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
#if CONFIG_SPECTRUM_WF
	if (ev->gesture == GESTURE_PRESS && ev->action == ACT_QUICK) {
#if CONFIG_SPECTRUM_WF3D
		wf_view = (uint8_t)((wf_view + 1u) % WF_VIEWS);	/* waterfall > 3D (the same rows) > bars > waterfall */
		if (wf_view == WF_VIEW_FLAT)
			wf_open();
		else if (wf_view == WF_VIEW_OFF)
			wf_close();
		else if (wf.buf == NULL && !wf_no_room)
			wf_open();
		ui_toast(!wf_on ? TR("Waterfall off") : wf.buf == NULL ? TR("Waterfall: no room") :
			 wf_view == WF_VIEW_3D ? TR("3D waterfall") : TR("Waterfall on"));
#else
		wf_on = !wf_on;
		if (wf_on)
			wf_open();
		else
			wf_close();
		ui_toast(!wf_on ? TR("Waterfall off") : wf.buf != NULL ? TR("Waterfall on") : TR("Waterfall: no room"));
#endif
		ui_dirty();
		return true;
	}
#endif
	if (ev->gesture == GESTURE_PRESS && ev->action == ACT_SELECT) {
		int t = snap() ? target_point() : -1;
		radio_req_spectrum(0, 0);
		if (t >= 0) {
			uint32_t hz = spectrum_point_hz(&sv, (uint8_t)t);
#if CONFIG_SPECTRUM_WF
			wf_close();
#endif
			radio_req_vfo(true);
			radio_req_vfo_set(hz);
			ui_go_home();
		}
		return true;
	}
	if (ev->gesture == GESTURE_PRESS && ev->action == ACT_BACK) {
		radio_req_spectrum(0, 0);	/* and the list's own Back leaves the screen */
#if CONFIG_SPECTRUM_WF
		wf_close();
#endif
	}
	return false;
}

const ui_scr_def_t ui_spectrum_screen = {
	.name = "spectrum", .flags = SF_FIELD, .key = spectrum_key, .draw = spectrum_draw, .enter = spectrum_enter,
};
#endif
