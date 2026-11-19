/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus Tools > Tone finder (CONFIG_TONE_FIND) and Tools > DTMF / SelCall (CONFIG_SELCALL), radio/radio_fmdec.h.
 * The decoders listen only while their screen is on top (ui_msg_tick, as the pager's).
 *
 * Tone finder: the frequency and signal strength, then large what the signal carries ("CTCSS 88.5 Hz", "DCS 023N",
 * "Searching...", "No tone") with a bar for how firmly, the equivalent DCS names (the same signal on the air), the
 * last tone found once the signal has gone, and what the FD6818's own measurement registers read (experimental). OK
 * opens Write tone for the tone heard (or the last one): RX and TX / RX only / TX only. On a channel it is written
 * into the stored channel through the channel editor's own call (ui_svc_write_channel: the whole channel as read
 * from the codeplug, only its tones changed; the radio then uses it, temporary changes dropped as after an editor
 * save); in the VFO it is set as the VFO's tone (radio_req_tone, kept like any VFO setting).
 *
 * DTMF / SelCall: the last sequences, newest first ("ZVEI1 12345  438.500", how long ago); # picks the tone set
 * looked for (Auto, ZVEI1, ZVEI2, CCIR, EIA; DTMF always); OK shows a row whole. */
#include "config.h"
#if CONFIG_TONE_FIND || CONFIG_SELCALL
#include <string.h>
#include "ui_nav.h"
#include "ui_screens.h"
#include "ui_svc.h"
#include "fmt.h"
#include "i18n.h"
#include "gfx.h"
#include "codeplug/codeplug.h"
#include "radio/radio.h"
#include "radio/radio_msg.h"
#include "radio/radio_fmdec.h"
#if CONFIG_TONE_FIND
#include "radio/tonefind.h"
#endif
#if CONFIG_SELCALL
#include "radio/selcall.h"
#endif

/* static: the UI task's stack is tight. line: text drawn now (never kept), toast: a toast's text (it stays up) */
static char line[48], toast[40];

/* Whether the channel suits (an FM channel, not the air band's AM): what the first line says otherwise */
static const char *listen_line(const char *tune)
{
	const radio_view_t *v = ui_view();
	uint8_t st = radio_msg_status();
	if (st == RADIO_MSG_ST_ON)
		return TR("Listening on this frequency");
	if (st == RADIO_MSG_ST_WAIT || (v->ch.mode == RADIO_MODE_FM && !v->am))
		return TR("Waiting for receive audio");
	return tune;
}

#if CONFIG_TONE_FIND
static radio_tonef_info_t ti;
static subtone_t pick;		/* the tone Write tone writes */

/* "CTCSS 88.5 Hz", "DCS 023N" */
static void sb_found(sbuf_t *b, uint8_t type, uint16_t value)
{
	if (type == TONE_CTCSS) {
		sb_str(b, "CTCSS ");
		sb_u32(b, value / 10u, 1);
		sb_chr(b, '.');
		sb_u32(b, value % 10u, 1);
		sb_str(b, " Hz");
	} else {
		sb_str(b, "DCS ");
		sb_chr(b, (char)('0' + ((value >> 6) & 7u)));
		sb_chr(b, (char)('0' + ((value >> 3) & 7u)));
		sb_chr(b, (char)('0' + (value & 7u)));
		sb_chr(b, type == TONE_DCS_INV ? 'I' : 'N');
	}
}

/* The other standard names of the same DCS signal ("= 047I 712N"), at most three */
static void sb_same(sbuf_t *b, uint8_t type, uint16_t value)
{
	uint32_t w = tonef_dcs_word(value);
	if (type == TONE_DCS_INV)
		w = ~w & 0x7FFFFFu;
	uint8_t n = 0;
	for (uint8_t r = 0; r < 23u && n < 3u; r++) {
		for (uint8_t inv = 0; inv < 2u && n < 3u; inv++) {
			int c = tonef_dcs_code(inv ? ~w : w);
			if (c < 0 || !tonef_dcs_standard((uint16_t)c) || ((uint16_t)c == value && inv == (type == TONE_DCS_INV)))
				continue;
			sb_str(b, n == 0 ? "= " : " ");
			sb_found(b, inv ? TONE_DCS_INV : TONE_DCS, (uint16_t)c);
			n++;
		}
		w = ((w >> 1) | (w << 22)) & 0x7FFFFFu;
	}
}

