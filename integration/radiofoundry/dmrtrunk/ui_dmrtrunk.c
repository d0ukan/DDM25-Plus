/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus Tools > DMR trunk (radio/dmrtrunk.h): follow a DMR Tier III trunked system, receive only. The selected
 * channel is the control channel (TSCC): a DMR channel on its frequency, with its color code (or Any), and the
 * talkgroup to follow (its contact and RX group list; RX group list Any or Left / Right: every group). OK starts or
 * stops; Left / Right switch between the channel's groups and every group. The screen shows the system (model, net,
 * site from C_ALOHA, or unknown), the TSCC (searching or locked, CSBKs a second, ones that failed the CRC), the call
 * followed now or last (talkgroup, source ID and its name from the ID database), and the grants heard (newest first,
 * with why one wasn't followed). Back stops; leaving the screen otherwise stops it after a few seconds. */
#include "config.h"
#if CONFIG_DMR_TRUNK
#include "ui_nav.h"
#include "ui_screens.h"
#include "fmt.h"
#include "i18n.h"
#include "gfx.h"
#include "codeplug/codeplug.h"
#include "radio/radio.h"
#include "radio/dmr_csbk.h"
#include "radio/dmrtrunk.h"
#if CONFIG_ID_DB
#include "radio/iddb.h"
#endif

static bool every_group;
static dmrtk_view_t tv;

/* Why a grant was not followed, in a few letters ("" = followed) */
static const char *why(uint8_t flags)
{
	return flags & DMRTK_G_ENC ? "ENC" : flags & DMRTK_G_NO_PLAN ? "?" : flags & DMRTK_G_BAND ? TR("band") :
	       flags & DMRTK_G_PRIVATE ? TR("priv") : flags & DMRTK_G_OTHER ? "-" :
	       flags & DMRTK_G_BUSY ? TR("busy") : "";
}

/* The system: "Small Net 12 Site 3" (as the standard numbers them: sent + 1) */
static void __attribute__((noinline)) sys_line(sbuf_t *b)
{
	static const char *const model[4] = { "Tiny", "Small", "Large", "Huge" };
	if (!tv.have_sys) {
		sb_str(b, TR("System unknown"));
		return;
	}
	sb_str(b, model[tv.model & 3u]);
	sb_str(b, " Net ");
	sb_u32(b, tv.net + 1u, 1);
	sb_chr(b, ' ');
	sb_str(b, TR("Site "));
	sb_u32(b, tv.site + 1u, 1);
}

/* The call followed now or last: "TG 2621 2620123 TA1ABC Ahmet 3s" */
static void __attribute__((noinline)) call_line(sbuf_t *b)
{
	sb_str(b, tv.call.op == CSBKO_PV_GRANT ? "ID " : "TG ");
	sb_u32(b, tv.call.tg, 1);
	if (tv.call.src) {
		sb_chr(b, ' ');
		sb_u32(b, tv.call.src, 1);
#if CONFIG_ID_DB
		char name[18];
		if (iddb_text(tv.call.src, name, sizeof name)) {
			sb_chr(b, ' ');
			sb_str(b, name);
		}
#endif
	}
	if (tv.call.flags & DMRTK_G_ENC)
		sb_str(b, " ENC");
	sb_chr(b, ' ');
	sb_age(b, ui_st.now_ms - tv.call.ms);
}

static void __attribute__((noinline)) grant_line(sbuf_t *b, const dmrtk_grant_t *g)
{
	sb_u32(b, g->tg, 1);
	sb_chr(b, ' ');
	if (g->hz) {
		sb_freq(b, g->hz);
	} else {
		sb_str(b, "ch ");
		sb_u32(b, g->lpcn, 1);
	}
	sb_chr(b, '/');
	sb_u32(b, g->ts, 1);
	sb_chr(b, ' ');
	if (g->emergency)
		sb_str(b, "EM ");
	sb_str(b, why(g->flags));
	sb_chr(b, ' ');
	sb_age(b, ui_st.now_ms - g->ms);
}

static void tk_draw(void)
{
	char buf[44];
	sbuf_t b;
	bool have = dmrtk_snapshot(&tv);
	bool on = have && tv.state != DMRTK_OFF;
	const radio_view_t *rv = ui_view();
	gfx_clear(ui_col.bg);
	int y = ui_draw_top(rv, &ui_st, TR("DMR trunk"), "") + 2;
	const font_t *s = ui_layout.small, *m = ui_layout.medium;
	/* The control channel and the groups followed */
	sb_init(&b, buf, sizeof buf);
	sb_str(&b, "CC ");
	sb_freq(&b, on ? tv.cc_hz : rv->ch.rx_hz);
	bool all = on ? tv.all : every_group || rv->ch.listen_list == CODEPLUG_LIST_ANY;
	sb_chr(&b, ' ');
	if (!all && rv->dmr.target_id && !rv->dmr.target_private) {
		sb_str(&b, "TG ");
		sb_u32(&b, on ? tv.follow_tg : rv->dmr.target_id, 1);
	} else {
		sb_str(&b, TR("all groups"));
	}
	gfx_text(s, 0, y, buf, ui_col.dim);
	y += s->h + 3;
	sb_init(&b, buf, sizeof buf);
	color_t c = ui_col.dim;
	if (!on) {
		sb_str(&b, TR("Stopped: OK starts"));
	} else if (tv.state == DMRTK_HUNT) {
		sb_str(&b, TR("Searching"));
		c = ui_col.warn;
	} else if (tv.state == DMRTK_CONTROL) {
		sb_str(&b, TR("Control locked"));
		c = ui_col.fg;
	} else {
		sb_str(&b, TR("Voice "));
		sb_freq(&b, tv.call.hz);
		sb_str(&b, " TS");
		sb_u32(&b, tv.call.ts, 1);
		c = ui_col.rx;
	}
	gfx_text(m, 0, y, buf, c);
	y += m->h + 2;
	if (on) {
		sb_init(&b, buf, sizeof buf);
		sys_line(&b);
		gfx_text(s, 0, y, buf, ui_col.dim);
		y += s->h + 1;
		/* "CSBK 12/s bad 0 LPCN 4": LPCNs whose frequency is known */
		sb_init(&b, buf, sizeof buf);
		sb_str(&b, "CSBK ");
		sb_u32(&b, tv.rate, 1);
		sb_str(&b, TR("/s bad "));
		sb_u32(&b, tv.bad, 1);
		sb_str(&b, " LPCN ");
		sb_u32(&b, tv.plan_n, 1);
		gfx_text(s, 0, y, buf, ui_col.dim);
	}
	y += s->h + 3;
	gfx_hline(0, y, gfx_width(), ui_col.line);
	y += 3;
	/* The call followed now (or last), then the grants heard */
	if (on && tv.call.ms) {
		sb_init(&b, buf, sizeof buf);
		call_line(&b);
		gfx_text(s, 0, y, buf, tv.state == DMRTK_VOICE ? ui_col.rx : ui_col.fg);
		y += s->h + 2;
	}
	if (on && tv.n == 0)
		gfx_text(s, 0, y, TR("No grants heard yet"), ui_col.dim);
	for (uint8_t i = 0; on && i < tv.n && y + s->h < ui_layout.hint_y; i++) {
		sb_init(&b, buf, sizeof buf);
		grant_line(&b, &tv.recent[i]);
		gfx_text(s, 0, y, buf, tv.recent[i].flags == DMRTK_G_FOLLOWED ? ui_col.fg : ui_col.dim);
		y += s->h + 1;
	}
	ui_draw_bottom(&ui_st, on ? TR("OK stop  <> groups") : TR("OK start  <> groups"));
	ui_draw_banner(&ui_st);
}

static void start(void)
{
	const radio_view_t *rv = ui_view();
	if (rv->vfo || rv->ch.mode != RADIO_MODE_DMR) {
		ui_toast(TR("Select a DMR channel first"));
		return;
	}
	radio_req_dmr_trunk(true, every_group);
}

static bool tk_key(const input_event_t *ev)
{
	bool press = ev->gesture == GESTURE_PRESS || ev->gesture == GESTURE_REPEAT;
	bool on = dmrtk_snapshot(&tv) && tv.state != DMRTK_OFF;
	if (press && (ev->action == ACT_NAV_LEFT || ev->action == ACT_NAV_RIGHT)) {
		every_group = !every_group;
		if (on)
			start();	/* follows the other choice from now on */
		ui_dirty();
		return true;
	}
	if (ev->gesture == GESTURE_PRESS && ev->action == ACT_SELECT) {
		if (on)
			radio_req_dmr_trunk(false, false);
		else
			start();
		ui_dirty();
		return true;
	}
	if (ev->gesture == GESTURE_PRESS && ev->action == ACT_BACK)
		radio_req_dmr_trunk(false, false);	/* and the screen's own Back leaves */
	return false;
}

static void tk_enter(void)
{
	ui_dirty();
}

const ui_scr_def_t ui_dmrtrunk_screen = {
	.name = "dmrtrunk", .flags = SF_FIELD, .key = tk_key, .draw = tk_draw, .enter = tk_enter,
};
#endif
