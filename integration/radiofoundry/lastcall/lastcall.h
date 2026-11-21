/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus last calls (CONFIG_LAST_CALLS, docs/plus.md "Last calls", ROADMAP item 43): the last LASTCALL_MAX calls
 * heard in any mode (DMR, P25, YSF, NXDN, dPMR, the FM caller IDs) and on any watched channel (the multi-watch letter
 * A-F, dual watch's A / B), newest first, one entry a call (Recent and the heard list keep a station once). Fed where
 * Recent is fed (ui_app.c recent_add, and while a DMR or P25 call goes on). RAM only. The portable part (no RTOS):
 * the list and its rules; installed as core/ui/lastcall.{c,h}. The screen is ui_lastcall.c.
 *
 * An entry is 16 bytes: the source and the destination 24 bits each (DMR, P25, dPMR IDs; NXDN 16-bit; an FM caller
 * ID's unit and fleet), or, for a station known only by a text (a YSF callsign, a DTMF string), its first 8
 * characters at 6 bits each in those 48 bits (LASTCALL_TEXT); the letter, the flags, when it was last heard (the UI
 * clock, ms), the NAC (P25) / RAN (NXDN) / colour code (dPMR) / colour code | slot << 4 (DMR), and the mode. */
#ifndef PLUS_LASTCALL_H
#define PLUS_LASTCALL_H

#include <stdbool.h>
#include <stdint.h>

#define LASTCALL_MAX	8u
#define LASTCALL_GAP_MS	5000u		/* the same station heard again after this much quiet: another call */
#define LASTCALL_CHARS	8u

/* flags */
#define LASTCALL_PRIV	0x01u		/* the destination is a unit (a private / unit-to-unit call) */
#define LASTCALL_ENC	0x02u		/* encrypted: heard, not played */
#define LASTCALL_TEXT	0x04u		/* id / tg hold the station's text (lastcall_pack) */

typedef struct {
	uint32_t id : 24;	/* the source (LASTCALL_TEXT: the text's first 4 characters) */
	uint32_t letter : 8;	/* the watched channel: 'A'..'F', 0 none */
	uint32_t tg : 24;	/* the destination (LASTCALL_TEXT: the text's last 4) */
	uint32_t flags : 8;
	uint32_t ms;		/* last heard (the UI clock) */
	uint16_t nac;
	uint8_t mode;		/* RADIO_MODE_DMR, _P25, _YSF, _NXDN, _DPMR, or an FM caller ID kind (0x80..) */
	uint8_t spare;
} lastcall_t;

typedef struct {
	lastcall_t e[LASTCALL_MAX];	/* newest first */
	uint8_t n;
} lastcall_list_t;

void lastcall_clear(lastcall_list_t *l);
/* The same station: mode, letter, destination, and source (a source still unknown, 0, matches any: P25's comes with
 * its first LDU1) */
bool lastcall_same(const lastcall_t *a, const lastcall_t *b);
/* A call heard. new_call: a call starts (Recent took it in); else a call going on (heard again). The newest entry of
 * the same station heard within LASTCALL_GAP_MS is that call (for a new call: only with the same, known source: a
 * caller reported twice): it is updated (the source, the flags, the NAC, the time) and moves to the top; otherwise a
 * new entry goes on top (the oldest dropped when full). Returns the entry's index (0) */
uint8_t lastcall_note(lastcall_list_t *l, const lastcall_t *k, bool new_call, uint32_t now_ms);
/* Seconds since e was last heard */
uint32_t lastcall_age_s(const lastcall_t *e, uint32_t now_ms);
/* A text into id / tg (LASTCALL_TEXT) and back: upper case, A-Z 0-9 - / * # kept, anything else as '?', the first 8
 * characters (trailing spaces dropped on the way back) */
void lastcall_pack(const char *text, lastcall_t *e);
void lastcall_unpack(const lastcall_t *e, char out[LASTCALL_CHARS + 1]);

#endif
