/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus search ranges: presets, the range line and the range editor (ui_srange.h). Installed as
 * core/ui/ui_srange.c. */
#include "config.h"
#if CONFIG_SEARCH_RANGE
#include <string.h>
#include "board.h"
#include "ui_nav.h"
#include "ui_screens.h"
#include "fmt.h"
#include "i18n.h"
#include "gfx.h"
#include "radio/srange.h"
#include "ui/ui_srange.h"

#define MHZ		1000000u

static const char *const preset_names[SRANGE_P_MANUAL] = { "+-500 kHz", "+-1 MHz", "+-2.5 MHz", "430-440 MHz",
							     "144-146 MHz" };
/* The steps in the editor's order (smallest first): srange.h's tags */
static const uint8_t step_order[SRANGE_STEPS] = { 1, 2, 3, 4, 0, 5, 6, 7, 8, 9 };

enum { ROW_START, ROW_END, ROW_STEP, ROW_MOVE, ROW_COUNT };

/* The editor: the range being edited, the row, the digits typed into Start or End */
static struct {
	ui_srange_t *r;		/* NULL = closed */
	uint32_t lo, hi;
	uint8_t step, row, len;
	char digits[UI_ENTRY_MAX + 1];
} ed;

void ui_srange_bounds(const ui_srange_t *r, uint32_t center_hz, uint32_t *lo, uint32_t *hi)
{
	static const uint32_t half[3] = { 500000u, 1000000u, 2500000u };
	uint8_t p = r->preset;
	if (p == SRANGE_P_MANUAL && r->lo_hz != 0 && r->hi_hz > r->lo_hz) {
		*lo = r->lo_hz;
		*hi = r->hi_hz;
	} else if (p <= SRANGE_P_2M5) {
		center_hz -= center_hz % 12500u;
		*lo = center_hz - half[p];
		*hi = center_hz + half[p];
	} else if (p == SRANGE_P_VHF) {
		*lo = 144u * MHZ;
		*hi = 146u * MHZ;
	} else {
		*lo = 430u * MHZ;
		*hi = 440u * MHZ;
	}
}

uint32_t ui_srange_request(const ui_srange_t *r, uint32_t center_hz, uint32_t *hi)
{
	uint32_t lo;
	ui_srange_bounds(r, center_hz, &lo, hi);
	return srange_tag(lo, r->step);
}

void ui_srange_turn(ui_srange_t *r, int dir)
{
	uint8_t n = r->lo_hz != 0 ? SRANGE_P_COUNT : SRANGE_P_MANUAL;	/* the manual one once it is set */
	uint8_t p = r->preset < n ? r->preset : 0u;
	r->preset = (uint8_t)((p + (dir > 0 ? 1u : n - 1u)) % n);
}

/* "433.5", "430", "433.00625": MHz with the decimals it needs */
static void sr_mhz(sbuf_t *b, uint32_t hz)
{
	sb_u32(b, hz / MHZ, 1);
	uint32_t f = hz % MHZ / 10u;	/* 10 Hz units, 5 digits */
	if (f == 0)
		return;
	uint8_t d = 5;
	while (f % 10u == 0) {
		f /= 10u;
		d--;
	}
	sb_chr(b, '.');
	sb_u32(b, f, d);
}

/* "12.5 kHz", "1 MHz" (short: "12.5k", "1M") */
static void sr_step(sbuf_t *b, uint8_t tag, bool brief)
{
	uint32_t hz = srange_step_hz(tag);
	if (hz >= MHZ) {
		sb_u32(b, hz / MHZ, 1);
		sb_str(b, brief ? "M" : " MHz");
		return;
	}
	sb_u32(b, hz / 1000u, 1);
	uint32_t f = hz % 1000u;
	if (f != 0) {
		sb_chr(b, '.');
		sb_u32(b, f % 100u == 0 ? f / 100u : f % 10u == 0 ? f / 10u : f, f % 100u == 0 ? 1 : f % 10u == 0 ? 2 : 3);
	}
	sb_str(b, brief ? "k" : " kHz");
}

static void sr_range(sbuf_t *b, const ui_srange_t *r)
{
	if (r->preset < SRANGE_P_MANUAL) {
		sb_str(b, preset_names[r->preset]);
		return;
	}
	sr_mhz(b, r->lo_hz);
	sb_chr(b, '-');
	sr_mhz(b, r->hi_hz);
}

