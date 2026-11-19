/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus P25 Phase 1 trunk following (docs/plus.md), receive only. The native P25 receiver listens on a trunked
 * system's control channel; its TSDUs' TSBKs come here (p25_tsbk.c decodes them in the receive worker). The channel
 * plan (IDEN_UP / IDEN_UP_VU, IDEN_UP_TDMA), the system (NET_STS_BCST: WACN, system ID; RFSS_STS_BCST: RFSS, site)
 * and the group voice grants (GRP_V_CH_GRANT, GRP_V_CH_GRANT_UPDT, GRP_V_CH_GRANT_UPDT_EXP) are kept; everything else
 * is ignored. A grant for a followed group (the channel's talkgroup, or every group) on an FDMA voice channel this
 * radio can receive moves the receiver there; the call plays through the normal P25 voice path (an encrypted one stays
 * muted, and an encrypted grant is not followed at all) and the receiver comes back to the control channel when the
 * call ends (a terminator, no frame for P25TK_FADE_MS, or none at all P25TK_NO_CALL_MS after the retune). The one
 * receiver can't hear the control channel while on a voice channel: no other grant preempts a call. Nothing is ever
 * sent: no registration, no affiliation, PTT is denied while following.
 * Pure logic for the radio task (native_lab.c tunes and feeds it); the screen reads a consistent copy. */
#ifndef PLUS_P25TRUNK_H
#define PLUS_P25TRUNK_H

#include <stdbool.h>
#include <stdint.h>

#define P25TK_LOST_MS		3000u	/* on the control channel: no valid TSBK this long = lost (searching again) */
#define P25TK_NO_CALL_MS	1500u	/* on a voice channel: no P25 frame this long after the retune = no call */
#define P25TK_FADE_MS		1000u	/* on a voice channel: no frame this long after the last one = the call ended */
#define P25TK_MOVE_MS		3000u	/* the receiver did not get to the voice channel in this long: back */
#define P25TK_HOLDOFF_MS	2000u	/* after a call ended: its group's updates on that channel are not followed */
#define P25TK_MISS_HOLDOFF_MS	10000u	/* ... after a retune that found no call: no grant of it is, this long */
#define P25TK_IDLE_MS		5000u	/* the screen didn't read the follower this long (it was left): it stops */
#define P25TK_RECENT		6u
#define P25TK_IDENS		16u

typedef enum { P25TK_OFF, P25TK_HUNT, P25TK_CONTROL, P25TK_VOICE } p25tk_state_t;

/* What became of a grant (p25tk_grant_t.flags) */
enum {
	P25TK_G_FOLLOWED = 0x01,	/* the receiver went there */
	P25TK_G_OTHER = 0x02,		/* not a followed group */
	P25TK_G_ENC = 0x04,		/* the grant's service options say encrypted: not followed */
	P25TK_G_TDMA = 0x08,		/* a Phase 2 (TDMA) channel: not followed */
	P25TK_G_NO_PLAN = 0x10,		/* its channel identifier is not known yet (no IDEN_UP): not followed */
	P25TK_G_BAND = 0x20,		/* outside what this radio receives: not followed */
	P25TK_G_BUSY = 0x40,		/* came while a call was followed (or just after it ended): not followed */
};

typedef struct {
	uint32_t hz;		/* the voice channel, 0 = not known */
	uint32_t src;		/* the source unit (from the grant or the call's link control), 0 = not known */
	uint32_t ms;		/* last heard */
	uint16_t tg;
	uint16_t ch;		/* channel identifier (4 bits) and number (12 bits) */
	uint8_t flags;
} p25tk_grant_t;

typedef struct {
	uint8_t state;		/* p25tk_state_t */
	bool all;		/* every group, else follow_tg */
	bool have_net, have_site;
	uint16_t follow_tg;
	uint16_t nac;		/* the receive's NAC filter (0xF7E: any) */
	uint16_t heard_nac;	/* the control channel's, 0xFFFF before one is heard */
	uint32_t cc_hz;
	uint32_t wacn;
	uint16_t sysid;
	uint8_t rfss, site;
	uint16_t rate;		/* valid TSBKs in the last second */
	uint32_t tsbks, bad;	/* valid TSBKs, blocks that failed the CRC */
	uint16_t lost, calls;	/* control channel losses, calls followed */
	p25tk_grant_t call;	/* the call followed now (state VOICE) or last (call.ms 0: none yet) */
	uint8_t n;
	p25tk_grant_t recent[P25TK_RECENT];	/* grants heard, newest first */
} p25tk_view_t;

/* What the receiver heard since it was last tuned where p25tk_poll asked (native_lab.c counts it) */
typedef struct {
	uint32_t on_hz;		/* where the receive runs now, 0 while it moves */
	uint32_t frames;	/* P25 frames since it got there (any kind: voice, terminators, TSBK blocks) */
	uint32_t terms;		/* terminators (TDU, TDULC) since then */
	uint32_t last_ms;	/* when the last frame came (when frames != 0) */
	uint32_t bad;		/* TSBK blocks that failed (a running count) */
	uint16_t nac;		/* the last frame's NAC */
	uint16_t tg;		/* the group and source unit the last voice link control named (0 = none) */
	uint32_t src;
} p25tk_rx_t;

/* The radio task's side. start: the control channel and NAC (the selected P25 channel's) and the group to follow
 * (0 or all: every group) */
void p25tk_start(uint32_t cc_hz, uint16_t nac, uint16_t tg, bool all, uint32_t now_ms);
void p25tk_stop(void);
/* While following: change the groups followed (the system's state is kept) */
void p25tk_follow(uint16_t tg, bool all);
bool p25tk_on(void);
uint16_t p25tk_nac(void);
/* One valid TSBK, octets 0-9 (the CRC checked) */
void p25tk_tsbk(const uint8_t b[10], uint32_t now_ms);
/* On every radio task turn: the frequency the receiver must be on (0: off) */
uint32_t p25tk_poll(const p25tk_rx_t *rx, uint32_t now_ms);
/* Where the receiver must be now (what p25tk_poll returned last; the control channel after start), 0 = off */
uint32_t p25tk_hz(void);
/* Changes whenever what the screen shows does */
uint32_t p25tk_serial(void);
/* A channel (identifier and number) as Hz from the plan heard so far; 0 = unknown. *tdma (may be NULL): a Phase 2 one */
uint32_t p25tk_channel_hz(uint16_t ch, bool *tdma);
/* A consistent copy for the screen (it also tells the follower the screen still looks) */
bool p25tk_snapshot(p25tk_view_t *out);
/* This radio receives there (native_lab.c: the board's bands; host tests define their own) */
bool p25tk_band_ok(uint32_t hz);

void radio_req_p25_trunk(bool on, bool all);	/* on: from the selected P25 channel; off: stop */

#endif
