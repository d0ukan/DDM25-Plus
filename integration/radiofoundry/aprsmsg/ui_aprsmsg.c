/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus Tools > APRS: Messages, New message, SOS (emergency) (aprs/aprs_plus.h, docs/plus.md). Installed as
 * core/ui/ui_aprsmsg.c; ui_aprs.c's last three rows call in here.
 *   - Messages: the APRS messages to us, newest first ("FROM: text" and its age, unread ones marked); OK opens one
 *     (from, how long ago, the text), # (or the message's Reply) opens the composer with the sender filled in.
 *   - New message: the composer. To (callsign-SSID) and Text are typed with the radio's text entry (ui_menus.c), or
 *     a canned text is picked; Send hands them to the engine (acks, 3 tries). The line above the rows says how the
 *     last message went: sending (try n of 3), delivered (acked), not delivered; or why nothing can be sent.
 *   - SOS: a confirmation page first (OK sends, Back doesn't); then the emergency position (Mic-E "Emergency") at once
 *     and every 2 minutes until the row is turned off. RAM only: off after power-off. The row shows "On, next m:ss".
 * Toasts (ui_aprsmsg_tick, every UI tick): our message sent / delivered / not delivered, a new message, an SOS sent,
 * and "EMERGENCY: <call>" when a station's Mic-E emergency position is heard. Static buffers throughout: the UI
 * task's stack is tight. */
#include "config.h"
#if CONFIG_APRS_MSG
#include <string.h>
#include "ui_nav.h"
#include "ui_screens.h"
#include "fmt.h"
#include "i18n.h"
#include "aprs/aprs_plus.h"

_Static_assert(APRS_MSG_TEXT_MAX == 67, "ui_menus.c's text entry (TEXT_MAX) holds an APRS message");

static char c_to[APRS_ADDRESSEE_LEN + 1];	/* the composer's addressee ... */
static char c_text[APRS_MSG_TEXT_MAX + 1];	/* ... and text */
static char sent_to[APRS_ADDRESSEE_LEN + 1];	/* the addressee of the message last sent */
static aprs_msg_t am;				/* one received message */
static char page[APRS_MSG_TEXT_MAX + 64];	/* the screen on top's body */
static char tb[40];				/* a toast */

static const char *why(uint8_t block)
{
	const char *w = ui_aprs_why(block);
	return w != NULL ? w : block == APRS_NO_POSITION ? TR("No position: GPS or a fixed one") : TR("Can't send now");
}

/* The composer with an addressee (NULL: as it was), right above Tools > APRS (the screen stack is 6 deep) */
static void compose_open(const char *to)
{
	if (to != NULL) {
		str_copy(c_to, sizeof c_to, to);
		c_text[0] = '\0';
	}
	while (ui_depth() > 0 && ui_frame()->scr != SCR_APRS)
		ui_pop();
	ui_push(SCR_APRS_COMPOSE, 0);
}

/* The composer to a station on top of the screen in use (Tools > Map's APRS station, CONFIG_MAP): Back returns there */
void ui_aprsmsg_compose_to(const char *to)
{
	str_copy(c_to, sizeof c_to, to);
	c_text[0] = '\0';
	ui_push(SCR_APRS_COMPOSE, 0);
}

#if CONFIG_APRS_STATION
/* DDM32 Plus APRS station page (ui_aprsx.h): the message we sent last, to whom (NULL: none since power on) */
const char *ui_aprsmsg_sent(const char **text)
{
	*text = c_text;
	return sent_to[0] != '\0' ? sent_to : NULL;
}
#endif

/* --- Messages --- */

#if CONFIG_APRS_RX
static uint16_t inbox_count(void) { return aprs_msg_count(); }

static void inbox_item(uint16_t i, ui_item_t *it)
{
	if (!aprs_msg_peek((uint8_t)i, &am))
		return;
	sbuf_t b;
	sb_init(&b, it->label, sizeof it->label);
	sb_str(&b, am.from);
	sb_str(&b, ": ");
	sb_str(&b, am.text);
	ui_sb_age(it->value, sizeof it->value, ui_st.now_ms - am.ms);
	it->flags |= UI_ITEM_PREVIEW;
	if (!am.read)
		it->flags |= UI_ITEM_WARN;	/* unread */
}

static void inbox_select(uint16_t i, bool hold)
{
	(void)hold;
	ui_push(SCR_APRS_READ, i);
}

static void inbox_context(uint16_t i)
{
	if (aprs_msg_peek((uint8_t)i, &am))
		compose_open(am.from);
}

const ui_scr_def_t ui_aprs_inbox_screen = {
	.name = "aprs-inbox", .flags = SF_LIST, .prefix = TRN("Messages"), .hint = TRN("OK open  # reply  Back"),
	.empty = TRN("No messages yet"),
	.count = inbox_count, .item = inbox_item, .select = inbox_select, .context = inbox_context,
};

/* One message: read (marked so) when it opens; the list may move under it, the copy stays */
static void read_enter(void)
{
	if (!aprs_msg((uint8_t)ui_frame()->arg, &am))
		memset(&am, 0, sizeof am);
}

static uint16_t read_count(void) { return am.from[0] != '\0' ? 1u : 0u; }

static void read_item(uint16_t i, ui_item_t *it)
{
	(void)i;
	str_copy(it->label, sizeof it->label, TR("Reply"));
}

static void read_select(uint16_t i, bool hold)
{
	(void)i;
	(void)hold;
	compose_open(am.from);
}

static void read_context(uint16_t i)
{
	read_select(i, false);
}

static const char *read_body(void)
{
	if (am.from[0] == '\0')
		return TR("Message deleted");
	sbuf_t b;
	sb_init(&b, page, sizeof page);
	sb_str(&b, TR("From "));
	sb_str(&b, am.from);
	sb_str(&b, ", ");
	sb_age_long(&b, ui_st.now_ms - am.ms);
	sb_chr(&b, '\n');
	sb_str(&b, am.text);
	return page;
}

const ui_scr_def_t ui_aprs_read_screen = {
	.name = "aprs-read", .flags = SF_LIST, .prefix = TRN("Message"), .hint = TRN("OK reply  Back"),
	.count = read_count, .item = read_item, .select = read_select, .context = read_context, .body = read_body,
	.enter = read_enter,
};
#endif

/* --- New message --- */

enum { CP_TO, CP_TEXT, CP_CANNED, CP_SEND, CP_COUNT };

static uint16_t compose_count(void) { return CP_COUNT; }

static void compose_item(uint16_t i, ui_item_t *it)
{
	static const char *const names[CP_COUNT] = { TRN("To"), TRN("Text"), TRN("Canned text"), TRN("Send") };
	str_copy(it->label, sizeof it->label, tr(names[i]));
	item_num_set(it, (uint16_t)(i + 1u));
	if (i == CP_TO)
		str_copy(it->value, sizeof it->value, c_to[0] != '\0' ? c_to : TR("Not set"));
	else if (i == CP_TEXT)
		str_copy(it->value, sizeof it->value, c_text[0] != '\0' ? c_text : TR("Not set"));
	else if (i == CP_SEND && (c_to[0] == '\0' || c_text[0] == '\0'))
		it->flags |= UI_ITEM_DIM;
}

static void compose_send(void)
{
	aprs_plus_t s;
	aprs_plus_state(&s);
	if (s.block != APRS_OK) {
		ui_toast(why(s.block));
		return;
	}
	switch (aprs_compose_send(c_to, c_text)) {
	case APRS_SEND_OK:
		str_copy(sent_to, sizeof sent_to, c_to);
		ui_toast(TR("Sending message"));
		break;
	case APRS_SEND_BAD_TO: ui_toast(TR("Type a callsign, like TA1ABC-7")); break;
	case APRS_SEND_NO_TEXT: ui_toast(TR("Type a text or pick one")); break;
	default: ui_toast(TR("Wait: the last message is still going")); break;
	}
	ui_dirty();
}

static void compose_select(uint16_t i, bool hold)
{
	(void)hold;
	switch (i) {
	case CP_TO: ui_text_edit(c_to, APRS_ADDRESSEE_LEN, NULL, 0); break;
	case CP_TEXT: ui_text_edit(c_text, APRS_MSG_TEXT_MAX, NULL, 1); break;
	case CP_CANNED: ui_push(SCR_APRS_CANNED, 0); break;
	default: compose_send(); break;
	}
}

/* How the last message went, or why none can go */
static const char *compose_body(void)
{
	aprs_plus_t s;
	aprs_plus_state(&s);
	if (s.block != APRS_OK)
		return why(s.block);
	sbuf_t b;
	sb_init(&b, page, sizeof page);
	switch (s.out) {
	case APRS_OUT_SENDING:
		sb_str(&b, TR("Sending to "));
		sb_str(&b, sent_to);
		if (s.tries != 0) {
			sb_str(&b, TR(", try "));
			sb_u32(&b, s.tries, 1);
			sb_str(&b, TR(" of "));
			sb_u32(&b, APRS_MSG_TRIES, 1);
		}
#if CONFIG_APRS_RX
		if (!aprs_rx_listening()) {
			sb_chr(&b, '\n');
			sb_str(&b, TR("Acks are heard on the APRS frequency only"));
		}
#endif
		break;
	case APRS_OUT_DELIVERED:
		sb_str(&b, TR("Delivered to "));
		sb_str(&b, sent_to);
		break;
	case APRS_OUT_FAILED:
		sb_str(&b, TR("Not delivered to "));
		sb_str(&b, sent_to);
		sb_str(&b, TR(": no ack"));
		break;
	default:
		return TR("Type the call and the text, then Send");
	}
	return page;
}

const ui_scr_def_t ui_aprs_compose_screen = {
	.name = "aprs-compose", .flags = SF_LIST | SF_NUMBERED, .prefix = TRN("New message"),
	.hint = TRN("0-9 pick  Back"),
	.count = compose_count, .item = compose_item, .select = compose_select, .body = compose_body,
};

/* Canned texts: OK puts one in the text */
static uint16_t canned_count(void) { return APRS_CANNED_N; }

static void canned_item(uint16_t i, ui_item_t *it)
{
	sbuf_t b;
	sb_init(&b, it->label, sizeof it->label);
	sb_chr(&b, '"');
	sb_str(&b, aprs_canned[i]);
	sb_chr(&b, '"');
	item_num_set(it, (uint16_t)(i + 1u));
}

static void canned_select(uint16_t i, bool hold)
{
	(void)hold;
	str_copy(c_text, sizeof c_text, aprs_canned[i]);
	ui_pop();
}

const ui_scr_def_t ui_aprs_canned_screen = {
	.name = "aprs-canned", .flags = SF_LIST | SF_NUMBERED, .prefix = TRN("Canned text"), .hint = TRN("0-9 pick  Back"),
	.count = canned_count, .item = canned_item, .select = canned_select,
};

/* --- SOS: nothing goes out without this page's OK --- */

static uint16_t sos_count(void) { return 1; }

static void sos_item(uint16_t i, ui_item_t *it)
{
	(void)i;
	str_copy(it->label, sizeof it->label, TR("Send SOS"));
	it->flags |= UI_ITEM_WARN;
}

static void sos_select(uint16_t i, bool hold)
{
	(void)i;
	(void)hold;
	uint8_t w = aprs_sos_set(true);
	if (w != APRS_OK) {
		ui_toast(why(w));
		return;
	}
	ui_pop();
	ui_toast(TR("SOS on: sending"));
}

static const char *sos_body(void)
{
	return TR("Send an emergency position now and every 2 minutes until turned off?");
}

const ui_scr_def_t ui_aprs_sos_screen = {
	.name = "aprs-sos", .flags = SF_LIST, .prefix = TRN("SOS (emergency)"), .hint = TRN("OK yes  Back no"),
	.count = sos_count, .item = sos_item, .select = sos_select, .body = sos_body,
};

/* --- Tools > APRS's rows --- */

void ui_aprsmsg_item(uint8_t which, ui_item_t *it)
{
	aprs_plus_t s;
	aprs_plus_state(&s);
	sbuf_t b;
	sb_init(&b, it->value, sizeof it->value);
	if (which == 0) {
		if (!CONFIG_APRS_RX) {
			it->flags |= UI_ITEM_DIM;
		} else if (s.unread != 0) {
			sb_u32(&b, s.unread, 1);
			sb_str(&b, TR(" new"));
			it->flags |= UI_ITEM_WARN;
		} else {
			sb_u32(&b, s.messages, 1);
		}
	} else if (which == 1) {
		sb_str(&b, s.out == APRS_OUT_SENDING ? TR("Sending") : s.out == APRS_OUT_DELIVERED ? TR("Delivered") :
			   s.out == APRS_OUT_FAILED ? TR("Not delivered") : "");
	} else if (s.sos) {
		sb_str(&b, TR("On, next "));
		sb_mmss(&b, s.sos_next_s);
		it->flags |= UI_ITEM_WARN;
	} else {
		sb_str(&b, TR("Off"));
	}
}

void ui_aprsmsg_select(uint8_t which)
{
	if (which == 0) {
		if (!CONFIG_APRS_RX)
			ui_toast(TR("This radio can't receive APRS"));
		else
			ui_push(SCR_APRS_INBOX, 0);
	} else if (which == 1) {
		compose_open(NULL);
	} else {
		aprs_plus_t s;
		aprs_plus_state(&s);
		if (s.sos) {
			aprs_sos_set(false);
			ui_toast(TR("SOS off"));
			ui_dirty();
		} else {
			ui_push(SCR_APRS_SOS, 0);
		}
	}
}

/* --- Every UI tick: the toasts; true when a screen of ours (or Tools > APRS) is on top and something changed --- */

bool ui_aprsmsg_tick(uint8_t scr)
{
	static aprs_plus_t last;
	static bool primed;
	aprs_plus_t s;
	aprs_plus_state(&s);
	bool changed = memcmp(&s, &last, sizeof s) != 0;
	if (primed && changed) {
		sbuf_t b;
		sb_init(&b, tb, sizeof tb);
		if (s.emergencies != last.emergencies) {
			char call[AX25_CALL_LEN + 4];
			aprs_sos_heard(call);
			sb_str(&b, TR("EMERGENCY: "));
			sb_str(&b, call);
			ui_toast(tb);
		} else if (s.received != last.received) {
			ui_toast(TR("New APRS message"));
		} else if (s.out != last.out || s.tries != last.tries) {
			if (s.out == APRS_OUT_DELIVERED) {
				ui_toast(TR("Message delivered"));
			} else if (s.out == APRS_OUT_FAILED) {
				ui_toast(TR("Message not delivered"));
			} else if (s.out == APRS_OUT_SENDING && s.tries != 0) {
				sb_str(&b, TR("Message sent, try "));
				sb_u32(&b, s.tries, 1);
				sb_str(&b, TR(" of "));
				sb_u32(&b, APRS_MSG_TRIES, 1);
				ui_toast(tb);
			}
		} else if (s.sos_sent > last.sos_sent) {
			ui_toast(TR("SOS position sent"));
		}
	}
	primed = true;
	last = s;
	return changed && (scr == SCR_APRS || scr == SCR_APRS_INBOX || scr == SCR_APRS_COMPOSE);
}
#endif
