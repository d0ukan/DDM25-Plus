/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus Tools > CW decoder and its CW practice page (docs/plus.md "CW decoder and practice").
 *
 * CW decoder (SCR_CW): the speed and tone found, a mark lamp, a status line, then the text decoded as lines that
 * scroll up (the newest at the bottom). The decoder listens only while this screen is on top (ui_msg_tick: radio_msg
 * RADIO_MSG_CW; radio/radio_cw.h). OK clears the text, # opens the practice page, Back leaves.
 *
 * CW practice (SCR_CW_TRAIN): Koch method. Groups of five characters, or one character at a time, drawn from the first
 * n characters of the Koch order (K M U R E S N A P T ...), played at the speed and tone set, through the speaker only
 * (radio/cw_train.h: the beep generator; nothing here can key the transmitter). The answer is typed with the radio's
 * keypad letters as in its text entry (2 ABC ... 9 WXYZ, the same key again within a second for the next letter, the
 * digit after the letters, a held key its digit; 1 . , ? / = then 1; 0 is 0), Right ends a letter at once, Left or
 * Back deletes. OK checks it (characters right by edit distance) and shows what was sent; with nothing typed OK plays
 * the next. In Characters mode the next one plays by itself a second after the check. OK held plays it again (* is
 * the radio's quick settings everywhere), # sets the score to zero. Up / Down pick a setting, Left / Right change it,
 * while nothing is typed. RAM only. This is a simplified entry of its own (the same keys and timing as the radio's
 * text entry), not the text entry screen: the answer stays on the page with the group and the score. */
#include "config.h"
#if CONFIG_CW_RX
#include <string.h>
#include "ui_nav.h"
#include "ui_screens.h"
#include "fmt.h"
#include "i18n.h"
#include "gfx.h"
#include "radio/radio.h"
#include "radio/radio_msg.h"
#include "radio/radio_cw.h"
#include "radio/cw_train.h"

#define MULTI_MS	1000u	/* the same key again within this: the key's next character (the text entry's) */
#define NEXT_MS		1000u	/* Characters mode: the next one after a check */
#define LINE_MAX	40u

/* static: the UI task's stack is tight */
static char line[LINE_MAX + 1u];

/* --- CW decoder --- */

static radio_cw_info_t cin;
static uint32_t cw_seen;

static const char *cw_status(void)
{
	const radio_view_t *v = ui_view();
	uint8_t st = radio_msg_status();
	if (cin.nomem)
		return TR("Not enough memory for CW");
	if (st == RADIO_MSG_ST_OFF && v->ch.mode != RADIO_MODE_FM)
		return TR("CW: tune an FM or AM channel");
	if (st != RADIO_MSG_ST_ON)
		return TR("Waiting for receive audio");
	if (cin.tone_hz == 0)
		return TR("Listening: MCW tone 400-1000 Hz");
	return TR("Decoding");
}

static void cw_draw(void)
{
	const ui_layout_t *L = &ui_layout;
	const font_t *m = L->medium, *f = L->small;
	int w = gfx_width();
	radio_cw_info(&cin);
	gfx_clear(ui_col.bg);
	int y = ui_draw_top(ui_view(), &ui_st, TR("CW decoder"), NULL) + 2;
	sbuf_t b;
	sb_init(&b, line, sizeof line);
	if (cin.wpm != 0)
		sb_u32(&b, cin.wpm, 1);
	else
		sb_str(&b, "--");
	sb_str(&b, " WPM  ");
	if (cin.tone_hz != 0)
		sb_u32(&b, cin.tone_hz, 1);
	else
		sb_str(&b, "---");
	sb_str(&b, " Hz");
	gfx_text(m, 0, y, line, ui_col.fg);
	int lamp = m->h - 6;
	if (cin.key)
		gfx_fill(w - lamp - 2, y + 3, lamp, lamp, ui_col.rx);
	else
		gfx_rect(w - lamp - 2, y + 3, lamp, lamp, ui_col.dim);
	y += m->h + 2;
	gfx_text(f, 0, y, cw_status(), ui_col.dim);
	y += f->h + 4;
	gfx_hline(0, y - 2, w, ui_col.line);
	/* the text: character k on line k / cols, the newest lines that fit */
	unsigned cols = (unsigned)(w / f->w);
	cols = cols > LINE_MAX ? LINE_MAX : cols < 1u ? 1u : cols;
	int rows = (L->hint_y - y) / (f->h + 1);
	uint32_t total = radio_cw_written(), first = radio_cw_first();
	if (total > first && rows > 0) {
		uint32_t last = (total - 1u) / cols, top = first / cols;
		if (last - top + 1u > (uint32_t)rows)
			top = last + 1u - (uint32_t)rows;
		for (uint32_t ln = top; ln <= last; ln++, y += f->h + 1) {
			uint32_t from = ln * cols;
			uint16_t skip = 0;
			if (from < first) {
				skip = (uint16_t)(first - from);	/* the line's start is no longer kept */
				from = first;
			}
			radio_cw_copy(from, line, (uint16_t)(cols - skip));
			gfx_text(f, (int)skip * f->w, y, line, ui_col.fg);
		}
	}
	ui_draw_bottom(&ui_st, TR(TEXT_ALT_OPEN "OK clear  # practice  Back" TEXT_ALT_NEXT "OK clear  # practice"
				  TEXT_ALT_CLOSE));
	ui_draw_banner(&ui_st);
}

static bool cw_key(const input_event_t *ev)
{
	if (ev->gesture != GESTURE_PRESS)
		return false;
	if (ev->action == ACT_SELECT) {
		radio_cw_clear();
		ui_dirty();
		return true;
	}
	if (ev->action == ACT_CONTEXT) {
		ui_push(SCR_CW_TRAIN, 0);
		return true;
	}
	return false;
}

static void cw_enter(void)
{
	cw_seen = radio_cw_seq() - 1u;
	ui_dirty();
}

const ui_scr_def_t ui_cw_screen = {
	.name = "cw", .flags = SF_FIELD, .key = cw_key, .draw = cw_draw, .enter = cw_enter,
};

/* --- CW practice --- */

enum { PR_MODE, PR_KOCH, PR_SPEED, PR_TONE, PR_ROWS };
enum { PH_IDLE, PH_PLAY, PH_ANSWER, PH_DONE };

static uint8_t t_chars;			/* Characters mode (else groups) */
static uint8_t t_level = CW_TRAIN_LEVEL_MIN, t_wpm = 20, t_row, t_phase;
static uint16_t t_hz = 600;
static char t_sent[CW_TRAIN_MAX];
static char t_ans[CW_TRAIN_MAX + 1u];
static uint8_t t_n, t_m, t_right;
static uint16_t s_right, s_sent;
static int8_t mt_key = -1;		/* the key being cycled, -1 none */
static uint8_t mt_idx;
static uint32_t mt_ms, t_until, t_rng;

/* An answer being typed (what the arrows and OK act on) */
static bool tr_typing(void)
{
	return (t_phase == PH_PLAY || t_phase == PH_ANSWER) && t_m > 0;
}

static void tr_play(void)
{
	cw_train_play(t_sent, t_n, t_wpm, t_hz);
	t_until = ui_st.now_ms + CW_TRAIN_LEAD_MS + cw_train_ms(t_sent, t_n, t_wpm);
	t_phase = PH_PLAY;
	t_m = 0;
	t_ans[0] = '\0';
	mt_key = -1;
}

static void tr_next(void)
{
	if (t_rng == 0)
		t_rng = ui_st.now_ms * 2654435761u + 1u;
	t_n = t_chars ? 1u : CW_TRAIN_GROUP;
	cw_train_group(&t_rng, t_level, t_n, t_sent);
	tr_play();
}

static void tr_check(void)
{
	t_right = cw_train_right(t_sent, t_n, t_ans, t_m);
	s_right = (uint16_t)(s_right + t_right);
	s_sent = (uint16_t)(s_sent + t_n);
	t_phase = PH_DONE;
	t_until = ui_st.now_ms + NEXT_MS;
	mt_key = -1;
}

/* The keypad's characters, as the radio's text entry has them (only those the practice sends) */
static const char *const keys[10] = { "0", ".,?/=1", "ABC2", "DEF3", "GHI4", "JKL5", "MNO6", "PQRS7", "TUV8", "WXYZ9" };

static void tr_type(uint8_t k, bool held)
{
	if (t_phase != PH_PLAY && t_phase != PH_ANSWER)
		return;
	const char *set = keys[k];
	uint32_t now = ui_st.now_ms;
	if (!held && k == mt_key && t_m > 0 && now - mt_ms < MULTI_MS) {
		mt_idx = (uint8_t)((mt_idx + 1u) % strlen(set));
		t_ans[t_m - 1u] = set[mt_idx];
	} else if (t_m < CW_TRAIN_MAX) {
		mt_idx = 0;
		t_ans[t_m++] = held ? (char)('0' + k) : set[0];
		t_ans[t_m] = '\0';
	}
	mt_key = held ? -1 : (int8_t)k;
	mt_ms = now;
}

static int clampi(int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static void tr_change(int dir)
{
	switch (t_row) {
	case PR_MODE:
		t_chars = (uint8_t)!t_chars;
		break;
	case PR_KOCH:
		t_level = (uint8_t)clampi(t_level + dir, (int)CW_TRAIN_LEVEL_MIN, (int)CW_TRAIN_KOCH_N);
		break;
	case PR_SPEED:
		t_wpm = (uint8_t)clampi(t_wpm + dir, (int)CW_TRAIN_WPM_MIN, (int)CW_TRAIN_WPM_MAX);
		break;
	default:
		t_hz = (uint16_t)clampi(t_hz + dir * (int)CW_TRAIN_HZ_STEP, (int)CW_TRAIN_HZ_MIN, (int)CW_TRAIN_HZ_MAX);
		break;
	}
}

static bool tr_key(const input_event_t *ev)
{
	bool press = ev->gesture == GESTURE_PRESS || ev->gesture == GESTURE_REPEAT;
	uint8_t a = ev->action;
	if (a >= ACT_DIGIT_0 && a <= ACT_DIGIT_9 && (ev->gesture == GESTURE_PRESS || ev->gesture == GESTURE_LONG)) {
		tr_type((uint8_t)(a - ACT_DIGIT_0), ev->gesture == GESTURE_LONG);
		ui_dirty();
		return true;
	}
	if (a == ACT_SELECT && ev->gesture == GESTURE_LONG) {
		if (t_n > 0)
			tr_play();	/* again */
		ui_dirty();
		return true;
	}
	if (!press)
		return false;
	bool typing = tr_typing();
	if (typing && (a == ACT_NAV_LEFT || a == ACT_BACK)) {
		t_ans[--t_m] = '\0';
		mt_key = -1;
	} else if (typing && a == ACT_NAV_RIGHT) {
		mt_key = -1;	/* the letter is done: the same key starts the next one */
	} else if (!typing && (a == ACT_NAV_UP || a == ACT_NAV_DOWN)) {
		t_row = (uint8_t)((t_row + (a == ACT_NAV_DOWN ? 1u : PR_ROWS - 1u)) % PR_ROWS);
	} else if (!typing && (a == ACT_NAV_LEFT || a == ACT_NAV_RIGHT)) {
		tr_change(a == ACT_NAV_RIGHT ? 1 : -1);
	} else if (a == ACT_SELECT && ev->gesture == GESTURE_PRESS) {
		if (typing)
			tr_check();
		else
			tr_next();	/* nothing typed, or the last one checked: the next */
	} else if (a == ACT_CONTEXT && ev->gesture == GESTURE_PRESS) {
		s_right = s_sent = 0;
		ui_toast(TR("Score reset"));
	} else {
		return false;	/* Back with nothing typed: the screen's own Back leaves */
	}
	ui_dirty();
	return true;
}

static int tr_row(int y, uint8_t row, const char *label, const char *value)
{
	const font_t *f = ui_layout.medium;
	bool sel = row == t_row && !tr_typing();
	if (sel)
		gfx_fill(0, y - 1, gfx_width(), f->h + 2, ui_col.focus);
	color_t c = sel ? ui_col.focus_fg : ui_col.fg;
	gfx_text(f, 4, y, label, sel ? c : ui_col.dim);
	gfx_text(f, 4 + 6 * f->w, y, value, c);	/* after the longest label (5 characters) */
	return y + f->h + 3;
}

static void tr_draw(void)
{
	const font_t *f = ui_layout.small, *m = ui_layout.medium;
	gfx_clear(ui_col.bg);
	int y = ui_draw_top(ui_view(), &ui_st, TR("CW practice"), NULL) + 3;
	sbuf_t b;
	y = tr_row(y, PR_MODE, TR("Mode"), t_chars ? TR("Characters") : TR("Groups"));
	sb_init(&b, line, sizeof line);
	sb_u32(&b, t_level, 1);
	sb_str(&b, "  ");
	for (uint8_t i = 0; i < t_level && i < 8u; i++)
		sb_chr(&b, cw_koch[t_level > 8u ? t_level - 8u + i : i]);	/* the newest eight */
	y = tr_row(y, PR_KOCH, TR("Koch"), line);
	sb_init(&b, line, sizeof line);
	sb_u32(&b, t_wpm, 1);
	sb_str(&b, " WPM");
	y = tr_row(y, PR_SPEED, TR("Speed"), line);
	sb_init(&b, line, sizeof line);
	sb_u32(&b, t_hz, 1);
	sb_str(&b, " Hz");
	y = tr_row(y, PR_TONE, TR("Tone"), line) + 4;
	/* what is going on */
	const char *what = t_phase == PH_IDLE ? TR("OK plays the first one") : t_phase == PH_PLAY ? TR("Listen...") :
			   t_phase == PH_ANSWER ? TR("Type what you heard, OK") : NULL;
	if (what != NULL) {
		gfx_text(f, 0, y, what, ui_col.dim);
	} else {
		sb_init(&b, line, sizeof line);
		sb_str(&b, t_right == t_n ? TR("Right:") : TR("Sent:"));
		sb_chr(&b, ' ');
		for (uint8_t i = 0; i < t_n; i++)
			sb_chr(&b, t_sent[i]);
		if (t_n == 1u) {
			char pat[10];
			cw_train_pattern(t_sent[0], pat);
			sb_str(&b, "  ");
			sb_str(&b, pat);
		}
		gfx_text(f, 0, y, line, t_right == t_n ? ui_col.rx : ui_col.warn);
	}
	y += f->h + 3;
	/* the answer */
	sb_init(&b, line, sizeof line);
	sb_str(&b, "> ");
	sb_str(&b, t_ans);
	if (t_phase == PH_PLAY || t_phase == PH_ANSWER)
		sb_chr(&b, '_');
	gfx_text(m, 0, y, line, ui_col.fg);
	y += m->h + 3;
	sb_init(&b, line, sizeof line);
	sb_str(&b, TR("Score"));
	sb_chr(&b, ' ');
	sb_u32(&b, s_right, 1);
	sb_chr(&b, '/');
	sb_u32(&b, s_sent, 1);
	if (s_sent != 0) {
		sb_str(&b, "  ");
		sb_u32(&b, (uint32_t)s_right * 100u / s_sent, 1);
		sb_str(&b, " %");
	}
	gfx_text(f, 0, y, line, ui_col.dim);
	ui_draw_bottom(&ui_st, tr_typing() ? TR(TEXT_ALT_OPEN "OK check  < del  > next" TEXT_ALT_NEXT "OK check <del >next"
					    TEXT_ALT_CLOSE) :
		       TR(TEXT_ALT_OPEN "OK play  hold OK: again" TEXT_ALT_NEXT "OK play  hold: again" TEXT_ALT_CLOSE));
	ui_draw_banner(&ui_st);
}

static void tr_enter(void)
{
	t_phase = PH_IDLE;
	t_n = t_m = 0;
	t_ans[0] = '\0';
	ui_dirty();
}

/* The practice page's time: the playing ends, a refusal, Characters mode's next one */
static bool tr_tick(void)
{
	uint32_t now = ui_st.now_ms;
	if (t_phase == PH_PLAY) {
		if (cw_train_state() == CW_PLAY_REFUSED) {
			t_phase = PH_IDLE;
			ui_toast(TR("Not while transmitting"));
			return true;
		}
		if ((int32_t)(now - t_until) >= 0) {
			t_phase = PH_ANSWER;
			return true;
		}
	} else if (t_phase == PH_DONE && t_chars && (int32_t)(now - t_until) >= 0) {
		tr_next();
		return true;
	}
	return false;
}

const ui_scr_def_t ui_cwtrain_screen = {
	.name = "cw-practice", .flags = SF_FIELD, .key = tr_key, .draw = tr_draw, .enter = tr_enter,
};

bool ui_cw_tick(uint8_t scr)
{
	if (scr == SCR_CW_TRAIN)
		return tr_tick();
	if (scr != SCR_CW)
		return false;
	uint32_t q = radio_cw_seq();
	if (q == cw_seen)
		return false;
	cw_seen = q;
	return true;
}
#endif