/* "0723 8000 0000": what the FD6818's 0x68-0x6A read */
static void sb_chip_raw(sbuf_t *b)
{
	static const char hex[] = "0123456789ABCDEF";
	for (uint8_t i = 0; i < 3u && ti.chip_ok; i++) {
		for (int s = 12; s >= 0; s -= 4)
			sb_chr(b, hex[(ti.chip[i] >> s) & 15u]);
		if (i < 2u)
			sb_chr(b, ' ');
	}
}

/* What they mean if they are the BK4819's: "FD6818: CTCSS 88.5 Hz" */
static void sb_chip(sbuf_t *b)
{
	sb_str(b, "FD6818: ");
	if (!ti.chip_ok) {
		sb_chr(b, '-');
		return;
	}
	uint16_t r68 = ti.chip[0], r69 = ti.chip[1], r6a = ti.chip[2];
	if (!(r68 & 0x8000u) && (r68 & 0x1FFFu) != 0) {
		uint32_t hz10 = ((uint32_t)(r68 & 0x1FFFu) * 4843u + 5000u) / 10000u;	/* / 20.64888, x 10 */
		sb_found(b, TONE_CTCSS, (uint16_t)hz10);
	} else if (!(r69 & 0x8000u)) {
		uint32_t w = ((uint32_t)(r69 & 0xFFFu) << 12) | (r6a & 0xFFFu);
		uint16_t best = 0xFFFF;
		for (uint8_t r = 0; r < 23u; r++) {
			uint16_t k = tonef_dcs_key(w);
			if (k < best)
				best = k;
			w = ((w >> 1) | (w << 22)) & 0x7FFFFFu;
		}
		if (best != 0xFFFF)
			sb_found(b, (best & 0x200u) ? TONE_DCS_INV : TONE_DCS, (uint16_t)(best & 0x1FFu));
		else
			sb_str(b, "DCS ?");
	} else {
		sb_chr(b, '-');
	}
}

static void tonef_enter(void)
{
	ui_dirty();
}

static void __attribute__((noinline)) tonef_draw(void)
{
	sbuf_t b;
	const radio_view_t *v = ui_view();
	radio_tonef_info(&ti);
	gfx_clear(ui_col.bg);
	int y = ui_draw_top(v, &ui_st, TR("Tone finder"), "") + 2;
	const font_t *f = ui_layout.small, *big = ui_layout.large;
	int w = gfx_width();
	/* 438.500  -87 dBm */
	sb_init(&b, line, sizeof line);
	sb_freq(&b, v->ch.rx_hz);
	sb_str(&b, "  ");
	if (v->rssi_dbm == 0)
		sb_str(&b, "---");
	else
		sb_i32(&b, v->rssi_dbm);
	sb_str(&b, " dBm");
	gfx_text(f, 0, y, line, ui_col.fg);
	y += f->h + 2;
	gfx_text(f, 0, y, ti.nomem ? TR("Not enough memory") : listen_line(TR("Tone finder: tune an FM channel")),
		 ui_col.dim);
	y += f->h + 6;
	/* what is heard */
	bool found = ti.state == RADIO_TONEF_FOUND;
	sb_init(&b, line, sizeof line);
	if (found)
		sb_found(&b, ti.type, ti.value);
	else if (ti.state == RADIO_TONEF_NONE)
		sb_str(&b, TR("No tone"));
	else if (ti.state == RADIO_TONEF_SEARCH)
		sb_str(&b, TR("Searching..."));
	else
		sb_str(&b, "---");
	gfx_text(big, (w - gfx_text_width(big, line)) / 2, y, line, found ? ui_col.rx : ui_col.fg);
	y += big->h + 4;
	/* how firmly */
	int bh = f->h, bw = w - 2;
	gfx_rect(0, y, bw + 2, bh, ui_col.line);
	int x = found ? ti.quality * bw / 100 : 0;
	if (x > 0)
		gfx_fill(1, y + 1, x, bh - 2, ui_col.rx);
	y += bh + 4;
	/* the same DCS signal's other names, or the last tone once the signal is gone */
	sb_init(&b, line, sizeof line);
	if (found && ti.type != TONE_CTCSS) {
		sb_same(&b, ti.type, ti.value);
	} else if (!found && ti.last_type != TONE_NONE) {
		sb_str(&b, TR("Last"));
		sb_str(&b, ": ");
		sb_found(&b, ti.last_type, ti.last_value);
	}
	gfx_text(f, 0, y, line, ui_col.dim);
	y += f->h + 2;
	sb_init(&b, line, sizeof line);
	sb_chip(&b);
	gfx_text(f, 0, y, line, ui_col.dim);
	y += f->h + 2;
	sb_init(&b, line, sizeof line);
	sb_chip_raw(&b);
	gfx_text(f, 0, y, line, ui_col.dim);
	ui_draw_bottom(&ui_st, TR("OK write  Back"));
	ui_draw_banner(&ui_st);
}

