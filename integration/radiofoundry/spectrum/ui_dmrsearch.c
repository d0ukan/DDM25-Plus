/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus Tools > DMR search (radio/dmrsearch.h): find DMR in a range whatever its color code, slot and talkgroup.
 * Left / Right pick the range (around the frequency in use, or a band), OK starts or stops; the screen shows the
 * sweep, the probe or the call being played, and the found list (frequency, color code, slots, repeater or direct,
 * how long ago; the last call's talkgroup and source below). Up / Down pick a find; OK on it puts the VFO there in
 * DMR with its color code and slot. Back stops the search; leaving the screen otherwise stops it after a few
 * seconds (DMRS_IDLE_MS).
 * CONFIG_FIND_SAVE (ui/findsave.h): # saves every find into a new zone "Bulunanlar N"; OK held on a picked find saves
 * that one (into the newest such zone). The list stays as it was while the save runs. */
#include "config.h"
#if CONFIG_DMR_SEARCH
#include <string.h>
#include "ui_nav.h"
#include "ui_screens.h"
#include "fmt.h"
#include "i18n.h"
#include "gfx.h"
#include "radio/radio.h"
#include "radio/dmrsearch.h"
#include "ui/ui_scratch.h"
#if CONFIG_SEARCH_RANGE
#include "ui/ui_srange.h"
#endif
#if CONFIG_FIND_SAVE
#include "ui/findsave.h"
#endif

#if CONFIG_SEARCH_RANGE
/* The range and step (DDM32 Plus search ranges, ui_srange.h): kept while the radio is on */
static ui_srange_t rng = UI_SRANGE_INIT;
#else
enum { R_500K, R_1M, R_2M5, R_UHF, R_VHF, R_COUNT };
static const char *const range_names[R_COUNT] = { "+-500 kHz", "+-1 MHz", "+-2.5 MHz", "430-440 MHz", "144-146 MHz" };
static uint8_t range_i = R_1M;
#endif
static uint32_t sel_hz;		/* the find picked with Up / Down, 0 = none */
/* The search as last copied: the tool screens' shared buffer (ui_scratch.h), refilled by snap() in each draw and key */
#define sv	(ui_scratch.dmrs)

/* The search's state; while finds are being saved, the list as the save began (still in the buffer unless another
 * screen has taken it since: then the save stops, ui_dmrsearch_find) */
static bool snap(void)
{
#if CONFIG_FIND_SAVE
	if (ui_findsave_busy() && ui_scratch_mine(UI_SCRATCH_DMRS))
		return true;
#endif
	ui_scratch_take(UI_SCRATCH_DMRS);
	return dmrs_snapshot(&sv);
}

#if CONFIG_FIND_SAVE
bool ui_dmrsearch_find(uint8_t i, findsave_item_t *out)
{
	if (!ui_scratch_mine(UI_SCRATCH_DMRS) || i >= sv.found_n)
		return false;
	const dmrs_found_t *f = &sv.found[i];
	memset(out, 0, sizeof *out);
	out->kind = FINDSAVE_DMR;
	out->hz = f->hz;
	out->cc = f->cc != DMRS_CC_NONE && f->cc <= 15u ? f->cc : FINDSAVE_CC_NONE;
	out->ts_mask = f->ts_mask;
	return true;
}
#endif

#if !CONFIG_SEARCH_RANGE
static void range_of(uint8_t r, uint32_t center, uint32_t *lo, uint32_t *hi)
{
	static const uint32_t half[3] = { 500000u, 1000000u, 2500000u };
	if (r <= R_2M5) {
		center -= center % 12500u;
		*lo = center - half[r];
		*hi = center + half[r];
	} else if (r == R_UHF) {
		*lo = 430000000u;
		*hi = 440000000u;
	} else {
		*lo = 144000000u;
		*hi = 146000000u;
	}
}
#endif

static void sb_mhz4(sbuf_t *b, uint32_t hz)
{
	sb_u32(b, hz / 1000000u, 1);
	sb_chr(b, '.');
	sb_u32(b, (hz / 100u) % 10000u, 4);
}

/* The picked find's row in the snapshot, -1 = none */
static int sel_row(void)
{
	for (uint8_t i = 0; sel_hz != 0 && i < sv.found_n; i++)
		if (sv.found[i].hz == sel_hz)
			return i;
	return -1;
}

/* "433.5000 CC1 TS1+2 RPT 3m" */
static void found_line(sbuf_t *b, const dmrs_found_t *f)
{
	sb_mhz4(b, f->hz);
	if (f->cc != DMRS_CC_NONE) {
		sb_str(b, " CC");
		sb_u32(b, f->cc, 1);
	}
	if (f->ts_mask & 3u) {
		sb_str(b, " TS");
		sb_str(b, (f->ts_mask & 3u) == 3u ? "1+2" : (f->ts_mask & 1u) ? "1" : "2");
	}
	if (f->sync_mask & DMRS_SYNC_BS)
		sb_str(b, " RPT");
	if (f->sync_mask & DMRS_SYNC_MS)
		sb_str(b, " DIR");
	sb_chr(b, ' ');
	sb_age(b, ui_st.now_ms - f->heard_ms);
}

static void dmrs_draw(void)
{
	char buf[40];
	sbuf_t b;
	bool have = snap();
	gfx_clear(ui_col.bg);
	int y = ui_draw_top(ui_view(), &ui_st, TR("DMR search"), "") + 2;
	const font_t *s = ui_layout.small, *m = ui_layout.medium;
#if CONFIG_SEARCH_RANGE
	ui_srange_line(&rng, s, gfx_width(), buf, sizeof buf);
	gfx_text(s, 0, y, buf, ui_col.dim);
	y += s->h + 3;
	if (ui_srange_editing()) {	/* the range editor has the page */
		ui_srange_draw(y);
		ui_draw_bottom(&ui_st, ui_srange_hint());
		ui_draw_banner(&ui_st);
		return;
	}
#else
	sb_init(&b, buf, sizeof buf);
	sb_str(&b, TR("Range "));
	sb_str(&b, range_names[range_i]);
	gfx_text(s, 0, y, buf, ui_col.dim);
	y += s->h + 3;
#endif
	sb_init(&b, buf, sizeof buf);
	color_t c = ui_col.dim;
	if (!have || sv.phase == DMRS_OFF) {
		sb_str(&b, TR("Stopped: OK starts"));
	} else if (sv.phase == DMRS_SWEEP) {
		sb_str(&b, TR("Sweep "));
		sb_mhz4(&b, sv.at_hz);
		c = ui_col.fg;
	} else if (sv.playing) {
		sb_str(&b, "DMR ");
		sb_mhz4(&b, sv.at_hz);
		c = ui_col.rx;
	} else {
		sb_str(&b, TR("Probe "));
		sb_mhz4(&b, sv.at_hz);
		c = ui_col.warn;
	}
	gfx_text(m, 0, y, buf, c);
	y += m->h + 2;
#if CONFIG_FIND_SAVE
	if (ui_findsave_line(buf, sizeof buf)) {
		gfx_text(s, 0, y, buf, ui_col.warn);
	} else
#endif
	if (have && sv.phase != DMRS_OFF) {
		sb_init(&b, buf, sizeof buf);
		sb_str(&b, TR("Pass "));
		sb_u32(&b, sv.sweeps + 1u, 1);
		sb_str(&b, TR("  hits "));
		sb_u32(&b, sv.hits, 1);
		sb_str(&b, TR("  floor "));
		sb_i32(&b, sv.floor_dbm);
		gfx_text(s, 0, y, buf, ui_col.dim);
	}
	y += s->h + 4;
	gfx_hline(0, y, gfx_width(), ui_col.line);
	y += 3;
	if (!have || sv.found_n == 0)
		gfx_text(s, 0, y, TR("No DMR found yet"), ui_col.dim);
	int sel = have ? sel_row() : -1;
	for (uint8_t i = 0; have && i < sv.found_n && y + s->h < ui_layout.hint_y; i++) {
		const dmrs_found_t *f = &sv.found[i];
		bool picked = (int)i == sel;
		bool more = f->dst != 0 || f->src != 0 || f->enc;
		int rows = more && y + 2 * (s->h + 1) <= ui_layout.hint_y ? 2 : 1;
		if (picked)
			gfx_fill(0, y - 1, gfx_width(), rows * (s->h + 1) + 1, ui_col.focus);
		sb_init(&b, buf, sizeof buf);
		found_line(&b, f);
		gfx_text(s, 0, y, buf, picked ? ui_col.focus_fg : i == 0 && sv.playing ? ui_col.rx : ui_col.fg);
		y += s->h + 1;
		if (rows == 2) {
			sb_init(&b, buf, sizeof buf);
			sb_str(&b, "   ");
			if (f->dst != 0) {
				sb_str(&b, f->group ? "TG " : "ID ");
				sb_u32(&b, f->dst, 1);
				sb_chr(&b, ' ');
			}
			if (f->src != 0) {
				sb_str(&b, TR("from "));
				sb_u32(&b, f->src, 1);
				sb_chr(&b, ' ');
			}
			if (f->enc)
				sb_str(&b, TR("Encrypted"));
			gfx_text(s, 0, y, buf, picked ? ui_col.focus_fg : f->enc ? ui_col.warn : ui_col.dim);
			y += s->h + 1;
		}
	}
	const char *hint = sel >= 0 ? TR("OK tune  ^v pick") :
			   have && sv.phase != DMRS_OFF ? TR("OK stop  ^v pick") : TR("OK start  <> range");
#if CONFIG_SEARCH_RANGE
	if (sel < 0 && !(have && sv.phase != DMRS_OFF))
		hint = TR(TEXT_ALT_OPEN "OK start  <> range  * edit" TEXT_ALT_NEXT "OK start  * range" TEXT_ALT_CLOSE);
#endif
#if CONFIG_FIND_SAVE
	if (sel >= 0)
		hint = TR(TEXT_ALT_OPEN "OK tune  hold OK save  ^v pick" TEXT_ALT_NEXT "OK tune  hold OK save"
			  TEXT_ALT_CLOSE);
	else if (have && sv.found_n != 0)
		hint = sv.phase != DMRS_OFF ?
		       TR(TEXT_ALT_OPEN "OK stop  ^v pick  # save all" TEXT_ALT_NEXT "OK stop  # save all" TEXT_ALT_CLOSE) :
		       TR(TEXT_ALT_OPEN "OK start  ^v pick  # save all" TEXT_ALT_NEXT "OK start  # save all" TEXT_ALT_CLOSE);
#endif
	ui_draw_bottom(&ui_st, hint);
	ui_draw_banner(&ui_st);
}

static void restart(void)
{
	uint32_t lo, hi;
#if CONFIG_SEARCH_RANGE
	lo = ui_srange_request(&rng, ui_view()->ch.rx_hz, &hi);	/* the step rides on lo (srange.h) */
#else
	range_of(range_i, ui_view()->ch.rx_hz, &lo, &hi);
#endif
	radio_req_dmr_search(lo, hi);
	ui_dirty();
}

/* OK on a find: the search stops and the VFO goes there in DMR, with the color code and the slot when known */
static void tune_find(const dmrs_found_t *f)
{
	radio_req_dmr_search(0, 0);
	radio_req_vfo_set(f->hz);
	radio_req_mode(RADIO_MODE_DMR);
	if (f->cc != DMRS_CC_NONE)
		radio_req_dmr_color_code(f->cc);
	if ((f->ts_mask & 3u) == 1u || (f->ts_mask & 3u) == 2u)
		radio_req_dmr_timeslot((f->ts_mask & 1u) ? 1u : 2u);
	sel_hz = 0;
	ui_go_home();
	/* A repeater's output: the VFO is simplex there, so PTT would talk on the output; say so */
	if (f->sync_mask & DMRS_SYNC_BS)
		ui_toast(TR("Repeater: set the TX offset"));
}

static bool dmrs_key(const input_event_t *ev)
{
	bool press = ev->gesture == GESTURE_PRESS || ev->gesture == GESTURE_REPEAT;
	bool have = snap();
	bool on = have && sv.phase != DMRS_OFF;
#if CONFIG_SEARCH_RANGE
	/* The range editor: it takes every key while open; an applied range restarts a running search on it */
	int ek = ui_srange_key(ev);
	if (ek != UI_SRANGE_NONE) {
		if (ek == UI_SRANGE_APPLIED && on)
			restart();
		return true;
	}
	/* * opens it; a digit opens it typing Start */
	if (ev->gesture == GESTURE_PRESS && (ev->action == ACT_QUICK ||
					      (ev->action >= ACT_DIGIT_0 && ev->action <= ACT_DIGIT_9))) {
		ui_srange_open(&rng, ui_view()->ch.rx_hz,
			       ev->action == ACT_QUICK ? 0 : (char)('0' + (ev->action - ACT_DIGIT_0)));
		return true;
	}
#endif
	if (press && (ev->action == ACT_NAV_LEFT || ev->action == ACT_NAV_RIGHT)) {
#if CONFIG_SEARCH_RANGE
		ui_srange_turn(&rng, ev->action == ACT_NAV_RIGHT ? 1 : -1);
#else
		range_i = (uint8_t)((range_i + (ev->action == ACT_NAV_RIGHT ? 1u : R_COUNT - 1u)) % R_COUNT);
#endif
		if (on)
			restart();
		ui_dirty();
		return true;
	}
	if (press && (ev->action == ACT_NAV_UP || ev->action == ACT_NAV_DOWN)) {
		/* Down from nothing picks the newest; Up past the newest picks nothing again */
		int r = have ? sel_row() : -1;
		r += ev->action == ACT_NAV_DOWN ? 1 : -1;
		if (!have || r < 0 || sv.found_n == 0)
			sel_hz = 0;
		else
			sel_hz = sv.found[r < sv.found_n ? r : sv.found_n - 1].hz;
		ui_dirty();
		return true;
	}
#if CONFIG_FIND_SAVE
	if (ev->gesture == GESTURE_PRESS && ev->action == ACT_CONTEXT) {
		ui_findsave_start(FINDSAVE_DMR, 0, have ? sv.found_n : 0, false);	/* # saves them all */
		ui_dirty();
		return true;
	}
	if (ev->gesture == GESTURE_LONG && ev->action == ACT_SELECT) {
		int r = have ? sel_row() : -1;
		if (r >= 0)	/* OK held on a find saves that one (OK alone tunes it) */
			ui_findsave_start(FINDSAVE_DMR, (uint8_t)r, (uint8_t)(r + 1), true);
		ui_dirty();
		return true;
	}
#endif
	if (ev->gesture == GESTURE_PRESS && ev->action == ACT_SELECT) {
		int r = have ? sel_row() : -1;
		if (r >= 0)
			tune_find(&sv.found[r]);
		else if (on)
			radio_req_dmr_search(0, 0);
		else
			restart();
		ui_dirty();
		return true;
	}
	if (ev->gesture == GESTURE_PRESS && ev->action == ACT_BACK) {
		sel_hz = 0;
		radio_req_dmr_search(0, 0);	/* and the screen's own Back leaves */
	}
	return false;
}

static void dmrs_enter(void)
{
#if CONFIG_SEARCH_RANGE
	ui_srange_close();
#endif
	sel_hz = 0;
	ui_dirty();
}

const ui_scr_def_t ui_dmrsearch_screen = {
	.name = "dmrsearch", .flags = SF_FIELD, .key = dmrs_key, .draw = dmrs_draw, .enter = dmrs_enter,
};
#endif
