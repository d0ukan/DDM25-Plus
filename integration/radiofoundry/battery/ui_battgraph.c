/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus Tools > Battery with its history (battgraph.h): the charge level and voltage, the last 3.2 hours as a
 * graph (a sample every 2 minutes) and the time left while discharging ("--" while charging or unsteady). The history
 * is taken in the UI task's tick whatever screen is open (ui_battgraph_tick, from ui_app_tick), from the voltage the
 * radio core reads once a second (radio_view_t.battery_mv). RAM only. A radio that measures its DC supply instead
 * keeps upstream's Supply page (ui_battgraph_wanted). */
#include "config.h"
#if CONFIG_BATT_GRAPH
#include "board.h"
#include "ui_nav.h"
#include "ui_screens.h"
#include "fmt.h"
#include "i18n.h"
#include "gfx.h"
#include "radio/radio.h"
#include "ui/battgraph.h"
#include "ui/ui_misc.h"

static battg_t hist;

void ui_battgraph_reset(void)
{
	battg_reset(&hist);
}

void ui_battgraph_tick(const radio_view_t *v, uint32_t now_ms)
{
	if (board_volts() == BOARD_VOLTS_BATTERY)
		battg_feed(&hist, v->battery_mv, now_ms);
}

bool ui_battgraph_wanted(void)
{
	return board_volts() == BOARD_VOLTS_BATTERY;
}

/* "7.82 V" */
static void sb_v2(sbuf_t *b, uint16_t mv)
{
	uint32_t cv = (mv + 5u) / 10u;
	sb_u32(b, cv / 100u, 1);
	sb_chr(b, '.');
	sb_u32(b, cv % 100u, 2);
	sb_str(b, " V");
}

/* The samples as a line, the newest at the right; scaled to their own range (0.2 V at least) */
static void __attribute__((noinline)) graph(int x, int y, int w, int h)
{
	const font_t *s = ui_layout.small;
	char buf[12];
	sbuf_t b;
	gfx_rect(x, y, w, h, ui_col.line);
	uint8_t n = hist.n, lo = 255, hi = 0;
	for (uint8_t a = 0; a < n; a++) {
		uint8_t v = battg_sample(&hist, a);
		lo = v < lo ? v : lo;
		hi = v > hi ? v : hi;
	}
	if (n == 0) {
		gfx_text(s, x + 4, y + (h - s->h) / 2, TR("First point in 2 min"), ui_col.dim);
		return;
	}
	if (hi - lo < 20) {	/* 0.2 V at least */
		int mid = (hi + lo) / 2;
		lo = (uint8_t)(mid < 10 ? 0 : mid - 10);
		hi = (uint8_t)(lo + 20 > 255 ? 255 : lo + 20);
	}
	int iw = w - 2, ih = h - 4, py = -1;
	for (uint8_t a = 0; a < n; a++) {
		uint8_t v = battg_sample(&hist, a);
		int px = x + 1 + iw - 1 - (int)a * iw / (int)BATTG_SAMPLES;
		int vy = y + 2 + (ih - 1) - (int)(v - lo) * (ih - 1) / (int)(hi - lo);
		int x0 = px - iw / (int)BATTG_SAMPLES + 1;
		if (x0 > px)
			x0 = px;
		gfx_hline(x0, vy, px - x0 + 1, ui_col.rx);
		if (py >= 0 && py != vy)	/* join the step to the newer sample */
			gfx_vline(px + 1, py < vy ? py : vy, (py < vy ? vy - py : py - vy) + 1, ui_col.rx);
		py = vy;
	}
	sb_init(&b, buf, sizeof buf);
	sb_v2(&b, battg_mv(hi));
	gfx_text(s, x + 3, y + 2, buf, ui_col.dim);
	sb_init(&b, buf, sizeof buf);
	sb_v2(&b, battg_mv(lo));
	gfx_text(s, x + 3, y + h - s->h - 2, buf, ui_col.dim);
}

/* "Time left: 4 h 20 min" / "Time left: --" with why on the line after it (*why, "" with an estimate) */
static void __attribute__((noinline)) time_left(sbuf_t *b, const char **why)
{
	int32_t min = battg_minutes_left(&hist, BATTG_EMPTY_MV);
	sb_str(b, TR("Time left: "));
	*why = "";
	if (min == BATTG_NONE) {
		static const char *const whys[] = { "", TRN("Collecting: needs 30 min"), TRN("Charging"),
						    TRN("Not steady") };
		uint8_t w = battg_why_none(&hist, BATTG_EMPTY_MV);
		sb_str(b, "--");
		*why = tr(whys[w < 4u ? w : 1u]);
		return;
	}
	if (min > 99 * 60) {
		sb_str(b, TR(">99 h"));
		return;
	}
	sb_u32(b, (uint32_t)min / 60u, 1);
	sb_str(b, TR(" h "));
	sb_u32(b, (uint32_t)min % 60u, 2);
	sb_str(b, TR(" min"));
}

static void battgraph_draw(void)
{
	char buf[40];
	sbuf_t b;
	const font_t *s = ui_layout.small, *m = ui_layout.medium;
	const radio_view_t *v = ui_view();
	gfx_clear(ui_col.bg);
	int y = ui_draw_top(v, &ui_st, TR("Battery"), "") + 2;
	int w = gfx_width();
	sb_init(&b, buf, sizeof buf);
	sb_u32(&b, ui_st.battery_pct, 1);
	sb_str(&b, "%  ");
	if (v->battery_mv)
		sb_v2(&b, v->battery_mv);
	bool low = ui_st.battery_pct <= (ui_st.low_batt_pct ? ui_st.low_batt_pct : 15);
	gfx_text(m, 0, y, buf, low ? ui_col.warn : ui_col.fg);
	y += m->h + 3;
	const char *why;
	sb_init(&b, buf, sizeof buf);
	time_left(&b, &why);
	gfx_text(s, 0, y, buf, ui_col.fg);
	y += s->h + 2;
	gfx_text(s, 0, y, why, ui_col.dim);
	y += s->h + 4;
	int gh = ui_layout.hint_y - y - s->h - 4;
	if (gh > 120)
		gh = 120;
	if (gh < 24)
		gh = 24;
	graph(0, y, w, gh);
	y += gh + 1;
	gfx_text(s, 0, y, "-3.2 h", ui_col.dim);
	const char *now_txt = TR("now");
	gfx_text(s, w - gfx_text_width(s, now_txt) - 1, y, now_txt, ui_col.dim);
	ui_draw_bottom(&ui_st, TR("A point every 2 min  Back"));
	ui_draw_banner(&ui_st);
}

static void battgraph_enter(void)
{
	ui_dirty();
}

const ui_scr_def_t ui_battgraph_screen = {
	.name = "battery", .flags = SF_FIELD, .draw = battgraph_draw, .enter = battgraph_enter,
};
#endif