static bool tonef_key(const input_event_t *ev)
{
	if (ev->action != ACT_SELECT || ev->gesture != GESTURE_PRESS)
		return false;
	radio_tonef_info(&ti);
	if (ti.state == RADIO_TONEF_FOUND)
		pick = (subtone_t){ ti.type, ti.value };
	else if (ti.last_type != TONE_NONE)
		pick = (subtone_t){ ti.last_type, ti.last_value };
	else {
		ui_toast(TR("No tone found yet"));
		return true;
	}
	ui_push(SCR_TONEF_WRITE, 0);
	return true;
}

const ui_scr_def_t ui_tonef_screen = {
	.name = "tonefinder", .flags = SF_FIELD, .key = tonef_key, .draw = tonef_draw, .enter = tonef_enter,
};

/* --- Write tone --- */

enum { TW_BOTH, TW_RX, TW_TX, TW_COUNT };

static uint16_t tw_count(void)
{
	return TW_COUNT;
}

static void tw_item(uint16_t i, ui_item_t *it)
{
	static const char *const labels[TW_COUNT] = { TRN("RX and TX"), TRN("RX only"), TRN("TX only") };
	if (i < TW_COUNT)
		str_copy(it->label, sizeof it->label, tr(labels[i]));
}

static void tw_select(uint16_t i, bool hold)
{
	static channel_t ch;	/* static: the UI task's stack is tight */
	(void)hold;
	const radio_view_t *v = ui_view();
	uint8_t which = i == TW_RX ? RADIO_TONE_RX : i == TW_TX ? RADIO_TONE_TX : RADIO_TONE_BOTH;
	if (v->ch.mode != RADIO_MODE_FM || v->am) {
		ui_toast(TR("FM only: this is not an FM channel"));
		return;
	}
	if (v->vfo) {
		radio_req_tone(which, pick);	/* the VFO keeps it as any VFO setting */
		ui_toast(TR("VFO tone set"));
		ui_pop();
		return;
	}
	if (v->hotspot) {
		ui_toast(TR("Stop hotspot to save"));
		return;
	}
	if (v->channel_number == 0 || !codeplug_channel(v->channel_number, &ch)) {
		ui_toast(TR("Pick a saved channel"));
		return;
	}
	if (which & RADIO_TONE_RX)
		ch.rx_tone = pick;
	if (which & RADIO_TONE_TX)
		ch.tx_tone = pick;
	/* the channel editor's own write: the stored channel with its tones changed */
	if (!ui_svc_write_channel(v->channel_number, &ch)) {
		ui_toast(TR("Couldn't save now: try again"));
		return;
	}
	sbuf_t b;
	sb_init(&b, toast, sizeof toast);
	sb_str(&b, TR("Saved channel "));
	sb_u32(&b, v->channel_number, 1);
	ui_toast(toast);
	ui_pop();
}

