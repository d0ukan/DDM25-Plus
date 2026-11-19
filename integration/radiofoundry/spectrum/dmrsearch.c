/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus DMR search engine (dmrsearch.h). No RTOS, no hardware. A sibling of p25search.c (the same sweep, skip
 * list and revisits) kept separate so that either search can be built without the other. */
#include "config.h"
#if CONFIG_DMR_SEARCH
#include <string.h>
#include "radio/dmrsearch.h"

static dmrs_view_t s;
static uint32_t tuned_ms;
static int32_t floor_q4;		/* floor in dBm x 16 */
static int16_t peak;			/* the step's strongest reading so far */
static uint8_t nreads;			/* ... and how many were taken */
static struct { uint32_t hz, until_ms; } skip[DMRS_SKIP_MAX];
static volatile uint32_t seq;		/* odd while written (snapshot retries) */
static volatile uint32_t reads;		/* snapshots taken: the screen still looks */
static uint32_t reads_seen, read_ms;
/* The probe of the frequency held now: since when, DMR heard there yet, a call playing at the last look, the end of
 * the last call (the hang runs from it) */
static uint32_t hit_ms, end_ms;
static bool heard_here, was_call, ended;
/* Revisits (as P25 search): every DMRS_REVISIT_STEPS sweep steps one frequency of the found list that lies in the
 * range is read between two steps, round robin, so a new call there is caught in about two seconds instead of a
 * whole pass; the sweep then goes on where it was */
static uint32_t resume_hz;
static uint8_t since_revisit, revisit_i;
static bool revisiting;

static void begin_write(void) { seq++; __asm__ volatile("" ::: "memory"); }
static void end_write(void) { __asm__ volatile("" ::: "memory"); seq++; }

static bool skipped(uint32_t hz, uint32_t now_ms)
{
	for (unsigned i = 0; i < DMRS_SKIP_MAX; i++)
		if (skip[i].hz == hz && (int32_t)(skip[i].until_ms - now_ms) > 0)
			return true;
	return false;
}

static void add_skip(uint32_t hz, uint32_t now_ms)
{
	unsigned pick = 0;
	for (unsigned i = 0; i < DMRS_SKIP_MAX; i++) {
		if (skip[i].hz == hz || (int32_t)(skip[i].until_ms - now_ms) <= 0) {
			pick = i;
			break;
		}
		if ((int32_t)(skip[i].until_ms - skip[pick].until_ms) < 0)
			pick = i;	/* full: replace the one expiring first */
	}
	skip[pick].hz = hz;
	skip[pick].until_ms = now_ms + DMRS_SKIP_MS;
}

/* The next found frequency to read again (round robin), 0 = none in the range */
static uint32_t revisit_pick(uint32_t now_ms)
{
	for (unsigned k = 0; k < s.found_n; k++) {
		revisit_i = (uint8_t)((revisit_i + 1u) % s.found_n);
		uint32_t hz = s.found[revisit_i].hz;
		if (hz >= s.lo_hz && hz <= s.hi_hz && !skipped(hz, now_ms))
			return hz;
	}
	return 0;
}

/* The next step after `hz`, wrapping to the bottom (a full sweep) */
static uint32_t next_step(uint32_t hz)
{
	uint32_t n = hz + s.step_hz;
	if (n > s.hi_hz) {
		n = s.lo_hz;
		s.sweeps++;
	}
	return n;
}

/* Where to go from s.at_hz: back to the sweep after a revisit, else the next step or, now and then, a revisit */
static uint32_t advance(uint32_t now_ms)
{
	if (revisiting) {
		revisiting = false;
		return resume_hz;
	}
	uint32_t n = next_step(s.at_hz);
	if (++since_revisit >= DMRS_REVISIT_STEPS) {
		since_revisit = 0;
		uint32_t r = revisit_pick(now_ms);
		if (r != 0 && r != n) {
			resume_hz = n;
			revisiting = true;
			return r;
		}
	}
	return n;
}

/* A step is tuned now: its readings start after the settle */
static void tuned(uint32_t now_ms)
{
	tuned_ms = now_ms;
	nreads = 0;
}

uint32_t dmrs_start(uint32_t lo_hz, uint32_t hi_hz, uint32_t step_hz, uint32_t now_ms)
{
	if (hi_hz < lo_hz) {
		uint32_t t = lo_hz;
		lo_hz = hi_hz;
		hi_hz = t;
	}
	begin_write();
	s.phase = DMRS_SWEEP;	/* the found list outlives a restart (a new range) */
	s.playing = false;
	s.floor_dbm = -125;
	s.lo_hz = lo_hz;
	s.hi_hz = hi_hz;
	s.step_hz = step_hz ? step_hz : 12500u;
	s.at_hz = lo_hz;
	s.sweeps = s.hits = s.found_dmr = 0;
	end_write();
	floor_q4 = -125 * 16;
	memset(skip, 0, sizeof skip);
	revisiting = false;
	since_revisit = revisit_i = 0;
	resume_hz = 0;
	read_ms = now_ms;
	reads_seen = reads;
	tuned(now_ms);
	return lo_hz;
}

