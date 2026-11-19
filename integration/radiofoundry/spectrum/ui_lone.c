/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus Tools > Lone worker (lone.h): Left / Right pick the time (Off, 5 .. 60 min), Up / Down what the alarm
 * does besides the tone (Sound only, DMR emergency, APRS SOS: what the build has). The screen shows the state, the
 * time left and, in the alarm, what was sent or why nothing was. The timer runs whatever screen is open (ui.c). */
#include "config.h"
#if CONFIG_LONE_WORKER
#include "ui_nav.h"
#include "ui_screens.h"
#include "fmt.h"
#include "i18n.h"
#include "gfx.h"
#include "ui/lone.h"

static void lone_enter(void)
{
	ui_dirty();
}

const char *ui_lone_tx_text(void)
{
	switch (lone_tx()) {
	case LONE_TX_DMR: return TR("DMR emergency sent");
	case LONE_TX_SOS: return TR("SOS position sent");
	case LONE_TX_NO_DMR: return TR("Not sent: needs a DMR channel");
	case LONE_TX_NO_POS: return TR("SOS not sent: no position");
	case LONE_TX_NO_APRS: return TR("SOS not sent: APRS not ready");
	default: return NULL;
	}
}

static void lone_draw(void)
{
	static const char *const on_names[LONE_ON_COUNT] = { TRN("Sound only"), TRN("DMR emergency"), TRN("APRS SOS") };

	char buf[24];
	sbuf_t b;
	gfx_clear(ui_col.bg);
	int y = ui_draw_top(ui_view(), &ui_st, TR("Lone worker"), "") + 6;
	const font_t *m = ui_layout.medium, *big = ui_layout.large;
	int w = gfx_width();
	sb_init(&b, buf, sizeof buf);
	uint8_t c = lone_choice();
	if (c == 0) {
		sb_str(&b, TR("Off"));
	} else {
		sb_u32(&b, lone_minutes(c), 1);
		sb_str(&b, TR(" min"));
	}
	gfx_text(big, (w - gfx_text_width(big, buf)) / 2, y, buf, c ? ui_col.fg : ui_col.dim);
	y += big->h + 6;
	lone_on_t on = lone_on();
	int x = gfx_text(m, 0, y, TR("On alarm"), ui_col.dim);
	gfx_text(m, x + m->w, y, tr(on_names[on]), on == LONE_ON_SOUND ? ui_col.fg : ui_col.warn);
	y += m->h + 6;
	lone_state_t s = lone_state();
	color_t col = ui_col.dim;
	sb_init(&b, buf, sizeof buf);
	if (s == LONE_WATCH) {
		sb_str(&b, TR("Alarm in "));
		sb_mmss(&b, (lone_left_ms(ui_st.now_ms) + 999u) / 1000u);
		col = ui_col.fg;
	} else if (s == LONE_WARN) {
		sb_str(&b, TR("Press a key! "));
		sb_mmss(&b, (lone_left_ms(ui_st.now_ms) + 999u) / 1000u);
		col = ui_col.warn;
	} else if (s == LONE_ALARM) {
		sb_str(&b, TR("ALARM"));
		col = ui_col.tx;
	}
	gfx_text(m, (w - gfx_text_width(m, buf)) / 2, y, buf, col);
	y += m->h + 6;
	const char *sent = ui_lone_tx_text();
	if (sent != NULL) {
		lone_tx_t t = lone_tx();
		gfx_text(ui_layout.small, 0, y, sent, t == LONE_TX_DMR || t == LONE_TX_SOS ? ui_col.tx : ui_col.warn);
	} else {
		gfx_text(ui_layout.small, 0, y, TR("No key or PTT this long: warning,"), ui_col.dim);
		if (on == LONE_ON_SOUND)
			gfx_text(ui_layout.small, 0, y + ui_layout.small->h + 1, TR("then the alarm until a key"), ui_col.dim);
		else
			gfx_text(ui_layout.small, 0, y + ui_layout.small->h + 1, TR("then the alarm and it TRANSMITS"), ui_col.warn);
	}
	ui_draw_bottom(&ui_st, TR("<> time  ^v on alarm"));
	ui_draw_banner(&ui_st);
}

static bool lone_key(const input_event_t *ev)
{
	bool press = ev->gesture == GESTURE_PRESS || ev->gesture == GESTURE_REPEAT;
	if (press && (ev->action == ACT_NAV_LEFT || ev->action == ACT_NAV_RIGHT)) {
		uint8_t c = (uint8_t)((lone_choice() + (ev->action == ACT_NAV_RIGHT ? 1u : LONE_CHOICES - 1u)) % LONE_CHOICES);
		lone_set(c, ui_st.now_ms);
		ui_dirty();
		return true;
	}
	if (press && (ev->action == ACT_NAV_UP || ev->action == ACT_NAV_DOWN)) {
		lone_on_step(ev->action == ACT_NAV_DOWN);
		ui_dirty();
		return true;
	}
	return false;
}

const ui_scr_def_t ui_lone_screen = {
	.name = "lone", .flags = SF_FIELD, .key = lone_key, .draw = lone_draw, .enter = lone_enter,
};
#endif