static const char *tw_body(void)
{
	const radio_view_t *v = ui_view();
	sbuf_t b;
	sb_init(&b, line, sizeof line);
	sb_found(&b, pick.type, pick.value);
	sb_str(&b, " > ");
	if (v->vfo)
		sb_str(&b, "VFO");
	else
		sb_str(&b, v->ch.name);
	return line;
}

const ui_scr_def_t ui_tonef_write_screen = {
	.name = "tonewrite", .flags = SF_LIST | SF_FIELD, .prefix = TRN("Write tone"),
	.hint = TRN("OK write  Back"), .count = tw_count, .item = tw_item, .select = tw_select, .body = tw_body,
};
#endif

#if CONFIG_SELCALL
static radio_selcall_t se;

static uint16_t sc_count(void)
{
	return radio_selcall_count();
}

/* "ZVEI1 12345  438.500" */
static void sb_entry(sbuf_t *b, const radio_selcall_t *e)
{
	sb_str(b, selcall_set_name(e->set));
	sb_chr(b, ' ');
	sb_str(b, e->text);
	sb_str(b, "  ");
	sb_freq(b, e->rx_hz);
}

static void sc_item(uint16_t i, ui_item_t *it)
{
	if (!radio_selcall_get((uint8_t)i, &se))
		return;
	sbuf_t b;
	sb_init(&b, it->label, sizeof it->label);
	sb_entry(&b, &se);
	ui_sb_age(it->value, sizeof it->value, ui_st.now_ms - se.ms);
	it->flags |= UI_ITEM_PREVIEW;	/* the frequency is cut first */
}

static void sc_select(uint16_t i, bool hold)
{
	(void)hold;
	if (!radio_selcall_get((uint8_t)i, &se))
		return;
	sbuf_t b;
	sb_init(&b, toast, sizeof toast);
	sb_entry(&b, &se);
	ui_toast(toast);
}

static const char *sc_set_name(uint8_t set)
{
	return set == SELCALL_ANY ? TR("Auto") : selcall_set_name(set);
}

static const char *sc_body(void)
{
	sbuf_t b;
	sb_init(&b, line, sizeof line);
	sb_str(&b, listen_line(TR("DTMF / SelCall: tune an FM channel")));
	sb_str(&b, " - ");
	sb_str(&b, sc_set_name(radio_selcall_only()));
	return line;
}

static bool sc_key(const input_event_t *ev)
{
	if (ev->action != ACT_CONTEXT || ev->gesture != GESTURE_PRESS)
		return false;
	uint8_t set = (uint8_t)((radio_selcall_only() + 1u) % SELCALL_SETS);
	radio_selcall_set(set);
	sbuf_t b;
	sb_init(&b, toast, sizeof toast);
	sb_str(&b, "SelCall: ");
	sb_str(&b, sc_set_name(set));
	ui_toast(toast);
	ui_dirty();
	return true;
}

const ui_scr_def_t ui_selcall_screen = {
	.name = "selcall", .flags = SF_LIST | SF_FIELD, .prefix = TRN("DTMF / SelCall"), .hint = TRN("# tone set  Back"),
	.empty = TRN("Nothing heard yet"), .count = sc_count, .item = sc_item, .select = sc_select, .body = sc_body,
	.key = sc_key,
};
#endif

/* ui_msg_tick: the kind this screen listens for (0: not one of these), and whether it should draw again */
uint8_t ui_fmdec_kind(uint8_t scr)
{
#if CONFIG_TONE_FIND
	if (scr == SCR_TONEF || scr == SCR_TONEF_WRITE)
		return RADIO_MSG_TONEF;
#endif
#if CONFIG_SELCALL
	if (scr == SCR_SELCALL)
		return RADIO_MSG_SELCALL;
#endif
	(void)scr;
	return 0;
}

bool ui_fmdec_tick(void)
{
	static uint32_t seen;
	uint32_t q = radio_fmdec_seq();
	if (q == seen)
		return false;
	seen = q;
	return true;
}
#endif
