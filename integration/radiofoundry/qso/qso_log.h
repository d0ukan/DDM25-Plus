/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus QSO log (CONFIG_QSO_LOG, docs/plus.md "QSO log"): the call history (Recent, CONFIG_CALL_LOG: 20 entries,
 * RAM only) with a QSO mark per entry and the frequency each was heard or made on, and its read-only export for the
 * PC over the debug link (0x21 [14], qso_log_export; byte format in docs/plus.md).
 *
 * Marks: * on a Recent row marks it as a QSO or takes the mark away (the user's choice then stays: the automatic rule
 * never changes that entry again). Automatic: a heard call is marked when we transmitted on the same frequency
 * within QSO_AUTO_MS before or after it (a DMR call we made is in Recent itself; any transmission is also seen from
 * the radio view's state, so FM and P25 count too).
 *
 * The marks live beside Recent, not in its entries (ui_recent_t is unchanged): bit i is entry i, and recent_add's move
 * of the list (ui_app.c) moves them with it (qso_log_shift). Every change to Recent happens in the UI task between
 * qso_log_begin and qso_log_end (one seqlock word): the debug link's export, in the host link task, copies only while
 * no change is under way and retries otherwise. RAM only (gone at power off, as Recent): 108 B. */
#ifndef PLUS_QSO_LOG_H
#define PLUS_QSO_LOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "radio/radio.h"

#define QSO_AUTO_MS		60000u	/* the automatic mark: our TX this close to the call, on its frequency */
#define QSO_EXPORT_VERSION	1u
#define QSO_EXPORT_HEAD		10u	/* bytes before the entries (after the status byte) */
#define QSO_EXPORT_ENTRY	36u	/* bytes an entry */
#define QSO_NAME_BYTES		16u

/* qso_export entry flags */
#define QSO_F_OUT	0x01u	/* a call we made (DMR) */
#define QSO_F_GROUP	0x02u	/* to a talkgroup (dst is a group) */
#define QSO_F_QSO	0x04u	/* marked as a QSO */
#define QSO_F_USER	0x08u	/* the mark (or its absence) is the user's choice */
#define QSO_F_ENC	0x10u	/* encrypted (heard, not played) */
#define QSO_F_EMERG	0x20u	/* a P25 emergency call */
#define QSO_F_TO_US	0x40u	/* a DMR private call to us */

/* The UI task, around every change to Recent (ui_app.c's tick, its reset, the * key) */
void qso_log_begin(void);
void qso_log_end(const radio_view_t *v, uint32_t now_ms);
/* recent_add moved entries 0 .. i-1 to 1 .. i and put a new one at 0; dup: the old entry i was the same station
 * again (its mark moves to 0 with it), else it was dropped */
void qso_log_shift(uint8_t i, bool dup);
void qso_log_reset(void);

bool qso_log_marked(uint8_t i);
bool qso_log_user(uint8_t i);
uint32_t qso_log_hz(uint8_t i);		/* 0: not known */
/* The * key on Recent row i: the mark on or off, the user's from now on. Returns the new mark */
bool qso_log_toggle(uint8_t i);

/* The export for the debug link: the version, the count and every entry into out (cap bytes), not counting the
 * reply's status byte. Returns the bytes written, 0 when a change was under way (try again) or cap is too small */
size_t qso_log_export(uint8_t *out, size_t cap, uint32_t now_ms);

#endif
