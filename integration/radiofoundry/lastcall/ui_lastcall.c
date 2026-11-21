/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus Tools > Last call (lastcall.h, docs/plus.md "Last calls"): the last 8 calls heard, newest first, from
 * every mode and every watched channel, one row each: "B DMR Joe" and "TG2862 2m" (the watched letter, the mode, the
 * caller's name or ID; the talkgroup or the unit called, how long ago; the talkgroup gives way first where the row is
 * short). OK on the newest P25 call plays it again on the P25 channel in use (CONFIG_CALL_REPLAY, the P25 receiver's
 * last 8 s, exactly as Tools > Last call did before); OK again stops it. The other modes keep no audio (owner,
 * 2026-11-20): their rows say so. Installed as core/ui/ui_lastcall.c.
 *
 * Fed by ui_app.c (plus_lastcall.py), where the heard list is fed: recent_add for every call Recent takes in (a new
 * call; FM caller IDs too), and while a DMR or P25 call goes on. Calls we made are not kept. The caller's name is the
 * one Recent keeps for the same station (DMR: the contact, the ID database, the talker alias), looked up when the row
 * is drawn. RAM: the list (8 x 16 B), its count, the tick's view pointer. */
#include "config.h"
#if CONFIG_LAST_CALLS
#include <string.h>
#include "ui_nav.h"
#include "ui_screens.h"
#include "ui_svc.h"
#include "fmt.h"
#include "i18n.h"
#include "gfx.h"
#include "codeplug/codeplug.h"
#include "ui/lastcall.h"
#include "ui/ui_lastcall.h"
#if CONFIG_CALL_REPLAY
#include "p25/p25_lab.h"
#endif
#if CONFIG_FM_ID
#include "radio/radio_fmid.h"
#endif

static lastcall_list_t list;
static const radio_view_t *tick_view;	/* this tick's view: the letter and DMR's colour code and slot */

void ui_lastcall_view(const radio_view_t *v)
{
	tick_view = v;
}

const lastcall_list_t *ui_lastcall_list(void)
{
	return &list;
}

void ui_lastcall_reset(void)
{
	lastcall_clear(&list);
}

void ui_lastcall_recent(const ui_recent_t *r, bool new_call)
{
	if (r->out)
		return;		/* a call we made */
	const radio_view_t *v = tick_view != NULL ? tick_view : ui_view();
	lastcall_t k;
	memset(&k, 0, sizeof k);
	k.mode = r->mode == 0 ? (uint8_t)RADIO_MODE_DMR : r->mode;
	if (r->mode == RADIO_MODE_YSF || (r->mode >= 0x80u && r->name[0] != '\0')) {
		if (r->name[0] == '\0')
			return;
		lastcall_pack(r->name, &k);	/* a YSF callsign, a DTMF string (FM caller ID) */
	} else {
		k.id = r->src & 0xFFFFFFu;
		k.tg = r->dst & 0xFFFFFFu;
		if (!r->group && r->mode < 0x80u)
			k.flags |= LASTCALL_PRIV;
	}
	if (r->enc)
		k.flags |= LASTCALL_ENC;
	k.nac = r->nac;
	if (k.mode == RADIO_MODE_DMR)
		k.nac = (uint16_t)((v->dmr.cc & 15u) | (v->dmr.ts & 3u) << 4);
#if CONFIG_DUAL_WATCH
	k.letter = (uint8_t)ui_dual_letter_here(v);
#endif
	lastcall_note(&list, &k, new_call, ui_st.now_ms);
}

int ui_lastcall_playable(void)
{
#if CONFIG_CALL_REPLAY
	uint16_t tg = 0;
	uint32_t src = 0;
	if (p25_lab_replay_info(&tg, &src, NULL) == 0)
		return -1;
	for (uint8_t i = 0; i < list.n; i++) {
		const lastcall_t *e = &list.e[i];
		if (e->mode != RADIO_MODE_P25)
			continue;
		/* only the newest P25 call: the receiver keeps one */
		return e->tg == tg && (e->id == src || e->id == 0 || src == 0) && !(e->flags & LASTCALL_ENC) ? i : -1;
	}
#endif
	return -1;
}

static const char *mode_name(uint8_t mode)
{
	switch (mode) {
	case RADIO_MODE_DMR: return "DMR";
	case RADIO_MODE_P25: return "P25";
	case RADIO_MODE_YSF: return "YSF";
	case RADIO_MODE_NXDN: return "NXDN";
	case RADIO_MODE_DPMR: return "dPMR";
	default: return "FM";
	}
}

/* Recent's entry for the same station (its name), NULL for none */
static const ui_recent_t *recent_of(const lastcall_t *e)
{
	for (uint8_t i = 0; i < ui_recent_count(); i++) {
		const ui_recent_t *r = ui_recent_get(i);
		uint8_t mode = r->mode == 0 ? (uint8_t)RADIO_MODE_DMR : r->mode;
		if (r->out || mode != e->mode)
			continue;
		if (e->flags & LASTCALL_TEXT) {
			lastcall_t t;
			memset(&t, 0, sizeof t);
			lastcall_pack(r->name, &t);
			if (t.id == e->id && t.tg == e->tg)
				return r;
		} else if ((r->src & 0xFFFFFFu) == e->id && (r->dst & 0xFFFFFFu) == e->tg) {
			return r;
		}
	}
	return NULL;
}

