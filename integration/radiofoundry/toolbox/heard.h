/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus heard list (CONFIG_HEARD, docs/plus.md "Heard list"): every station heard on P25, DMR, YSF, NXDN and
 * dPMR, one entry per (mode, source, destination), newest first, with how many calls and when the last one was.
 * Fed where Recent (the call history) is fed (ui_app.c recent_add, and while a DMR / P25 call goes on); unlike
 * Recent it keeps a station once, counts its calls and keeps the frequency it was heard on. RAM only, gone at power
 * off. The portable part (no RTOS): the list, the merge and the order; installed as core/ui/heard.{c,h}. The screens
 * are ui_heard.c.
 *
 * An entry is 16 bytes: the source and the destination take 24 bits each (DMR, P25 and dPMR IDs are 24-bit, NXDN
 * 16-bit; a YSF station has only a callsign, packed into those 48 bits as 8 characters of 6 bits), the call count 8
 * bits (saturating at 255), the frequency 32, the last-heard time 20 bits of seconds (an entry older than
 * HEARD_AGE_MAX_S is dropped before the 20-bit clock could wrap) and the NAC (P25), RAN (NXDN), colour code (dPMR)
 * or colour code and slot (DMR: CC | slot << 4) 12 bits. */
#ifndef PLUS_HEARD_H
#define PLUS_HEARD_H

#include <stdbool.h>
#include <stdint.h>

#define HEARD_MAX	16u
#define HEARD_GAP_S	5u		/* heard again after this much quiet: another call */
#define HEARD_AGE_MAX_S	(1ul << 19)	/* about 6 days: older entries go (the clock below wraps at 2^20 s) */
#define HEARD_CLOCK_MASK	0xFFFFFul
#define HEARD_PRIV	0x80u		/* kind flag: the destination is a unit (a private / unit-to-unit call) */
#define HEARD_MODE(k)	((uint8_t)((k) & 0x7Fu))	/* RADIO_MODE_DMR, _P25, _YSF, _NXDN, _DPMR */
#define HEARD_CALL_CHARS	8u

typedef struct {
	uint32_t id : 24;	/* the source unit (YSF: the callsign's first 4 characters) */
	uint32_t count : 8;	/* calls heard, at most 255 */
	uint32_t tg : 24;	/* the destination: talkgroup, or unit with HEARD_PRIV (YSF: the callsign's last 4) */
	uint32_t kind : 8;	/* the mode, HEARD_PRIV */
	uint32_t hz;		/* heard on, 0 = not known */
	uint32_t last_s : 20;	/* when it was last heard: seconds (the UI clock / 1000), mod 2^20 */
	uint32_t nac : 12;	/* P25 NAC, NXDN RAN, dPMR CC, DMR colour code | slot << 4 */
} heard_t;

typedef struct {
	heard_t e[HEARD_MAX];	/* newest first */
	uint8_t n;
} heard_list_t;

void heard_clear(heard_list_t *l);
/* A call heard: the entry with the same kind, id and tg moves to the top (its frequency and NAC as given now), else a
 * new one goes there (the oldest dropped when full). new_call: a new call (the count goes up); else the call goes on
 * and the count goes up only when it had been quiet HEARD_GAP_S. Entries older than HEARD_AGE_MAX_S go first */
void heard_note(heard_list_t *l, const heard_t *k, bool new_call, uint32_t now_s);
/* Seconds since e was last heard */
uint32_t heard_age_s(const heard_t *e, uint32_t now_s);
/* Drop the entries older than HEARD_AGE_MAX_S */
void heard_expire(heard_list_t *l, uint32_t now_s);
/* A YSF callsign into id / tg and back: upper case, A-Z 0-9 - / kept, anything else as '?', cut at 8 characters
 * (trailing spaces dropped) */
void heard_pack_call(const char *call, heard_t *e);
void heard_unpack_call(const heard_t *e, char out[HEARD_CALL_CHARS + 1]);

#endif
