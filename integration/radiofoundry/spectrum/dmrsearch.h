/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus DMR search (docs/plus.md): find DMR traffic anywhere in a frequency range, whatever its color code,
 * timeslot and talkgroup. The board stays in DMR the whole time with the call stack hearing everything (hear all,
 * any color code, either slot). The receiver steps across the range reading the signal strength (a fast sweep);
 * a step well above the noise floor is a hit and is probed: the frequency is held while the modem reports bursts.
 * DMR bursts there: the frequency, color code, slots, repeater (BS sync) or mobile (MS sync) and the last call's
 * talkgroup and source go into the found list; a voice call plays and the sweep goes on once it has ended and a
 * short hang has passed. No DMR: the frequency is skipped for a while. Receive only: PTT never transmits while
 * searching (it stops the search). Pure logic: radio_sm.c tunes and reads the call stack (dmr_call_heard). */
#ifndef PLUS_DMRSEARCH_H
#define PLUS_DMRSEARCH_H

#include <stdbool.h>
#include <stdint.h>
#include "config.h"

#define DMRS_SETTLE_MS		30u	/* readings this long after a retune are the step's */
#define DMRS_READS		3u	/* readings per step, a poll (25 ms) apart: a direct mode burst (30 ms in 60) lands
					 * in one of them; the step's level is their peak */
#if CONFIG_SEARCH_FAST
/* DDM32 Plus faster sweep (docs/plus.md "Search ranges and speed"): the first of the DMRS_READS readings at the poll
 * after the retune (25 ms), the others settled, so a quiet step takes three polls instead of four. A strong settled
 * reading is a hit. A strong first reading alone (a burst caught only there, or the step before's level on a slow
 * reading; half the margin counts there, as a burst that began just before it reads only part of its level) gets
 * DMRS_MORE_READS more settled ones, which cover every phase of the 60 ms frame without it; still
 * nothing: the step before, if it was read so, is read again settled */
#define DMRS_QUICK_MS		20u
#define DMRS_MORE_READS		2u
/* A carrier found again not to be DMR within DMRS_AGAIN_MS of its skip running out (an FM repeater, a beacon) is
 * skipped for DMRS_SKIP_LONG_MS: fewer probes of the same busy channels on every pass */
#define DMRS_SKIP_LONG_MS	30000u
#define DMRS_AGAIN_MS		60000u
#endif
#define DMRS_HIT_DB		10	/* a hit: this far above the floor ... */
#define DMRS_HIT_MIN_DBM	(-115)	/* ... and at least this strong */
#define DMRS_PROBE_MS		600u	/* a hit: bursts within this long are DMR (sync comes once per 360 ms superframe in
					 * a voice call, every 60 ms on a repeater; the rest is the modem's lock) */
#define DMRS_MIN_BURSTS		2u	/* bursts with a good check that make it DMR */
#define DMRS_LATE_MS		1500u	/* DMR without a call to play: held this long from the hit for one (late entry
					 * takes the call's LC from the second superframe at worst), then on */
#define DMRS_HANG_MS		2000u	/* a call ended: held this long for a reply, then on */
#define DMRS_SKIP_MS		10000u	/* a carrier that wasn't DMR is not probed again for this long */
#define DMRS_FOUND_MAX		8u
#define DMRS_SKIP_MAX		8u
#define DMRS_REVISIT_STEPS	16u	/* a found frequency in the range is read again after this many steps */
#define DMRS_IDLE_MS		5000u	/* the screen didn't read the search this long (it was left): it stops */
#define DMRS_CC_NONE		0xFFu

/* found / heard sync_mask */
#define DMRS_SYNC_BS		1u	/* a repeater's outbound */
#define DMRS_SYNC_MS		2u	/* a mobile: direct mode (MS or TDMA direct sync), or a repeater's input */

typedef enum { DMRS_OFF, DMRS_SWEEP, DMRS_PROBE, DMRS_CALL } dmrs_phase_t;

/* What the receiver heard since the frequency was tuned: the radio task's reading of the call stack */
typedef struct {
	uint16_t bursts;	/* bursts with a good check */
	uint8_t cc;		/* the color code of the latest burst that names one, DMRS_CC_NONE = none yet */
	uint8_t ts_mask;	/* slots with traffic: bit 0 TS1, bit 1 TS2 (named by a repeater or TDMA direct sync) */
	uint8_t sync_mask;	/* DMRS_SYNC_* */
	bool call;		/* a voice call plays */
	bool other;		/* a call heard but not played (encrypted) */
	bool group;		/* the call's: group call (else private) */
	uint32_t dst, src;	/* the call's talkgroup (or called ID) and source, 0 = not known yet */
} dmrs_heard_t;

typedef struct {
	uint32_t hz;
	uint32_t dst, src;	/* the last call's talkgroup (or called ID) and source, 0 = none heard */
	uint32_t heard_ms;	/* when it was last heard */
	uint16_t calls;		/* voice calls played there */
	uint8_t cc;		/* DMRS_CC_NONE = not named yet */
	uint8_t ts_mask;	/* slots with traffic seen there */
	uint8_t sync_mask;	/* DMRS_SYNC_*: repeater and / or mobile */
	bool group;
	bool enc;		/* a call there was encrypted (not played) */
} dmrs_found_t;

typedef struct {
	uint8_t phase;		/* dmrs_phase_t */
	bool playing;		/* a call plays on at_hz */
	uint8_t found_n;
	int16_t floor_dbm;	/* noise floor estimate (a slow average of the quiet steps) */
	uint32_t lo_hz, hi_hz, step_hz;
	uint32_t at_hz;		/* the step being read, or the frequency being probed / played */
	uint32_t sweeps, hits, found_dmr;
	dmrs_found_t found[DMRS_FOUND_MAX];	/* newest first */
} dmrs_view_t;

/* The radio task's side. start: the first frequency to tune. sweep: on every poll while sweeping, with the reading;
 * returns the next frequency to tune (0 = stay) and sets *hit when the step is a hit (the search then holds it in
 * DMRS_PROBE). probe: on every poll while probing or playing, with what the receiver heard since the tune; returns
 * the frequency to tune next (0 = stay). Every return other than 0 is a retune: the caller clears what was heard */
uint32_t dmrs_start(uint32_t lo_hz, uint32_t hi_hz, uint32_t step_hz, uint32_t now_ms);
uint32_t dmrs_sweep(int16_t rssi_dbm, uint32_t now_ms, bool *hit);
uint32_t dmrs_probe(const dmrs_heard_t *h, uint32_t now_ms);
void dmrs_stop(void);
bool dmrs_on(void);
uint8_t dmrs_phase(void);
uint32_t dmrs_at_hz(void);
/* A consistent copy for the UI (and a sign that the screen still looks) */
bool dmrs_snapshot(dmrs_view_t *out);
/* The PC's search log: the state and found entry i, not counted as a look (dmrsearch.c) */
typedef struct {
	uint8_t phase, found_n;
	uint32_t at_hz, lo_hz, hi_hz;
	dmrs_found_t f;
} dmrs_peek_t;
bool dmrs_peek(unsigned i, dmrs_peek_t *out);

void radio_req_dmr_search(uint32_t lo_hz, uint32_t hi_hz);	/* lo 0 = stop */

#endif