/* The caller: "Joe", "3101234", "TA2DOU", "MDC 1A2B", "DTMF 1234#" */
static void who(sbuf_t *b, const lastcall_t *e)
{
	const ui_recent_t *r = recent_of(e);
	char text[CODEPLUG_NAME_LEN + 1];
	if (e->flags & LASTCALL_TEXT) {
		if (r != NULL)
			str_copy(text, sizeof text, r->name);
		else
			lastcall_unpack(e, text);
	} else {
		text[0] = '\0';
	}
#if CONFIG_FM_ID
	if (e->mode >= RADIO_FMID_MDC) {
		char line[40];
		radio_fmid_text(e->mode, (uint16_t)e->id, (uint16_t)e->tg, text, line, sizeof line);
		sb_str(b, line);
		return;
	}
#endif
	if (text[0] != '\0')
		sb_str(b, text);
	else if (r != NULL && r->name[0] != '\0')
		sb_str(b, r->name);
	else if (e->id != 0)
		sb_u32(b, e->id, 1);
	else
		sb_chr(b, '?');
}

/* "TG2862 ", ">3102222 " (the unit called), "" (YSF, FM caller IDs) */
static void dest(sbuf_t *b, const lastcall_t *e)
{
	if ((e->flags & LASTCALL_TEXT) || e->mode >= 0x80u || e->mode == RADIO_MODE_YSF)
		return;
	sb_str(b, (e->flags & LASTCALL_PRIV) ? GLYPH_ARROW : "TG");
	sb_u32(b, e->tg, 1);
	sb_chr(b, ' ');
}

/* "3m": the age in one unit */
static void age(sbuf_t *b, const lastcall_t *e)
{
	uint32_t s = lastcall_age_s(e, ui_st.now_ms);
	char unit = s < 60u ? 's' : s < 3600u ? 'm' : s < 86400u ? 'h' : 'd';
	sb_u32(b, s < 60u ? s : s < 3600u ? s / 60u : s < 86400u ? s / 3600u : s / 86400u, 1);
	sb_chr(b, unit);
}

/* --- The list --- */

static uint16_t lc_count(void)
{
	return list.n;
}

static void lc_item(uint16_t i, ui_item_t *it)
{
	if (i >= list.n)
		return;
	const lastcall_t *e = &list.e[i];
	sbuf_t b;
	/* "B DMR Joe" */
	sb_init(&b, it->label, sizeof it->label);
	if (e->letter != 0) {
		sb_chr(&b, (char)e->letter);
		sb_chr(&b, ' ');
	}
	sb_str(&b, mode_name(e->mode));
	sb_chr(&b, ' ');
	who(&b, e);
	/* "> TG2862 2m", where that does not fit "> 2m"; "Encrypted 2m" */
	sb_init(&b, it->value, sizeof it->value);
	bool play = ui_lastcall_playable() == (int)i;
	if (e->flags & LASTCALL_ENC) {
		it->flags |= UI_ITEM_WARN;
		sb_str(&b, TR("Encrypted"));
		sb_chr(&b, ' ');
		age(&b, e);
		return;
	}
	sb_str(&b, TEXT_ALT_OPEN);
	if (play)
		sb_str(&b, "> ");
	dest(&b, e);
	age(&b, e);
	sb_str(&b, TEXT_ALT_NEXT);
	if (play)
		sb_str(&b, "> ");
	age(&b, e);
	sb_str(&b, TEXT_ALT_CLOSE);
}

/* OK: the newest P25 call plays again on the P25 channel in use (Tools > Last call's replay, as before); again stops */
static void lc_select(uint16_t i, bool hold)
{
	(void)hold;
#if CONFIG_CALL_REPLAY
	if (p25_lab_replaying()) {
		p25_lab_replay(0);
		ui_toast(TR("Replay stopped"));
		return;
	}
	if ((int)i != ui_lastcall_playable()) {
		ui_toast(TR("No audio kept for this call"));
		return;
	}
	const radio_view_t *rv = ui_view();
	bool p25 = !rv->vfo && codeplug_mode_p25(rv->ch.mode);
#if CONFIG_YSF
	p25 = p25 && !codeplug_mode_ysf(rv->ch.mode);
#endif
	if (!p25) {
		ui_toast(TR("Last call: go to a P25 channel"));
		return;
	}
	if (!p25_lab_replay(1)) {
		ui_toast(TR("No call recorded yet"));
		return;
	}
	ui_go_home();
	ui_toast(TR("Playing the last call"));
#else
	(void)i;
	ui_toast(TR("No audio kept for this call"));
#endif
}

const ui_scr_def_t ui_lastcall_screen = {
	.name = "lastcall", .flags = SF_LIST | SF_FIELD, .prefix = TRN("Last calls"), .hint = TRN("OK play  Back"),
	.empty = TRN("No calls heard yet"), .count = lc_count, .item = lc_item, .select = lc_select,
};
#endif