void ui_srange_line(const ui_srange_t *r, const font_t *f, int max_w, char *out, unsigned len)
{
	/* "Range +-1 MHz  12.5k"; narrower: without "Range "; a long manual range alone (the editor shows its step) */
	for (int form = 0; form < 3; form++) {
		sbuf_t b;
		sb_init(&b, out, len);
		if (form == 0)
			sb_str(&b, TR("Range "));
		sr_range(&b, r);
		if (form < 2) {
			sb_str(&b, "  ");
			sr_step(&b, r->step, true);
		}
		if (b.p < b.end && gfx_text_width(f, out) <= max_w)
			return;
	}
}

/* --- The editor --- */

/* lo..hi lies in one transmit band (the receiver the searches step), its end above its start */
static bool range_ok(uint32_t lo, uint32_t hi)
{
	int band = board_tx_band(lo);
	return band >= 0 && hi > lo && board_tx_band(hi) == band;
}

void ui_srange_open(ui_srange_t *r, uint32_t center_hz, char digit)
{
	memset(&ed, 0, sizeof ed);
	ed.r = r;
	ui_srange_bounds(r, center_hz, &ed.lo, &ed.hi);
	ed.step = r->step;
	if (digit != 0 && ui_freq_entry_digit(ed.digits, &ed.len, digit))
		ed.row = ROW_START;
	ui_dirty();
}

void ui_srange_close(void)
{
	ed.r = NULL;
	ed.len = 0;
}

bool ui_srange_editing(void)
{
	return ed.r != NULL;
}

/* The typed digits into Start or End: false (with the entry's toast) when they make no frequency in a band */
static bool commit(void)
{
	if (ed.len == 0)
		return true;
	uint32_t hz = ui_freq_entry_hz(ed.digits, ed.len);
	if (hz == 0)
		return false;
	if (ed.row == ROW_START) {
		uint32_t span = ed.hi > ed.lo ? ed.hi - ed.lo : 0u;
		ed.lo = hz;
		if (ed.hi <= hz || !range_ok(hz, ed.hi))
			ed.hi = range_ok(hz, hz + span) ? hz + span : hz;	/* End follows, keeping the width when it can */
	} else {
		ed.hi = hz;
	}
	ed.len = 0;
	return true;
}

/* Start (ROW_START), End (ROW_END) or both (ROW_MOVE) by delta Hz. Not made when an end it moves would leave the bands
 * or the end would come down to the start (one band for both is checked when the range is applied) */
static void move(int32_t delta)
{
	uint32_t lo = ed.lo, hi = ed.hi;
	if (ed.row != ROW_END)
		lo = (uint32_t)((int32_t)lo + delta);
	if (ed.row != ROW_START)
		hi = (uint32_t)((int32_t)hi + delta);
	if (hi <= lo || (lo != ed.lo && board_tx_band(lo) < 0) || (hi != ed.hi && board_tx_band(hi) < 0))
		return;
	ed.lo = lo;
	ed.hi = hi;
}

static uint8_t step_place(uint8_t tag)
{
	for (uint8_t i = 0; i < SRANGE_STEPS; i++)
		if (step_order[i] == tag)
			return i;
	return 4u;
}

static int apply(void)
{
	if (!commit())
		return UI_SRANGE_KEPT;
	if (ed.hi <= ed.lo) {
		ui_toast(TR("Upper must be above Lower"));
		return UI_SRANGE_KEPT;
	}
	if (!range_ok(ed.lo, ed.hi)) {
		ui_toast(TR("Range must lie in one band"));
		return UI_SRANGE_KEPT;
	}
	ed.r->preset = SRANGE_P_MANUAL;
	ed.r->lo_hz = ed.lo;
	ed.r->hi_hz = ed.hi;
	ed.r->step = ed.step;
	ui_srange_close();
	return UI_SRANGE_APPLIED;
}

