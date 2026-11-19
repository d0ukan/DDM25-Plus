/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus DMR Tier III trunk following (docs/plus.md), receive only. The DMR call stack listens on a trunked site's
 * control channel (TSCC: the selected DMR channel's frequency and color code, both slots); the CSBKs it hears come here
 * (the HR_C7000 decodes and checks them; radio_sm.c passes them on instead of to the data service). Kept: the system
 * (C_ALOHA / C_BCAST system identity code: model, net, site), the channel plan (MBC absolute channel parameters: an
 * LPCN's frequencies, from a C_BCAST Chan_Freq or a grant with LPCN 0xFFF) and the voice grants (TV_GRANT, BTV_GRANT,
 * PV_GRANT). A grant for a followed group (the channel's talkgroup, its RX group list, or every group) on a channel
 * whose frequency is known and in this radio's bands moves the receiver there, locked to the granted slot; the call
 * plays through the normal DMR voice path (an encrypted call stays muted and the receiver goes back) and the receiver
 * returns to the TSCC when the call is over: P_CLEAR on the payload channel, DMRTK_HANG_MS after its voice ended
 * (terminator or fade), or no voice at all DMRTK_NO_CALL_MS after the retune. Private calls (PV_GRANT) are followed
 * with every group only. The one receiver can't hear the TSCC on a payload channel: no other grant preempts a call.
 * Nothing is ever sent: no registration, no random access, PTT is denied while following.
 * Pure logic for the radio task (radio_sm.c tunes and feeds it); the screen reads a consistent copy. */
#ifndef PLUS_DMRTRUNK_H
#define PLUS_DMRTRUNK_H

#include <stdbool.h>
#include <stdint.h>

#define DMRTK_LOST_MS		3000u	/* on the TSCC: no Tier III CSBK this long = lost (searching again) */
#define DMRTK_NO_CALL_MS	1500u	/* on a payload channel: no voice this long after the retune = no call */
#define DMRTK_HANG_MS		1500u	/* ... no voice this long after the last (terminator or fade): the call is over */
#define DMRTK_MOVE_MS		1000u	/* the receiver did not get to the payload channel in this long: back */
#define DMRTK_HOLDOFF_MS	1000u	/* after a call ended: its grants on that channel are not followed */
#define DMRTK_MISS_HOLDOFF_MS	10000u	/* ... after a retune that found no call, or an encrypted one */
#define DMRTK_IDLE_MS		5000u	/* the screen didn't read the follower this long (it was left): it stops */
#define DMRTK_RECENT		6u
#define DMRTK_PLAN		16u

typedef enum { DMRTK_OFF, DMRTK_HUNT, DMRTK_CONTROL, DMRTK_VOICE } dmrtk_state_t;

/* What became of a grant (dmrtk_grant_t.flags) */
enum {
	DMRTK_G_FOLLOWED = 0x01,	/* the receiver went there */
	DMRTK_G_OTHER = 0x02,		/* not a followed group */
	DMRTK_G_ENC = 0x04,		/* the call turned out encrypted (no key): muted, the receiver went back */
	DMRTK_G_PRIVATE = 0x08,		/* a private call (PV_GRANT), followed with every group only */
	DMRTK_G_NO_PLAN = 0x10,		/* the LPCN's frequency is not known (no Chan_Freq / absolute parameters) */
	DMRTK_G_BAND = 0x20,		/* outside what this radio receives */
	DMRTK_G_BUSY = 0x40,		/* came while a call was followed (or just after it ended) */
};

typedef struct {
	uint32_t hz;		/* the payload channel, 0 = not known */
	uint32_t tg;		/* the target: a talkgroup, or the called unit of a private call */
	uint32_t src;		/* the calling unit, 0 = not known */
	uint32_t ms;		/* last heard */
	uint16_t lpcn;		/* logical physical channel number */
	uint8_t ts;		/* 1 or 2 */
	uint8_t flags;		/* DMRTK_G_xxx */
	uint8_t op;		/* CSBKO: TV_GRANT, BTV_GRANT, PV_GRANT */
	bool emergency;
} dmrtk_grant_t;

typedef struct {
	uint8_t state;		/* dmrtk_state_t */
	bool all;		/* every group (and private calls), else follow_tg and the RX group list */
	bool have_sys;
	uint8_t model;		/* system identity code: 0 Tiny, 1 Small, 2 Large, 3 Huge */
	uint16_t net, site;	/* as sent (the standard shows them plus 1) */
	uint32_t follow_tg;
	uint32_t cc_hz;
	uint16_t rate;		/* Tier III CSBKs in the last second */
	uint32_t csbks, bad;	/* Tier III CSBKs, CSBKs that failed the CRC */
	uint16_t lost, calls;	/* TSCC losses, calls followed */
	uint16_t bcasts, mbcs;	/* C_BCAST announcements, MBC channel parameters heard */
	uint8_t plan_n;		/* LPCNs whose frequency is known */
	dmrtk_grant_t call;	/* the call followed now (state VOICE) or last (call.ms 0: none yet) */
	uint8_t n;
	dmrtk_grant_t recent[DMRTK_RECENT];	/* grants heard, newest first */
} dmrtk_view_t;

/* What the receiver does now (radio_sm.c reads the call stack for it) */
enum { DMRTK_RX_NONE, DMRTK_RX_PLAY, DMRTK_RX_MUTED };
typedef struct {
	uint32_t on_hz;		/* where the receiver is */
	uint8_t on_ts;		/* the slot it is locked to, 0 = both */
	uint8_t call;		/* DMRTK_RX_xxx: a voice call played, or heard but not played (encrypted, no key) */
	bool enc;
	uint32_t src, dst;	/* the call's */
} dmrtk_rx_t;

/* The radio task's side. start: the TSCC (the selected DMR channel's frequency), the group to follow (0 or all: every
 * group) and the channel's RX group list (kept by pointer: the caller's, unchanged while following) */
void dmrtk_start(uint32_t cc_hz, uint32_t tg, bool all, const uint32_t *groups, uint16_t ngroups, uint32_t now_ms);
void dmrtk_stop(void);
/* While following: change the groups followed (the system's state is kept) */
void dmrtk_follow(uint32_t tg, bool all);
bool dmrtk_on(void);
/* One control burst the call stack heard: kind 3 CSBK, 4 MBC header, 5 MBC continuation (the ETSI data types), ts 1
 * or 2 (0 = not known), its 12 octets (CRC included) */
void dmrtk_burst(uint8_t kind, int8_t ts, const uint8_t b[12], uint32_t now_ms);
/* On every radio task turn: the frequency the receiver must be on (0: off); dmrtk_ts() the slot */
uint32_t dmrtk_poll(const dmrtk_rx_t *rx, uint32_t now_ms);
/* Where the receiver must be now (what dmrtk_poll returned last; the TSCC after start), 0 = off */
uint32_t dmrtk_hz(void);
/* The slot it must be locked to: 0 on the TSCC (both), else the payload channel's */
uint8_t dmrtk_ts(void);
/* Changes whenever what the screen shows does */
uint32_t dmrtk_serial(void);
/* An LPCN's downlink frequency from the plan heard so far, 0 = unknown */
uint32_t dmrtk_channel_hz(uint16_t lpcn);
/* A consistent copy for the screen (it also tells the follower the screen still looks) */
bool dmrtk_snapshot(dmrtk_view_t *out);
/* This radio receives there (radio_sm.c: the board's bands; host tests define their own) */
bool dmrtk_band_ok(uint32_t hz);

void radio_req_dmr_trunk(bool on, bool all);	/* on: from the selected DMR channel; off: stop */

#endif
