/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus Tools > P25 trunk (radio/p25trunk.h): follow a P25 Phase 1 trunked system, receive only. The selected
 * channel is the control channel: a P25 channel on its frequency, with its NAC (or Any) and the talkgroup to follow
 * (Any: every group). OK starts or stops; Left / Right switch between the channel's talkgroup and every group. The
 * screen shows the system (WACN, system ID, RFSS-site, NAC), the control channel (searching or locked, TSBKs a second,
 * blocks that failed), the call followed now or last, and the grants heard (newest first, with why one wasn't
 * followed). Back stops; leaving the screen otherwise stops it after a few seconds (P25TK_IDLE_MS). */
#include "config.h"
#if CONFIG_P25_TRUNK
#include "ui_nav.h"
#include "ui_screens.h"
#include "fmt.h"
#include "i18n.h"
#include "gfx.h"
#include "codeplug/codeplug.h"
#include "radio/radio.h"
#include "radio/p25trunk.h"
#include "ui/ui_scratch.h"

static bool every_group;
/* The system as last copied: the tool screens' shared buffer (ui_scratch.h), refilled by snap() in each draw and
 * key */
#define tv	(ui_scratch.p25tk)

static bool snap(void)
{
	ui_scratch_take(UI_SCRATCH_P25TK);
	return p25tk_snapshot(&tv);
}

static void hex(sbuf_t *b, uint32_t value, unsigned digits)
{
	while (digits--)
		sb_chr(b, "0123456789ABCDEF"[(value >> (4u * digits)) & 15u]);
}

/* Why a grant was not followed, in a few letters ("" = followed) */
static const char *why(uint8_t flags)
{
	return flags & P25TK_G_ENC ? "ENC" : flags & P25TK_G_TDMA ? "P2" : flags & P25TK_G_NO_PLAN ? "?" :
	       flags & P25TK_G_BAND ? TR("band") : flags & P25TK_G_OTHER ? "-" : flags & P25TK_G_BUSY ? TR("busy") : "";
}

