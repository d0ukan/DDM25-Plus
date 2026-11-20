/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM25 Plus Tools > Clock (clockx.h, docs/plus.md "Clock tools"): four rows, Up / Down pick one.
 *   Stopwatch   OK starts / stops, # takes a lap while it runs (the newest three under it with their own times) and
 *               resets it while stopped.
 *   Countdown   Left / Right (or the knob) set the time while stopped (a minute a step, 10 s under a minute; held:
 *               five steps), OK starts / stops it (back to the time set).
 *   Alarm       Left / Right set the time of day (a minute; held: 10), OK turns it on / off. Local time.
 *   UTC offset  Left / Right: half an hour a step (+03:00 at first).
 * The title shows the local time; without a GPS time since power on there is none, and the screen says so. The
 * countdown's end and the alarm sound and show a banner on any screen, as the satellite pass alarm does
 * (ui_clock_alarm, from ui_app_tick's banner path). RAM only: all off at power on.
 * Installed as core/ui/ui_clock.c. */
#include "config.h"
#if CONFIG_CLOCK_TOOLS
#include <string.h>
#include "ui_nav.h"
#include "ui_screens.h"
#include "ui_svc.h"
#include "fmt.h"
#include "i18n.h"
#include "gfx.h"
#include "radio/radio.h"
#include "radio/beep.h"
#include "ui/clockx.h"
#include "ui/ui_clock.h"

#define GPS_POLL_MS	1000u
#define SHOW_MS		200u	/* the stopwatch's tenths while it runs on top */

enum { R_SW, R_CD, R_AL, R_TZ, R_COUNT };

static clockx_t clk;
static bool clk_ready;
static uint8_t focus;
static uint32_t polled_ms, shown_ms;

static clockx_t *state(void)
{
	if (!clk_ready) {
		clk_init(&clk);
		clk_ready = true;
	}
	return &clk;
}

clockx_t *ui_clock_state(void)
{
	return state();
}

/* "1:02:03.4" / "2:03.4" (tenths) */
static void sb_watch(sbuf_t *b, uint32_t ms)
{
	uint32_t s = ms / 1000u;
	if (s >= 3600u) {
		sb_u32(b, s / 3600u, 1);
		sb_chr(b, ':');
		sb_u32(b, s / 60u % 60u, 2);
	} else {
		sb_u32(b, s / 60u, 1);
	}
	sb_chr(b, ':');
	sb_u32(b, s % 60u, 2);
	sb_chr(b, '.');
	sb_u32(b, ms / 100u % 10u, 1);
}

/* "07:30" from minutes of the day, "07:30:15" from seconds */
static void sb_hm(sbuf_t *b, uint32_t min)
{
	sb_u32(b, min / 60u % 24u, 2);
	sb_chr(b, ':');
	sb_u32(b, min % 60u, 2);
}

static void sb_tz(sbuf_t *b, int16_t z)
{
	sb_chr(b, z < 0 ? '-' : '+');
	sb_hm(b, (uint32_t)(z < 0 ? -z : z));
}

/* --- Every UI tick --- */

static void gps_feed(uint32_t now)
{
	ui_position_t p;
	if (now - polled_ms < GPS_POLL_MS)
		return;
	polled_ms = now;
	if (ui_svc_position(&p))
		clk_gps(state(), p.fix, p.utc_hms, now);
}

bool ui_clock_tick(uint8_t top, uint32_t now)
{
	gps_feed(now);
	if (top != SCR_CLOCK || now - shown_ms < (state()->sw_run ? SHOW_MS : 1000u))
		return false;
	shown_ms = now;
	return true;
}

const char *ui_clock_alarm(uint32_t now)
{
	static char text[24];
	if (!clk_ready)
		return NULL;	/* nothing set since power on */
	uint8_t ev = clk_tick(&clk, now);
	if (ev == 0)
		return NULL;
	sbuf_t b;
	sb_init(&b, text, sizeof text);
	if (ev & CLK_EV_ALARM) {
		sb_str(&b, TR("Alarm "));
		sb_hm(&b, clk.al_min);
	} else {
		sb_str(&b, TR("Countdown done"));
	}
	radio_req_beep(ev & CLK_EV_ALARM ? BEEP_SCAN_HOLD : BEEP_TOT_WARN);
	return text;
}

/* --- The screen --- */

static int __attribute__((noinline)) draw_row(int y, uint8_t r, const char *label, const char *value, color_t vc)
{
	const font_t *m = ui_layout.medium;
	int w = gfx_width();
	bool f = r == focus;
	if (f)
		gfx_fill(0, y - 1, w, m->h + 2, ui_col.focus);
	gfx_text(m, 0, y, label, f ? ui_col.focus_fg : ui_col.dim);
	gfx_text(m, w - gfx_text_width(m, value), y, value, f ? ui_col.focus_fg : vc);
	return y + m->h + 4;
}

static void __attribute__((noinline)) clock_draw(void)
{
	clockx_t *c = state();
	uint32_t now = ui_st.now_ms, sod;
	char buf[24];
	sbuf_t b;
	bool have = clk_local_s(c, now, &sod);
	sb_init(&b, buf, sizeof buf);
	if (have) {
		sb_hm(&b, sod / 60u);
		sb_chr(&b, ':');
		sb_u32(&b, sod % 60u, 2);
	}
	gfx_clear(ui_col.bg);
	int y = ui_draw_top(ui_view(), &ui_st, TR("Clock"), buf) + 4;
	const font_t *s = ui_layout.small;
	/* Stopwatch and its laps */
	sb_init(&b, buf, sizeof buf);
	sb_watch(&b, clk_sw_ms(c, now));
	y = draw_row(y, R_SW, TR("Stopwatch"), buf, c->sw_run ? ui_col.rx : ui_col.fg);
	for (uint8_t j = 0; j < 3u; j++) {
		uint32_t lap;
		uint8_t n;
		if (!clk_sw_lap(c, j, &lap, &n))
			break;
		sb_init(&b, buf, sizeof buf);
		sb_str(&b, TR("Lap "));
		sb_u32(&b, n, 1);
		sb_str(&b, "  ");
		sb_watch(&b, lap);
		gfx_text(s, s->w * 2, y, buf, ui_col.dim);
		y += s->h + 1;
	}
	y += 2;
	sb_init(&b, buf, sizeof buf);
	sb_mmss(&b, clk_cd_left_s(c, now));
	y = draw_row(y, R_CD, TR("Countdown"), buf, c->cd_run ? ui_col.rx : ui_col.fg);
	sb_init(&b, buf, sizeof buf);
	sb_hm(&b, c->al_min);
	sb_chr(&b, ' ');
	sb_str(&b, c->al_on ? TR("On") : TR("Off"));
	y = draw_row(y, R_AL, TR("Alarm"), buf, c->al_on ? ui_col.rx : ui_col.dim);
	sb_init(&b, buf, sizeof buf);
	sb_tz(&b, c->tz_min);
	y = draw_row(y, R_TZ, TR("UTC offset"), buf, ui_col.fg);
	/* The clock's source */
	if (!have)
		gfx_text(s, 0, y + 2, TR("No clock: needs GPS"), ui_col.warn);
	else if (c->al_on)
		gfx_text(s, 0, y + 2, TR("Rings on any screen"), ui_col.dim);
	const char *hint = focus == R_SW ? (c->sw_run ? TR("OK stop  # lap  ^v row") : TR("OK start  # reset  ^v row")) :
			   focus == R_CD ? (c->cd_run ? TR("OK stop  ^v row") : TR("OK start  <> time  ^v row")) :
			   focus == R_AL ? TR("OK on/off  <> time  ^v row") : TR("<> offset  ^v row");
	ui_draw_bottom(&ui_st, hint);
	ui_draw_banner(&ui_st);
}

static void adjust(clockx_t *c, int dir, bool fast)
{
	switch (focus) {
	case R_CD: {
		int32_t step = c->cd_set_s < 60u || (c->cd_set_s == 60u && dir < 0) ? 10 : 60;
		clk_cd_adjust(c, dir * step * (fast ? 5 : 1));
		break;
	}
	case R_AL:
		clk_al_adjust(c, dir * (fast ? 10 : 1));
		break;
	case R_TZ:
		clk_tz_adjust(c, dir * 30);
		break;
	default:
		break;
	}
}

static bool clock_key(const input_event_t *ev)
{
	clockx_t *c = state();
	uint32_t now = ui_st.now_ms;
	bool press = ev->gesture == GESTURE_PRESS || ev->gesture == GESTURE_REPEAT;
	if (press && (ev->action == ACT_NAV_UP || ev->action == ACT_NAV_DOWN)) {
		if (ev->action == ACT_NAV_UP && focus > 0)
			focus--;
		else if (ev->action == ACT_NAV_DOWN && focus + 1u < R_COUNT)
			focus++;
		ui_dirty();
		return true;
	}
	if ((press && (ev->action == ACT_NAV_LEFT || ev->action == ACT_NAV_RIGHT)) ||
	    (ev->action == ACT_ENC_STEP && ev->value != 0)) {
		int dir = ev->action == ACT_ENC_STEP ? (ev->value > 0 ? 1 : -1) : ev->action == ACT_NAV_RIGHT ? 1 : -1;
		adjust(c, dir, ev->gesture == GESTURE_REPEAT);
		ui_dirty();
		return true;
	}
	if (ev->gesture == GESTURE_PRESS && ev->action == ACT_SELECT) {
		if (focus == R_SW)
			clk_sw_toggle(c, now);
		else if (focus == R_CD)
			clk_cd_toggle(c, now);
		else if (focus == R_AL)
			clk_al_toggle(c);
		ui_dirty();
		return true;
	}
	if (ev->gesture == GESTURE_PRESS && ev->action == ACT_CONTEXT && focus == R_SW) {
		clk_sw_hash(c, now);
		ui_dirty();
		return true;
	}
	return false;
}

static void clock_enter(void)
{
	(void)state();
	ui_dirty();
}

const ui_scr_def_t ui_clock_screen = {
	.name = "clock", .flags = SF_FIELD, .key = clock_key, .draw = clock_draw, .enter = clock_enter,
};
#endif
