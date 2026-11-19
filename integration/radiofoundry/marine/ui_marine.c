/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus Tools > Marine VHF (marine.h): the international marine VHF channels in channel order, each with its use
 * and its ship and coast station frequencies. Up / Down / the knob: pick (round), Left / Right: ten on, #: channel 16,
 * OK: listen. Listening puts the VFO on the coast station frequency (the ship one for intership channels), FM, 25 kHz,
 * no tones, simplex (TX = RX), through the requests Tools > Repeaters' tune uses, and goes Home. It never transmits:
 * the radio core refuses TX while the VFO is on a frequency of this table (radio_sm_tx_check, TXDENY_RX_ONLY), and
 * channel 70 (DSC) and AIS are data only, listed but not tuned. 2 B of state. */
#include "config.h"
#if CONFIG_MARINE
#include "board.h"
#include "ui_nav.h"
#include "ui_screens.h"
#include "fmt.h"
#include "i18n.h"
#include "gfx.h"
#include "radio/radio.h"
#include "radio/marine.h"

static uint8_t sel, top;

static const char *use_text(uint8_t use)
{
	static const char *const uses[MAR_USES] = {
		[MAR_DUPLEX] = TRN("Public corr., port"),
		[MAR_SIMPLEX] = TRN("Port ops, intership"),
		[MAR_INTERSHIP] = TRN("Intership only"),
		[MAR_DISTRESS] = TRN("Distress, calling"),
		[MAR_DSC] = TRN("DSC data, no voice"),
		[MAR_SAR] = TRN("Intership, SAR"),
		[MAR_BRIDGE] = TRN("Bridge to bridge"),
		[MAR_ONBOARD] = TRN("On board, 1 W"),
		[MAR_NAV] = TRN("Navigation, 1 W"),
		[MAR_AIS] = TRN("AIS data, no voice"),
	};
	return use < MAR_USES ? tr(uses[use]) : "";
}

/* "156.800" */
static void sb_mhz3(sbuf_t *b, uint32_t hz)
{
	uint32_t khz = (hz + 500u) / 1000u;
	sb_u32(b, khz / 1000u, 1);
	sb_chr(b, '.');
	sb_u32(b, khz % 1000u, 3);
}

/* Ship / coast: "157.100 / 161.700" (duplex), "156.800" (one frequency), "156.300 / -" (ships only) */
static void sb_freqs(sbuf_t *b, const marine_ch_t *c)
{
	uint32_t ship = marine_ship_hz(c), coast = marine_coast_hz(c);
	sb_mhz3(b, ship);
	if (coast == ship)
		return;
	sb_str(b, " / ");
	if (coast == 0)
		sb_chr(b, '-');
	else
		sb_mhz3(b, coast);
}

/* "16" and its frequencies (medium), its use (small) */
static void __attribute__((noinline)) row(int y, int h, const marine_ch_t *c, bool focus)
{
	char buf[24], fit[40];
	sbuf_t b;
	const font_t *s = ui_layout.small, *m = ui_layout.medium;
	int w = gfx_width();
	if (focus)
		gfx_fill(0, y, w, h, ui_col.focus);
	bool data = c->use == MAR_DSC || c->use == MAR_AIS, warn = data || c->use == MAR_DISTRESS;
	color_t fg = focus ? ui_col.focus_fg : warn ? ui_col.warn : ui_col.fg, dim = focus ? ui_col.focus_fg : ui_col.dim;
	gfx_text(m, 2, y + 1, c->name, fg);
	sb_init(&b, buf, sizeof buf);
	sb_freqs(&b, c);
	gfx_text(m, w - gfx_text_width(m, buf) - 2, y + 1, buf, fg);
	gfx_text_fit(s, use_text(c->use), w - 4, fit, sizeof fit);
	gfx_text(s, 2, y + m->h + 2, fit, dim);
}