uint32_t dmrs_sweep(int16_t rssi_dbm, uint32_t now_ms, bool *hit)
{
	*hit = false;
	if (s.phase != DMRS_SWEEP)
		return 0;
	if (reads != reads_seen) {
		reads_seen = reads;
		read_ms = now_ms;
	} else if (now_ms - read_ms >= DMRS_IDLE_MS) {
		dmrs_stop();	/* nobody looks: the radio task tunes the channel again */
		return 0;
	}
	if (now_ms - tuned_ms < DMRS_SETTLE_MS)
		return 0;
	if (rssi_dbm == 0)
		rssi_dbm = -140;
	if (nreads == 0 || rssi_dbm > peak)
		peak = rssi_dbm;
	nreads++;
	int16_t floor = (int16_t)(floor_q4 / 16);
	bool strong = peak >= DMRS_HIT_MIN_DBM && peak - floor >= DMRS_HIT_DB;
	if (!strong && nreads < DMRS_READS)
		return 0;	/* a direct mode burst may still come in the other half of the frame */
	begin_write();
	if (!strong) {
		floor_q4 += ((int32_t)peak * 16 - floor_q4) / 8;	/* only quiet steps move the floor */
		s.floor_dbm = (int16_t)(floor_q4 / 16);
	}
	if (strong && !skipped(s.at_hz, now_ms)) {
		s.phase = DMRS_PROBE;
		s.hits++;
		end_write();
		hit_ms = now_ms;
		heard_here = was_call = ended = false;
		*hit = true;
		return 0;
	}
	s.at_hz = advance(now_ms);
	end_write();
	tuned(now_ms);
	return s.at_hz;
}

/* The frequency held now into the found list (to the front), with what was heard there */
static void note(const dmrs_heard_t *h, bool new_call, uint32_t now_ms)
{
	unsigned i = 0;
	while (i < s.found_n && s.found[i].hz != s.at_hz)
		i++;
	dmrs_found_t f;
	if (i < s.found_n) {
		f = s.found[i];
		memmove(&s.found[1], &s.found[0], i * sizeof f);
	} else {
		memset(&f, 0, sizeof f);
		f.cc = DMRS_CC_NONE;
		if (s.found_n < DMRS_FOUND_MAX)
			s.found_n++;
		memmove(&s.found[1], &s.found[0], (size_t)(s.found_n - 1u) * sizeof f);
	}
	f.hz = s.at_hz;
	if (h->cc != DMRS_CC_NONE)
		f.cc = h->cc;
	f.ts_mask |= h->ts_mask;
	f.sync_mask |= h->sync_mask;
	if (h->dst != 0) {
		f.dst = h->dst;
		f.group = h->group;
	}
	if (h->src != 0)
		f.src = h->src;
	if (new_call)
		f.calls++;
	if (h->other)
		f.enc = true;
	else if (h->call)
		f.enc = false;
	f.heard_ms = now_ms;
	s.found[0] = f;
}

/* Back to sweeping: where the sweep was before a revisit, else the next step */
static uint32_t resume(uint32_t now_ms)
{
	s.phase = DMRS_SWEEP;
	s.playing = false;
	if (revisiting) {
		revisiting = false;
		s.at_hz = resume_hz;
	} else {
		s.at_hz = next_step(s.at_hz);
	}
	read_ms = now_ms;
	reads_seen = reads;
	tuned(now_ms);
	return s.at_hz;
}

uint32_t dmrs_probe(const dmrs_heard_t *h, uint32_t now_ms)
{
	if (s.phase != DMRS_PROBE && s.phase != DMRS_CALL)
		return 0;
	bool dmr = h->bursts >= DMRS_MIN_BURSTS || h->call || h->other;
	uint32_t hz = 0;
	begin_write();
	if (dmr) {
		if (!heard_here) {
			heard_here = true;
			s.found_dmr++;
		}
		note(h, h->call && !was_call, now_ms);
	}
	was_call = h->call;
	if (h->call) {
		s.phase = DMRS_CALL;	/* it plays (the call stack hears every talkgroup); held while it lasts */
		s.playing = true;
		ended = false;
	} else {
		s.playing = false;
		bool leave;
		if (s.phase == DMRS_CALL) {
			if (!ended) {
				ended = true;
				end_ms = now_ms;
			}
			leave = now_ms - end_ms >= DMRS_HANG_MS;	/* a reply in the hang plays too */
		} else if (!dmr) {
			leave = now_ms - hit_ms >= DMRS_PROBE_MS;
			if (leave)
				add_skip(s.at_hz, now_ms);	/* a carrier, not DMR (FM, P25, data of another kind) */
		} else {
			leave = now_ms - hit_ms >= DMRS_LATE_MS;	/* DMR, nothing to play: idle, data, encrypted */
		}
		if (leave)
			hz = resume(now_ms);
	}
	end_write();
	return hz;
}

void dmrs_stop(void)
{
	begin_write();
	s.phase = DMRS_OFF;
	s.playing = false;
	end_write();
}

bool dmrs_on(void)
{
	return s.phase != DMRS_OFF;
}

uint8_t dmrs_phase(void)
{
	return s.phase;
}

uint32_t dmrs_at_hz(void)
{
	return s.at_hz;
}

bool dmrs_snapshot(dmrs_view_t *out)
{
	reads++;
	for (unsigned tries = 0; tries < 8u; tries++) {
		uint32_t a = seq;
		__asm__ volatile("" ::: "memory");
		if (a & 1u)
			continue;
		memcpy(out, &s, sizeof *out);
		__asm__ volatile("" ::: "memory");
		if (seq == a)
			return true;
	}
	return false;
}
#endif
