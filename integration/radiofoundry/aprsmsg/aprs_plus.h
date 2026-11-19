/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus APRS messages and SOS (CONFIG_APRS_MSG, docs/plus.md), the engine side. Installed as
 * core/aprs/aprs_plus.{c,h}. The state that needs the engine's own (the SOS beacon, the message being sent, the
 * emergencies heard) lives in core/aprs/aprs.c, patched by tools/plus_features.py prepare_aprs_msg; its calls are
 * declared here. This file adds what needs none of it:
 *   - the Mic-E encoder (APRS Protocol Reference 1.0.1, chapter 10): the destination address carries the latitude
 *     digits, the three message bits, N / S, the longitude offset and W / E; the information field '`' (current GPS
 *     data), longitude degrees / minutes / hundredths + 28, speed and course, the symbol, an optional altitude
 *     ("xxx}", base 91) and a comment. The emergency position is the one with the three message bits 0 (destination
 *     digits all '0'-'9'): Kenwood and Yaesu radios alarm on it, aprs.fi shows "Emergency";
 *   - the canned texts the message composer offers (APRS text is ASCII: English ham shorthand, not translated);
 *   - the composer's check of an addressee and a text before aprs_send_message. */
#ifndef DM32_APRS_PLUS_H
#define DM32_APRS_PLUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "aprs/aprs.h"
#include "aprs/aprs_fmt.h"

#if CONFIG_APRS_MSG

/* aprs_parsed_t.mice_msg numbering: 0..6 = M0 (Off duty) .. M6 (Priority), 7 = Emergency */
#define APRS_MICE_EMERGENCY	7u
#define APRS_MICE_DST_LEN	6u		/* the destination's six characters */
#define APRS_SOS_PERIOD_MS	120000u		/* the emergency position again every 2 minutes */
#define APRS_SOS_COMMENT	"SOS"

/* A Mic-E position report: dst gets the six destination characters and a NUL, out the information field. msg: 0..6
 * standard message M0..M6, APRS_MICE_EMERGENCY. Latitude beyond +-90 and longitude beyond +-179 59.99' are clamped;
 * speed over 799 kn is sent as 799; course APRS_COURSE_NONE (or 0) as unknown. Returns the length (0: does not fit) */
size_t aprs_mice_build(const aprs_pos_t *pos, uint8_t msg, const char *comment, char *dst, char *out, size_t max);

/* The composer's canned texts (ASCII) */
#define APRS_CANNED_N		8u
extern const char *const aprs_canned[APRS_CANNED_N];

/* aprs_compose_send's results */
enum { APRS_SEND_OK, APRS_SEND_BAD_TO, APRS_SEND_NO_TEXT, APRS_SEND_BUSY };
/* The addressee typed (to: at least APRS_ADDRESSEE_LEN + 1 bytes): spaces dropped, upper case. True when it is a
 * callsign the engine can address ("CALL" or "CALL-SSID", SSID 0-15) */
bool aprs_addressee_fix(char *to);
/* Check and send: the addressee fixed in place, the text's trailing spaces dropped and the characters APRS messages
 * may not carry ('|', '~', '{') replaced by '-', then aprs_send_message. APRS_SEND_BUSY: a message still waits for
 * its ack (or no callsign) */
uint8_t aprs_compose_send(char *to, char *text);

/* --- In core/aprs/aprs.c (prepare_aprs_msg), any task --- */

/* Our message's state: none sent yet, being sent (waiting for the ack, retried), delivered (acked), not delivered
 * (no ack after APRS_MSG_TRIES) */
enum { APRS_OUT_NONE, APRS_OUT_SENDING, APRS_OUT_DELIVERED, APRS_OUT_FAILED };
typedef struct {
	uint8_t block;		/* aprs_block_t for RF now */
	bool pos;		/* a position to send (GPS, host or fixed) */
	uint8_t out;		/* APRS_OUT_* */
	uint8_t tries;		/* our message's transmissions so far */
	bool sos;		/* the emergency beacon is on */
	uint16_t sos_next_s;	/* to its next transmission (0: due now) */
	uint16_t sos_sent;	/* sent since it was turned on */
	uint8_t emergencies;	/* bumped on every emergency (Mic-E) heard; wraps */
	uint8_t messages, unread;	/* messages to us kept (CONFIG_APRS_RX) */
	uint8_t received;	/* bumped on every new message to us; wraps */
} aprs_plus_t;
/* All of it at once (zeroed first: the UI compares copies) */
void aprs_plus_state(aprs_plus_t *s);
/* SOS on: APRS_OK, or why it can't send (aprs_block_t: no callsign, no frequency, not amateur, no AFSK, no position);
 * nothing changes then. On: the emergency position at the next tick, then every APRS_SOS_PERIOD_MS. Off: always */
uint8_t aprs_sos_set(bool on);
/* The station of the last emergency heard (AX25_CALL_LEN + 4 bytes, "" = none) */
void aprs_sos_heard(char *call);
#if CONFIG_APRS_RX
/* Message i (0 = newest) without marking it read */
bool aprs_msg_peek(uint8_t i, aprs_msg_t *out);
#endif

#endif
#endif
