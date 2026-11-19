/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus Tools > Fox hunt (docs/plus.md): the channel in use's signal strength large, in S units, as a bar with a
 * peak hold, and the vario tone (fox.h) while the screen is open. Left / Right: the virtual attenuator (0, 10, 20,
 * 30 dB), OK: clear the peak, Back: tone off and leave. */
#include "config.h"
#if CONFIG_FOX
#include <string.h>
#include "ui_nav.h"
#include "ui_screens.h"
#include "fmt.h"
#include "i18n.h"
#include "gfx.h"
#include "radio/radio.h"
#include "radio/fox.h"

static uint8_t att;
static int16_t peak;

static void fox_enter(void)
{
	peak = 0;
	radio_req_fox(true, att);
	ui_dirty();
}

/* S units on VHF/UHF: S9 = -93 dBm, 6 dB a unit; above S9 in dB */
static void sb_s_units(sbuf_t *b, int16_t dbm)
{
	if (dbm == 0) {
		sb_str(b, "S-");
		return;
	}
	if (dbm > -93) {
		sb_str(b, "S9+");
		sb_u32(b, (uint32_t)(dbm + 93), 1);
		return;
	}
	int s = 9 - (-93 - dbm + 5) / 6;
	sb_chr(b, 'S');
	sb_chr(b, (char)('0' + (s < 0 ? 0 : s)));
}

static int bar_x(int16_t dbm, int w)
{
	int eff = dbm - att;
	if (dbm == 0 || eff <= FOX_FLOOR_DBM)
		return 0;
	if (eff >= FOX_TOP_DBM)
		return w;
	return (eff - FOX_FLOOR_DBM) * w / (FOX_TOP_DBM - FOX_FLOOR_DBM);
}

static void fox_draw(void)
{
	char buf[24];
	sbuf_t b;
	const radio_view_t *v = ui_view();
	fox_seen();
	int16_t dbm = v->rssi_dbm;
	if (dbm != 0 && (peak == 0 || dbm > peak))
		peak = dbm;
	gfx_clear(ui_col.bg);
	int y = ui_draw_top(v, &ui_st, TR("Fox hunt"), "") + 2;
	const font_t *f = ui_layout.small, *big = ui_layout.large;
	sb_init(&b, buf, sizeof buf);
	sb_freq(&b, v->ch.rx_hz);
	gfx_text(f, 0, y, buf, ui_col.fg);
	y += f->h + 4;
	int w = gfx_width();
	sb_init(&b, buf, sizeof buf);
	if (dbm == 0)
		sb_str(&b, "---");
	else
		sb_i32(&b, dbm);
	sb_str(&b, " dBm");
	gfx_text(big, (w - gfx_text_width(big, buf)) / 2, y, buf, ui_col.fg);
	y += big->h + 2;
	sb_init(&b, buf, sizeof buf);
	sb_s_units(&b, dbm);
	gfx_text(ui_layout.medium, (w - gfx_text_width(ui_layout.medium, buf)) / 2, y, buf, ui_col.rx);
	y += ui_layout.medium->h + 6;
	int bh = f->h + 6, bw = w - 2;
	gfx_rect(0, y, bw + 2, bh, ui_col.line);
	int x = bar_x(dbm, bw);
	if (x > 0)
		gfx_fill(1, y + 1, x, bh - 2, ui_col.rx);
	int px = bar_x(peak, bw);
	if (px > 0)
		gfx_vline(px, y, bh, ui_col.warn);
	y += bh + 4;
	sb_init(&b, buf, sizeof buf);
	sb_str(&b, TR("Peak "));
	if (peak == 0)
		sb_str(&b, "---");
	else
		sb_i32(&b, peak);
	gfx_text(f, 0, y, buf, ui_col.warn);
	sb_init(&b, buf, sizeof buf);
	sb_str(&b, TR("Att -"));
	sb_u32(&b, att, 1);
	sb_str(&b, "dB");
	gfx_text(f, w - gfx_text_width(f, buf), y, buf, ui_col.dim);
	ui_draw_bottom(&ui_st, TR("<> att  OK peak  Back"));
	ui_draw_banner(&ui_st);
}

static bool fox_key(const input_event_t *ev)
{
	bool press = ev->gesture == GESTURE_PRESS || ev->gesture == GESTURE_REPEAT;
	if (press && (ev->action == ACT_NAV_LEFT || ev->action == ACT_NAV_RIGHT)) {
		att = (uint8_t)((att + (ev->action == ACT_NAV_RIGHT ? 10u : 30u)) % 40u);
		radio_req_fox(true, att);
		ui_dirty();
		return true;
	}
	if (ev->gesture == GESTURE_PRESS && ev->action == ACT_SELECT) {
		peak = 0;
		ui_dirty();
		return true;
	}
	if (ev->gesture == GESTURE_PRESS && ev->action == ACT_BACK)
		radio_req_fox(false, 0);	/* and the screen's own Back leaves */
	return false;
}

const ui_scr_def_t ui_fox_screen = {
	.name = "fox", .flags = SF_FIELD, .key = fox_key, .draw = fox_draw, .enter = fox_enter,
};
#endif