int ui_srange_key(const input_event_t *ev)
{
	if (ed.r == NULL)
		return UI_SRANGE_NONE;
	bool press = ev->gesture == GESTURE_PRESS;
	bool held = ev->gesture == GESTURE_LONG || ev->gesture == GESTURE_REPEAT;
	ui_dirty();
	if (press && ev->action >= ACT_DIGIT_0 && ev->action <= ACT_DIGIT_9) {
		if (ed.row != ROW_START && ed.row != ROW_END)
			return UI_SRANGE_KEPT;	/* digits type a frequency */
		(void)ui_freq_entry_digit(ed.digits, &ed.len, (char)('0' + (ev->action - ACT_DIGIT_0)));
		return UI_SRANGE_KEPT;
	}
	if (press && ev->action == ACT_BACK) {
		if (ed.len != 0) {
			ed.len--;	/* Back takes the last digit first */
			ed.digits[ed.len] = 0;
			return UI_SRANGE_KEPT;
		}
		ui_srange_close();
		return UI_SRANGE_LEFT;
	}
	if (press && ev->action == ACT_SELECT) {
		/* Typing Start: OK takes it and goes on to End; otherwise OK applies the range */
		if (ed.len != 0 && ed.row == ROW_START) {
			if (commit())
				ed.row = ROW_END;
			return UI_SRANGE_KEPT;
		}
		return apply();
	}
	if ((press || held) && (ev->action == ACT_NAV_UP || ev->action == ACT_NAV_DOWN)) {
		if (!commit())
			return UI_SRANGE_KEPT;
		ed.row = (uint8_t)((ed.row + (ev->action == ACT_NAV_DOWN ? 1u : ROW_COUNT - 1u)) % ROW_COUNT);
		return UI_SRANGE_KEPT;
	}
	/* Left / Right (the knob too): -/+ a step; held (the DM-32's long Left / Right are USER_1 / USER_4): -/+ 1 MHz */
	int dir = 0;
	bool mhz = held;
	if ((press || held) && (ev->action == ACT_NAV_LEFT || ev->action == ACT_NAV_RIGHT))
		dir = ev->action == ACT_NAV_RIGHT ? 1 : -1;
	else if (held && (ev->action == ACT_USER_1 || ev->action == ACT_USER_4))
		dir = ev->action == ACT_USER_4 ? 1 : -1;
	else if (ev->action == ACT_ENC_STEP && ev->value != 0) {
		dir = ev->value > 0 ? 1 : -1;
		mhz = false;
	}
	if (dir == 0)
		return UI_SRANGE_KEPT;	/* the editor has the screen: nothing else acts under it */
	if (!commit())
		return UI_SRANGE_KEPT;
	if (ed.row == ROW_STEP) {
		uint8_t i = step_place(ed.step);
		ed.step = step_order[(i + (dir > 0 ? 1u : SRANGE_STEPS - 1u)) % SRANGE_STEPS];
	} else {
		move(dir * (int32_t)(mhz ? MHZ : srange_step_hz(ed.step)));
	}
	return UI_SRANGE_KEPT;
}

/* "43_.____" while typed */
static void sr_typed(sbuf_t *b)
{
	for (uint8_t i = 0; i < UI_ENTRY_MAX; i++) {
		if (i == UI_ENTRY_MHZ)
			sb_chr(b, '.');
		sb_chr(b, i < ed.len ? ed.digits[i] : '_');
	}
}

void ui_srange_draw(int y)
{
	static const char *const labels[ROW_COUNT] = { TRN("Lower"), TRN("Upper"), TRN("Step"), TRN("Move") };
	const font_t *s = ui_layout.small, *m = ui_layout.medium;
	char buf[40];
	sbuf_t b;
	gfx_text(s, 0, y, TR("Search range, MHz"), ui_col.dim);
	y += s->h + 4;
	int lw = 0;
	for (uint8_t i = 0; i < ROW_COUNT; i++) {
		int w = gfx_text_width(m, tr(labels[i]));
		if (w > lw)
			lw = w;
	}
	lw += m->w;
	for (uint8_t i = 0; i < ROW_COUNT; i++) {
		bool on = i == ed.row;
		if (on)
			gfx_fill(0, y - 1, gfx_width(), m->h + 2, ui_col.focus);
		color_t c = on ? ui_col.focus_fg : ui_col.fg;
		gfx_text(m, 0, y, tr(labels[i]), on ? ui_col.focus_fg : ui_col.dim);
		sb_init(&b, buf, sizeof buf);
		if (on && ed.len != 0) {
			sr_typed(&b);
		} else if (i == ROW_START || i == ROW_END) {
			sr_mhz(&b, i == ROW_START ? ed.lo : ed.hi);	/* MHz, as the title says */
		} else if (i == ROW_STEP) {
			sr_step(&b, ed.step, false);
		} else {
			uint32_t w = ed.hi > ed.lo ? ed.hi - ed.lo : 0u;	/* the width moved with the range */
			if (w >= MHZ) {
				sr_mhz(&b, w);
				sb_str(&b, " MHz");
			} else {
				sb_u32(&b, w / 1000u, 1);
				sb_str(&b, " kHz");
			}
		}
		gfx_text(m, lw, y, buf, c);
		y += m->h + 3;
	}
	y += 2;
	gfx_text(s, 0, y, TR("<> step, hold <> 1 MHz"), ui_col.dim);
	y += s->h + 1;
	gfx_text(s, 0, y, TR("0-9 type Lower / Upper"), ui_col.dim);
}

const char *ui_srange_hint(void)
{
	return ed.len != 0 && ed.row == ROW_START ? TR("OK next  Back delete") :
	       ed.len != 0 ? TR("OK apply  Back delete") : TR("OK apply  Back cancel");
}
#endif
