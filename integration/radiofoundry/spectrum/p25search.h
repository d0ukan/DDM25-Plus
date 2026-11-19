/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus P25 search (docs/plus.md): find P25 traffic anywhere in a frequency range, whatever its NAC and
 * talkgroup. The channel receiver steps across the range reading the signal strength (a fast sweep, no audio);
 * a step well above the noise floor is a hit, and the native P25 receiver then probes that frequency with any NAC
 * and any talkgroup. P25 frames there: the call is played and the frequency, NAC, talkgroup and source go into the
 * found list; the sweep goes on once the call has ended. No P25: the frequency is skipped for a while. Receive only:
 * PTT never transmits while searching (it stops the search). Pure logic: radio_sm.c tunes, native_lab.c probes. */
#ifndef PLUS_P25SEARCH_H
#define PLUS_P25SEARCH_H

#include <stdbool.h>
#include <stdint.h>

#define P25S_SETTLE_MS		30u	/* a reading this long after a retune is the step's */
#define P25S_HIT_DB		10	/* a hit: this far above the floor ... */
#define P25S_HIT_MIN_DBM	(-115)	/* ... and at least this strong */
#define P25S_SKIP_MS		10000u	/* a carrier that wasn't P25 is not probed again for this long */
#define P25S_FOUND_MAX		8u
#define P25S_SKIP_MAX		8u
#define P25S_REVISIT_STEPS	16u	/* a found frequency in the range is read again after this many steps */
#define P25S_IDLE_MS		5000u	/* the screen didn't read the search this long (it was left): it stops */

typedef enum { P25S_OFF, P25S_SWEEP, P25S_PROBE } p25s_phase_t;

typedef struct {
	uint32_t hz;
	uint16_t nac, tg;
	uint32_t src;
	uint32_t heard_ms;	/* when it was last heard */
	uint16_t calls;
} p25s_found_t;

typedef struct {
	uint8_t phase;		/* p25s_phase_t */
	uint32_t lo_hz, hi_hz, step_hz;
	uint32_t at_hz;		/* the step being read, or the frequency being probed / played */
	int16_t floor_dbm;	/* noise floor estimate (a slow average of the quiet steps) */
	uint32_t sweeps, hits, probes_p25;
	uint8_t found_n;
	p25s_found_t found[P25S_FOUND_MAX];	/* newest first */
	bool playing;		/* the probe found P25 and the call plays */
} p25s_view_t;

/* The radio task's side. start: the first frequency to tune. sweep: called on every poll while sweeping with the
 * reading; returns the next frequency to tune (0 = stay), and sets *hit when the current step is a hit (the
 * search then waits in P25S_PROBE for p25s_probe_done) */
uint32_t p25s_start(uint32_t lo_hz, uint32_t hi_hz, uint32_t step_hz, uint32_t now_ms);
uint32_t p25s_sweep(int16_t rssi_dbm, uint32_t now_ms, bool *hit);
void p25s_stop(void);
/* The receiver was tuned to p25s_at_hz() just now (the radio task, after something else had it): settle again */
void p25s_retuned(uint32_t now_ms);
/* Changes whenever the search moves to a new frequency outside the sweep's own steps (after a probe) */
uint32_t p25s_epoch(void);
bool p25s_on(void);
uint8_t p25s_phase(void);
uint32_t p25s_at_hz(void);
/* The native receiver's side: a probe of p25s_at_hz() found P25 (playing, with what it heard so far) or ended.
 * found: the call is noted (frequency, NAC, talkgroup, source); done: back to sweeping from the next step,
 * a probe that heard no P25 marks the frequency to skip. Both return the frequency to tune next (0 when off) */
void p25s_probe_found(uint16_t nac, uint16_t tg, uint32_t src, uint32_t now_ms);
uint32_t p25s_probe_done(bool was_p25, uint32_t now_ms);
/* A consistent copy for the UI */
bool p25s_snapshot(p25s_view_t *out);

void radio_req_p25_search(uint32_t lo_hz, uint32_t hi_hz);	/* lo 0 = stop */

#endif