static void tk_draw(void)
{
	char buf[40];
	sbuf_t b;
	bool have = snap();
	bool on = have && tv.state != P25TK_OFF;
	const radio_view_t *rv = ui_view();
	gfx_clear(ui_col.bg);
	int y = ui_draw_top(rv, &ui_st, TR("P25 trunk"), "") + 2;
	const font_t *s = ui_layout.small, *m = ui_layout.medium;
	/* The control channel and the groups followed */
	sb_init(&b, buf, sizeof buf);
	sb_str(&b, "CC ");
	sb_freq(&b, on ? tv.cc_hz : rv->ch.rx_hz);
	uint16_t tg = on ? (tv.all ? 0 : tv.follow_tg) : (every_group ? 0 : rv->ch.p25_tg);
	sb_str(&b, " ");
	if (tg) {
		sb_str(&b, "TG ");
		sb_u32(&b, tg, 1);
	} else {
		sb_str(&b, TR("all groups"));
	}
	gfx_text(s, 0, y, buf, ui_col.dim);
	y += s->h + 3;
	sb_init(&b, buf, sizeof buf);
	color_t c = ui_col.dim;
	if (!on) {
		sb_str(&b, TR("Stopped: OK starts"));
	} else if (tv.state == P25TK_HUNT) {
		sb_str(&b, TR("Searching"));
		c = ui_col.warn;
	} else if (tv.state == P25TK_CONTROL) {
		sb_str(&b, TR("Control locked"));
		c = ui_col.fg;
	} else {
		sb_str(&b, TR("Voice "));
		sb_freq(&b, tv.call.hz);
		c = ui_col.rx;
	}
	gfx_text(m, 0, y, buf, c);
	y += m->h + 2;
	if (on) {
		/* The system: "WACN BEE00 SYS 1C8 NAC 1C8", then "Site 1-5 TSBK 22/s bad 3" */
		sb_init(&b, buf, sizeof buf);
		if (tv.have_net) {
			sb_str(&b, "WACN ");
			hex(&b, tv.wacn, 5);
			sb_str(&b, " ");
		}
		if (tv.have_net || tv.have_site) {
			sb_str(&b, "SYS ");
			hex(&b, tv.sysid, 3);
			sb_str(&b, " ");
		}
		if (tv.heard_nac != 0xFFFFu) {
			sb_str(&b, "NAC ");
			hex(&b, tv.heard_nac, 3);
#if CONFIG_NAC_DEC
			sb_chr(&b, '/');
			sb_u32(&b, tv.heard_nac, 1);
#endif
		}
		gfx_text(s, 0, y, buf, ui_col.dim);
		y += s->h + 1;
		sb_init(&b, buf, sizeof buf);
		if (tv.have_site) {
			sb_str(&b, TR("Site "));
			sb_u32(&b, tv.rfss, 1);
			sb_chr(&b, '-');
			sb_u32(&b, tv.site, 1);
			sb_chr(&b, ' ');
		}
		sb_str(&b, "TSBK ");
		sb_u32(&b, tv.rate, 1);
		sb_str(&b, TR("/s bad "));
		sb_u32(&b, tv.bad, 1);
		gfx_text(s, 0, y, buf, ui_col.dim);
	}
	y += s->h + 3;
	gfx_hline(0, y, gfx_width(), ui_col.line);
	y += 3;
	/* The call followed now (or last), then the grants heard */
	if (on && tv.call.ms) {
		sb_init(&b, buf, sizeof buf);
		sb_str(&b, "TG ");
		sb_u32(&b, tv.call.tg, 1);
		if (tv.call.src) {
			sb_str(&b, " ID ");
			sb_u32(&b, tv.call.src, 1);
		}
		sb_chr(&b, ' ');
		sb_age(&b, ui_st.now_ms - tv.call.ms);
		gfx_text(s, 0, y, buf, tv.state == P25TK_VOICE ? ui_col.rx : ui_col.fg);
		y += s->h + 2;
	}
	if (on && tv.n == 0)
		gfx_text(s, 0, y, TR("No grants heard yet"), ui_col.dim);
	for (uint8_t i = 0; on && i < tv.n && y + s->h < ui_layout.hint_y; i++) {
		const p25tk_grant_t *g = &tv.recent[i];
		sb_init(&b, buf, sizeof buf);
		sb_u32(&b, g->tg, 1);
		sb_chr(&b, ' ');
		if (g->hz)
			sb_freq(&b, g->hz);
		else
			hex(&b, g->ch, 4);
		sb_chr(&b, ' ');
		sb_str(&b, why(g->flags));
		sb_chr(&b, ' ');
		sb_age(&b, ui_st.now_ms - g->ms);
		gfx_text(s, 0, y, buf, g->flags == P25TK_G_FOLLOWED ? ui_col.fg : ui_col.dim);
		y += s->h + 1;
	}
	ui_draw_bottom(&ui_st, on ? TR("OK stop  <> groups") : TR("OK start  <> groups"));
	ui_draw_banner(&ui_st);
}

static void start(void)
{
	const radio_view_t *rv = ui_view();
	if (rv->vfo || !codeplug_mode_p25(rv->ch.mode) || codeplug_mode_ysf(rv->ch.mode)) {
		ui_toast(TR("Select a P25 channel first"));
		return;
	}
	radio_req_p25_trunk(true, every_group);
}

static bool tk_key(const input_event_t *ev)
{
	bool press = ev->gesture == GESTURE_PRESS || ev->gesture == GESTURE_REPEAT;
	bool on = snap() && tv.state != P25TK_OFF;
	if (press && (ev->action == ACT_NAV_LEFT || ev->action == ACT_NAV_RIGHT)) {
		every_group = !every_group;
		if (on)
			start();	/* follows the other choice from now on */
		ui_dirty();
		return true;
	}
	if (ev->gesture == GESTURE_PRESS && ev->action == ACT_SELECT) {
		if (on)
			radio_req_p25_trunk(false, false);
		else
			start();
		ui_dirty();
		return true;
	}
	if (ev->gesture == GESTURE_PRESS && ev->action == ACT_BACK)
		radio_req_p25_trunk(false, false);	/* and the screen's own Back leaves */
	return false;
}

static void tk_enter(void)
{
	ui_dirty();
}

const ui_scr_def_t ui_p25trunk_screen = {
	.name = "p25trunk", .flags = SF_FIELD, .key = tk_key, .draw = tk_draw, .enter = tk_enter,
};
#endif