static void marine_draw(void)
{
	const font_t *s = ui_layout.small, *m = ui_layout.medium;
	gfx_clear(ui_col.bg);
	int y = ui_draw_top(ui_view(), &ui_st, TR("Marine VHF"), "") + 2;
	gfx_text(s, 0, y, TR("Ship/coast MHz, RX only"), ui_col.dim);
	y += s->h + 3;
	int rh = m->h + s->h + 4;
	int rows = (ui_layout.hint_y - y) / rh;
	if (rows < 1)
		rows = 1;
	if (sel >= marine_count)
		sel = 0;
	if (sel < top)
		top = sel;
	if (sel >= top + rows)
		top = (uint8_t)(sel - rows + 1);
	for (int k = 0; k < rows && top + k < marine_count; k++) {
		row(y, rh, &marine_table[top + k], top + k == sel);
		y += rh;
	}
	ui_draw_bottom(&ui_st, TR("OK listen  # ch 16  ^v pick"));
	ui_draw_banner(&ui_st);
}

/* OK: the VFO on the channel, receive only */
static void __attribute__((noinline)) listen_on(const marine_ch_t *c)
{
	static char done[40];
	uint32_t hz;
	if (!marine_listen_hz(c, &hz)) {
		ui_toast(c->use == MAR_DSC ? TR("Ch 70 is DSC data: not tuned for voice") : TR("AIS is data: not tuned"));
		return;
	}
	if (!CONFIG_VFO_FM) {
		ui_toast(TR("No VFO for this mode"));
		return;
	}
	if (ui_view()->state == RS_TX) {
		ui_toast(TR("Can't tune while transmitting"));
		return;
	}
	if (!board_in_band(hz, false)) {
		ui_toast(TR("Not in the radio's bands"));
		return;
	}
	subtone_t none = { TONE_NONE, 0 };
	radio_req_vfo_set(hz);		/* opens the VFO */
	radio_req_mode(RADIO_MODE_FM);
	radio_req_bandwidth(RADIO_BW_25K);
	radio_req_tone(RADIO_TONE_TX, none);
	radio_req_tone(RADIO_TONE_RX, none);
	radio_req_vfo_tx(0);		/* simplex: no split left from before (and TX is refused there anyway) */
	sbuf_t b;
	sb_init(&b, done, sizeof done);
	sb_str(&b, TR("Ch "));
	sb_str(&b, c->name);
	sb_chr(&b, ' ');
	sb_mhz3(&b, hz);
	sb_str(&b, TR(": RX only"));
	ui_toast(done);
	ui_go_home();
}

static bool marine_key(const input_event_t *ev)
{
	bool press = ev->gesture == GESTURE_PRESS || ev->gesture == GESTURE_REPEAT;
	int n = marine_count;
	if (press && (ev->action == ACT_NAV_UP || ev->action == ACT_NAV_DOWN || ev->action == ACT_ENC_STEP ||
		      ev->action == ACT_NAV_LEFT || ev->action == ACT_NAV_RIGHT)) {
		int d = ev->action == ACT_ENC_STEP ? (ev->value > 0 ? 1 : -1) : ev->action == ACT_NAV_DOWN ? 1 :
			ev->action == ACT_NAV_UP ? -1 : ev->action == ACT_NAV_RIGHT ? 10 : -10;
		sel = (uint8_t)(((int)sel + d % n + n) % n);
		ui_dirty();
		return true;
	}
	if (ev->action == ACT_CONTEXT && ev->gesture == GESTURE_PRESS) {
		int i = marine_find("16");
		sel = (uint8_t)(i >= 0 ? i : 0);
		ui_dirty();
		return true;
	}
	if (ev->action == ACT_SELECT && ev->gesture == GESTURE_PRESS) {
		if (sel < marine_count)
			listen_on(&marine_table[sel]);
		ui_dirty();
		return true;
	}
	return false;
}

static void marine_enter(void)
{
	int i = marine_find("16");	/* the screen opens on channel 16 */
	sel = (uint8_t)(i >= 0 ? i : 0);
	top = 0;
	ui_dirty();
}

const ui_scr_def_t ui_marine_screen = {
	.name = "marine", .flags = SF_FIELD, .key = marine_key, .draw = marine_draw, .enter = marine_enter,
};
#endif
