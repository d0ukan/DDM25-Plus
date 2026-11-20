/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM25 Plus one-key SOS (sos.h), the UI: the SOS key's page and Tools > SOS. Installed as core/ui/ui_sos.c.
 *   SCR_SOS, arg SOS_PAGE_ASK: "Send SOS now?" with the actions chosen and "it transmits"; OK (or the SOS key held)
 *     starts them and the page shows how each goes; Back cancels. With nothing chosen it says so and sends nothing.
 *   SCR_SOS, arg SOS_PAGE_STOP (the SOS key while one runs): each action's state, OK (or the key held) stops it.
 *   SCR_SOS_SET (Tools > SOS): APRS SOS, DMR emergency, CW SOS on / off (what the build has; RAM only, all off at
 *     power on).
 * Toasts on any screen say what was sent or why not (sos_tick's events). RAM: 2 B. */
#include "config.h"
#if CONFIG_SOS_KEY
#include "ui_nav.h"
#include "ui_screens.h"
#include "fmt.h"
#include "i18n.h"
#include "gfx.h"
#include "ui/sos.h"

enum { SOS_PAGE_ASK, SOS_PAGE_STATUS, SOS_PAGE_STOP };

static const uint8_t actions[3] = { SOS_APRS, SOS_DMR, SOS_CW };
static const char *const action_names[3] = { TRN("APRS SOS"), TRN("DMR emergency"), TRN("CW SOS (once)") };
static uint16_t shown;		/* the results last drawn (redraw on a change) */

static const char *res_text(sos_res_t r)
{
	switch (r) {
	case SOS_R_QUEUED: return TR("waits for the CW");
	case SOS_R_WAIT: return TR("sending");
	case SOS_R_SENT: return TR("sent");
	case SOS_R_NO_DMR: return TR("needs a DMR channel");
	case SOS_R_NO_POS: return TR("no position");
	case SOS_R_NO_APRS: return TR("APRS not ready");
	case SOS_R_NO_CW: return TR("can't transmit here");
	case SOS_R_STOPPED: return TR("stopped");
	default: return TR("off");
	}
}

static uint16_t signature(void)
{
	return (uint16_t)(sos_res(SOS_CW) | sos_res(SOS_DMR) << 4 | sos_res(SOS_APRS) << 8 | (sos_active() ? 1u << 12 : 0u));
}

static void confirm(void)
{
	ui_frame_t *f = ui_frame();
	if (f->arg == SOS_PAGE_ASK) {
		if (sos_start(ui_st.now_ms) == 0) {
			ui_toast(TR("No SOS actions chosen: Tools > SOS key"));
			return;
		}
		f->arg = SOS_PAGE_STATUS;
		ui_toast(TR("SOS started"));
	} else if (f->arg == SOS_PAGE_STOP) {
		sos_stop();
		f->arg = SOS_PAGE_STATUS;
		ui_toast(TR("SOS stopped"));
	}
	ui_dirty();
}

bool ui_sos_key(const input_event_t *ev)
{
	if (ev->action != ACT_SOS)
		return false;
	if (ev->gesture != GESTURE_PRESS && ev->gesture != GESTURE_LONG)
		return true;
	ui_frame_t *f = ui_frame();
	if (f->scr == SCR_SOS) {
		if (ev->gesture == GESTURE_LONG) {
			confirm();	/* the same key held confirms what the page offers */
		} else if (f->arg == SOS_PAGE_STATUS && sos_active()) {
			f->arg = SOS_PAGE_STOP;	/* pressed again while it runs: Stop SOS? */
			ui_dirty();
		}
		return true;
	}
	ui_push(SCR_SOS, sos_active() ? SOS_PAGE_STOP : SOS_PAGE_ASK);
	return true;
}

bool ui_sos_tick(uint32_t now_ms)
{
	const char *t = NULL;
	switch (sos_tick(now_ms)) {
	case SOS_EV_CW_SENT: t = TR("CW SOS sent"); break;
	case SOS_EV_CW_NO: t = TR("CW SOS not sent: can't transmit here"); break;
	case SOS_EV_DMR_SENT: t = TR("DMR emergency sent"); break;
	case SOS_EV_DMR_NO: t = TR("Not sent: needs a DMR channel"); break;
	case SOS_EV_APRS_NO_POS: t = TR("SOS not sent: no position"); break;
	case SOS_EV_APRS_NO: t = TR("SOS not sent: APRS not ready"); break;
	default: break;
	}
	if (t != NULL)
		ui_toast(t);
	uint16_t sig = signature();
	bool changed = sig != shown && ui_frame()->scr == SCR_SOS;
	if (changed)
		shown = sig;
	return t != NULL || changed;
}

static void sos_draw(void)
{
	char buf[48];
	sbuf_t b;
	uint8_t page = (uint8_t)ui_frame()->arg, m = sos_mask();
	gfx_clear(ui_col.bg);
	int y = ui_draw_top(ui_view(), &ui_st, "SOS", "") + 4;
	const font_t *s = ui_layout.small, *md = ui_layout.medium;
	const char *hint = TR("Back");
	shown = signature();
	if (page == SOS_PAGE_ASK) {
		if (m == 0) {
			gfx_text(md, 0, y, TR("No SOS actions chosen"), ui_col.warn);
			y += md->h + 4;
			gfx_text(s, 0, y, TR("Tools > SOS key picks them"), ui_col.dim);
		} else {
			gfx_text(md, 0, y, TR("Send SOS now?"), ui_col.warn);
			y += md->h + 6;
			for (uint8_t i = 0; i < 3u; i++) {
				if (!(m & actions[i]))
					continue;
				gfx_text(md, 0, y, tr(action_names[i]), ui_col.fg);
				y += md->h + 2;
			}
			y += 4;
			gfx_text(s, 0, y, TR("It TRANSMITS"), ui_col.warn);
			hint = TR(TEXT_ALT_OPEN "OK send  hold SOS key  Back no" TEXT_ALT_NEXT "OK send  Back no" TEXT_ALT_CLOSE);
		}
	} else {
		gfx_text(md, 0, y, page == SOS_PAGE_STOP ? TR("Stop SOS?") : sos_active() ? TR("SOS is on") : TR("SOS ended"),
			 page == SOS_PAGE_STOP || sos_active() ? ui_col.warn : ui_col.fg);
		y += md->h + 6;
		for (uint8_t i = 0; i < 3u; i++) {
			sos_res_t r = sos_res(actions[i]);
			if (r == SOS_R_NONE)
				continue;
			sb_init(&b, buf, sizeof buf);
			sb_str(&b, tr(action_names[i]));
			sb_str(&b, ": ");
			sb_str(&b, res_text(r));
			gfx_text(s, 0, y, buf, r == SOS_R_SENT ? ui_col.tx : r == SOS_R_WAIT || r == SOS_R_QUEUED ? ui_col.fg :
				 ui_col.warn);
			y += s->h + 2;
		}
		if (page == SOS_PAGE_STOP)
			hint = TR(TEXT_ALT_OPEN "OK stop SOS  Back" TEXT_ALT_NEXT "OK stop  Back" TEXT_ALT_CLOSE);
		else if (sos_active())
			hint = TR(TEXT_ALT_OPEN "Back  SOS key: stop" TEXT_ALT_NEXT "Back" TEXT_ALT_CLOSE);
	}
	ui_draw_bottom(&ui_st, hint);
	ui_draw_banner(&ui_st);
}

static bool sos_page_key(const input_event_t *ev)
{
	if (ev->action == ACT_SELECT && ev->gesture == GESTURE_PRESS) {
		confirm();
		return true;
	}
	return false;
}

static void sos_enter(void)
{
	ui_dirty();
}

const ui_scr_def_t ui_sos_screen = {
	.name = "sos", .flags = SF_FIELD, .key = sos_page_key, .draw = sos_draw, .enter = sos_enter,
};

/* --- Tools > SOS: the actions the SOS key starts --- */

static uint8_t set_action(uint16_t i)
{
	uint8_t built = sos_built();
	for (uint8_t k = 0; k < 3u; k++) {
		if (!(built & actions[k]))
			continue;
		if (i-- == 0)
			return k;
	}
	return 0xFFu;
}

static uint16_t set_count(void)
{
	uint16_t n = 0;
	for (uint8_t k = 0; k < 3u; k++)
		n += (sos_built() & actions[k]) != 0;
	return n;
}

static void set_item(uint16_t i, ui_item_t *it)
{
	uint8_t k = set_action(i);
	if (k == 0xFFu)
		return;
	bool on = (sos_mask() & actions[k]) != 0;
	str_copy(it->label, sizeof it->label, tr(action_names[k]));
	str_copy(it->value, sizeof it->value, on ? TR("On") : TR("Off"));
	if (on)
		it->flags |= UI_ITEM_WARN;
}

static void set_select(uint16_t i, bool hold)
{
	(void)hold;
	uint8_t k = set_action(i);
	if (k != 0xFFu)
		sos_mask_set((uint8_t)(sos_mask() ^ actions[k]));
	ui_dirty();
}

static const char *set_body(void)
{
	return TR("The SOS key (Settings > Keys) sends these after OK. Off at power on.");
}

const ui_scr_def_t ui_sos_set_screen = {
	.name = "sos-set", .flags = SF_LIST | SF_FIELD, .prefix = "SOS", .hint = TRN("OK on/off  Back"),
	.count = set_count, .item = set_item, .select = set_select, .body = set_body,
};
#endif
