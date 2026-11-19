/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus Tools > P25 search (radio/p25search.h): find P25 in a range whatever its NAC and talkgroup. Left / Right
 * pick the range (around the frequency in use, or a band), OK starts or stops; the screen shows the sweep, the
 * probe or the call being played, and the found list (frequency, NAC, talkgroup, source, how long ago). Back stops
 * it; leaving the screen otherwise stops it after a few seconds (P25S_IDLE_MS). */
#include "config.h"
#if CONFIG_P25_SEARCH
#include "ui_nav.h"
#include "ui_screens.h"
#include "fmt.h"
#include "i18n.h"
#include "gfx.h"
#include "radio/radio.h"
#include "radio/p25search.h"

enum { R_500K, R_1M, R_2M5, R_UHF, R_VHF, R_COUNT };
static const char *const range_names[R_COUNT] = { "+-500 kHz", "+-1 MHz", "+-2.5 MHz", "430-440 MHz", "144-146 MHz" };
static uint8_t range_i = R_1M;
static p25s_view_t sv;

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

static void sb_mhz4(sbuf_t *b, uint32_t hz)
{
	sb_u32(b, hz / 1000000u, 1);
	sb_chr(b, '.');
	sb_u32(b, (hz / 100u) % 10000u, 4);
}

static void hexnac(sbuf_t *b, uint16_t nac)
{
	for (int sh = 8; sh >= 0; sh -= 4)
		sb_chr(b, "0123456789ABCDEF"[(nac >> sh) & 15u]);
}

static void p25s_draw(void)
{
	char buf[40];
	sbuf_t b;
	bool have = p25s_snapshot(&sv);
	gfx_clear(ui_col.bg);
	int y = ui_draw_top(ui_view(), &ui_st, TR("P25 search"), "") + 2;
	const font_t *s = ui_layout.small, *m = ui_layout.medium;
	sb_init(&b, buf, sizeof buf);
	sb_str(&b, TR("Range "));
	sb_str(&b, range_names[range_i]);
	gfx_text(s, 0, y, buf, ui_col.dim);
	y += s->h + 3;
	sb_init(&b, buf, sizeof buf);
	color_t c = ui_col.dim;
	if (!have || sv.phase == P25S_OFF) {
		sb_str(&b, TR("Stopped: OK starts"));
	} else if (sv.phase == P25S_SWEEP) {
		sb_str(&b, TR("Sweep "));
		sb_mhz4(&b, sv.at_hz);
		c = ui_col.fg;
	} else if (sv.playing) {
		sb_str(&b, TR("P25 "));
		sb_mhz4(&b, sv.at_hz);
		c = ui_col.rx;
	} else {
		sb_str(&b, TR("Probe "));
		sb_mhz4(&b, sv.at_hz);
		c = ui_col.warn;
	}
	gfx_text(m, 0, y, buf, c);
	y += m->h + 2;
	if (have && sv.phase != P25S_OFF) {
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
	if (!have || sv.found_n == 0) {
		gfx_text(s, 0, y, TR("No P25 found yet"), ui_col.dim);
	}
	for (uint8_t i = 0; have && i < sv.found_n && y + s->h < ui_layout.hint_y; i++) {
		const p25s_found_t *f = &sv.found[i];
		sb_init(&b, buf, sizeof buf);
		sb_mhz4(&b, f->hz);
		sb_str(&b, " $");
		hexnac(&b, f->nac);
#if CONFIG_NAC_DEC
		sb_chr(&b, '/');
		sb_u32(&b, f->nac, 1);	// decimal too (some factory radios take the NAC so)
#endif
		if (f->tg) {
			sb_str(&b, " TG");
			sb_u32(&b, f->tg, 1);
		}
		sb_chr(&b, ' ');
		sb_age(&b, ui_st.now_ms - f->heard_ms);
		gfx_text(s, 0, y, buf, i == 0 && sv.playing ? ui_col.rx : ui_col.fg);
		y += s->h + 1;
		if (f->src && y + s->h < ui_layout.hint_y) {
			sb_init(&b, buf, sizeof buf);
			sb_str(&b, "   ID ");
			sb_u32(&b, f->src, 1);
			gfx_text(s, 0, y, buf, ui_col.dim);
			y += s->h + 1;
		}
	}
	ui_draw_bottom(&ui_st, have && sv.phase != P25S_OFF ? TR("OK stop  Back") : TR("OK start  <> range"));
	ui_draw_banner(&ui_st);
}

static void restart(void)
{
	uint32_t lo, hi;
	range_of(range_i, ui_view()->ch.rx_hz, &lo, &hi);
	radio_req_p25_search(lo, hi);
	ui_dirty();
}

static bool p25s_key(const input_event_t *ev)
{
	bool press = ev->gesture == GESTURE_PRESS || ev->gesture == GESTURE_REPEAT;
	bool on = p25s_snapshot(&sv) && sv.phase != P25S_OFF;
	if (press && (ev->action == ACT_NAV_LEFT || ev->action == ACT_NAV_RIGHT)) {
		range_i = (uint8_t)((range_i + (ev->action == ACT_NAV_RIGHT ? 1u : R_COUNT - 1u)) % R_COUNT);
		if (on)
			restart();
		ui_dirty();
		return true;
	}
	if (ev->gesture == GESTURE_PRESS && ev->action == ACT_SELECT) {
		if (on)
			radio_req_p25_search(0, 0);
		else
			restart();
		ui_dirty();
		return true;
	}
	if (ev->gesture == GESTURE_PRESS && ev->action == ACT_BACK)
		radio_req_p25_search(0, 0);	/* and the screen's own Back leaves */
	return false;
}

static void p25s_enter(void)
{
	ui_dirty();
}

const ui_scr_def_t ui_p25search_screen = {
	.name = "p25search", .flags = SF_FIELD, .key = p25s_key, .draw = p25s_draw, .enter = p25s_enter,
};
#endif
