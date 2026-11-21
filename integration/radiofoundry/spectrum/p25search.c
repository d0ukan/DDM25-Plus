/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus P25 search engine (p25search.h). No RTOS, no hardware. */
#include "config.h"
#if CONFIG_P25_SEARCH
#include <string.h>
#include "radio/p25search.h"

static p25s_view_t s;
static uint32_t tuned_ms;
static int32_t floor_q4;		/* floor in dBm x 16 */
static struct { uint32_t hz, until_ms; } skip[P25S_SKIP_MAX];
static volatile uint32_t seq;		/* odd while written (snapshot retries) */
static volatile uint32_t reads;		/* snapshots taken: the screen still looks */
static uint32_t reads_seen, read_ms, epoch;
/* Revisits (2026-11-17, the owner wanted it faster): every P25S_REVISIT_STEPS sweep steps one frequency of the
 * found list that lies in the range is read between two steps, round robin, so a new call on a channel found
 * before is caught in about a second instead of a whole sweep; the sweep then goes on where it was */
static uint32_t resume_hz;
static uint8_t since_revisit, revisit_i;
static bool revisiting;
/* The found list's key (p25search.h): found[0] is the entry of the probe held now (held), a call counted there and when */
static bool held, credited;
static uint32_t credit_ms;
#if CONFIG_SEARCH_FAST
/* The faster sweep (P25S_QUICK_MS): the step's stage, the step before it when only its quick reading was taken, and
 * the step a look back came from */
enum { ST_QUICK, ST_CONFIRM, ST_BACK };
static uint8_t stage;
static uint32_t prev_hz, back_from;
#endif

static void begin_write(void) { seq++; __asm__ volatile("" ::: "memory"); }
static void end_write(void) { __asm__ volatile("" ::: "memory"); seq++; }

static bool skipped(uint32_t hz, uint32_t now_ms)
{
	for (unsigned i = 0; i < P25S_SKIP_MAX; i++)
		if (skip[i].hz == hz && (int32_t)(skip[i].until_ms - now_ms) > 0)
			return true;
	return false;
}

static void add_skip(uint32_t hz, uint32_t now_ms)
{
#if CONFIG_SEARCH_FAST
	/* Its own entry (running or run out: a repeat gets the long skip), else one run out, else the one ending first */
	unsigned pick = P25S_SKIP_MAX;
	for (unsigned i = 0; i < P25S_SKIP_MAX && pick == P25S_SKIP_MAX; i++)
		if (skip[i].hz == hz)
			pick = i;
	bool again = pick < P25S_SKIP_MAX && (int32_t)(now_ms - skip[pick].until_ms) < (int32_t)P25S_AGAIN_MS;
	for (unsigned i = 0; i < P25S_SKIP_MAX && pick == P25S_SKIP_MAX; i++)
		if ((int32_t)(skip[i].until_ms - now_ms) <= 0)
			pick = i;
	if (pick == P25S_SKIP_MAX) {
		pick = 0;
		for (unsigned i = 1; i < P25S_SKIP_MAX; i++)
			if ((int32_t)(skip[i].until_ms - skip[pick].until_ms) < 0)
				pick = i;
	}
	skip[pick].hz = hz;
	skip[pick].until_ms = now_ms + (again ? P25S_SKIP_LONG_MS : P25S_SKIP_MS);
}
#else
	unsigned pick = 0;
	for (unsigned i = 0; i < P25S_SKIP_MAX; i++) {
		if (skip[i].hz == hz || (int32_t)(skip[i].until_ms - now_ms) <= 0) {
			pick = i;
			break;
		}
		if ((int32_t)(skip[i].until_ms - skip[pick].until_ms) < 0)
			pick = i;	/* full: replace the one expiring first */
	}
	skip[pick].hz = hz;
	skip[pick].until_ms = now_ms + P25S_SKIP_MS;
}
#endif

/* The next found frequency to read again (round robin), 0 = none in the range */
static uint32_t revisit_pick(uint32_t now_ms)
{
	for (unsigned k = 0; k < s.found_n; k++) {
		revisit_i = (uint8_t)((revisit_i + 1u) % s.found_n);
		uint32_t hz = s.found[revisit_i].hz;
		bool again = false;	/* a frequency with more entries (NACs, talkgroups) is read once a round */
		for (unsigned j = 0; j < revisit_i && !again; j++)
			again = s.found[j].hz == hz;
		if (!again && hz >= s.lo_hz && hz <= s.hi_hz && !skipped(hz, now_ms))
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
	if (++since_revisit >= P25S_REVISIT_STEPS) {
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

uint32_t p25s_start(uint32_t lo_hz, uint32_t hi_hz, uint32_t step_hz, uint32_t now_ms)
{
	if (hi_hz < lo_hz) {
		uint32_t t = lo_hz;
		lo_hz = hi_hz;
		hi_hz = t;
	}
	begin_write();
	uint8_t keep_n = s.found_n;
	p25s_found_t keep[P25S_FOUND_MAX];
	memcpy(keep, s.found, sizeof keep);
	memset(&s, 0, sizeof s);
	memcpy(s.found, keep, sizeof keep);	/* the found list outlives a restart (a new range) */
	s.found_n = keep_n;
	s.lo_hz = lo_hz;
	s.hi_hz = hi_hz;
	s.step_hz = step_hz ? step_hz : 12500u;
	s.at_hz = lo_hz;
	s.phase = P25S_SWEEP;
	s.floor_dbm = -125;
	end_write();
	floor_q4 = -125 * 16;
	memset(skip, 0, sizeof skip);
	revisiting = false;
	since_revisit = revisit_i = 0;
	resume_hz = 0;
	held = false;
#if CONFIG_SEARCH_FAST
	stage = ST_QUICK;
	prev_hz = 0;
#endif
	tuned_ms = read_ms = now_ms;
	reads_seen = reads;
	epoch++;
	return lo_hz;
}

uint32_t p25s_sweep(int16_t rssi_dbm, uint32_t now_ms, bool *hit)
{
	*hit = false;
	if (s.phase != P25S_SWEEP)
		return 0;
	if (reads != reads_seen) {
		reads_seen = reads;
		read_ms = now_ms;
	} else if (now_ms - read_ms >= P25S_IDLE_MS) {
		p25s_stop();	/* nobody looks: the radio task tunes the channel again */
		return 0;
	}
#if CONFIG_SEARCH_FAST
	if (now_ms - tuned_ms < (stage == ST_QUICK ? P25S_QUICK_MS : P25S_SETTLE_MS))
		return 0;
#else
	if (now_ms - tuned_ms < P25S_SETTLE_MS)
		return 0;
#endif
	if (rssi_dbm == 0)
		rssi_dbm = -140;
	int16_t floor = (int16_t)(floor_q4 / 16);
	bool strong = rssi_dbm >= P25S_HIT_MIN_DBM && rssi_dbm - floor >= P25S_HIT_DB;
#if CONFIG_SEARCH_FAST
	if (strong && stage == ST_QUICK) {
		stage = ST_CONFIRM;	/* read again settled, on the same step (no retune) */
		return 0;
	}
#endif
	begin_write();
	if (!strong) {
		floor_q4 += ((int32_t)rssi_dbm * 16 - floor_q4) / 8;	/* only quiet steps move the floor */
		s.floor_dbm = (int16_t)(floor_q4 / 16);
	}
	if (strong && !skipped(s.at_hz, now_ms)) {
		s.phase = P25S_PROBE;
		s.hits++;
		held = false;	/* a new probe: no entry of it yet */
#if CONFIG_SEARCH_FAST
		stage = ST_QUICK;
		prev_hz = 0;
#endif
		end_write();
		*hit = true;
		return 0;
	}
#if CONFIG_SEARCH_FAST
	if (!strong && stage == ST_CONFIRM && prev_hz != 0) {
		/* Strong at once, quiet settled: the step before, read quickly, may be where the signal is */
		back_from = s.at_hz;
		s.at_hz = prev_hz;
		prev_hz = 0;
		stage = ST_BACK;
		end_write();
		tuned_ms = now_ms;
		return s.at_hz;
	}
	if (stage == ST_BACK)
		s.at_hz = back_from;	/* nothing there either: on from where the look back began */
	prev_hz = stage == ST_QUICK ? s.at_hz : 0u;
	stage = ST_QUICK;
#endif
	s.at_hz = advance(now_ms);
	end_write();
	tuned_ms = now_ms;
	return s.at_hz;
}

/* The found list's key (p25search.h): frequency + NAC + talkgroup. An entry fits a report when the frequency is the
 * same and the NAC and talkgroup are the same or not known on one side; the same key: all three equal */
static bool fits(const p25s_found_t *f, uint16_t nac, uint16_t tg)
{
	return f->hz == s.at_hz && (f->nac == nac || f->nac == P25S_NAC_NONE || nac == P25S_NAC_NONE) &&
	       (f->tg == tg || f->tg == 0 || tg == 0);
}

/* Entry i to the top (newest first) */
static void to_front(unsigned i)
{
	p25s_found_t f = s.found[i];
	memmove(&s.found[1], &s.found[0], i * sizeof f);
	s.found[0] = f;
}

/* The entry a report goes to, moved to the top: the newest that fits (two entries of a frequency always differ in a NAC
 * or talkgroup both know, so a report knowing both fits one at most), else a new one on top (the oldest drops when the
 * list is full; none without a NAC). False: none */
static bool pick(uint16_t nac, uint16_t tg, bool *fresh)
{
	unsigned i = 0;
	while (i < s.found_n && !fits(&s.found[i], nac, tg))
		i++;
	if (i < s.found_n) {
		to_front(i);
		*fresh = false;
		return true;
	}
	if (nac == P25S_NAC_NONE)
		return false;
	if (s.found_n < P25S_FOUND_MAX)
		s.found_n++;
	memmove(&s.found[1], &s.found[0], (size_t)(s.found_n - 1u) * sizeof s.found[0]);
	memset(&s.found[0], 0, sizeof s.found[0]);
	s.found[0].hz = s.at_hz;
	s.found[0].nac = P25S_NAC_NONE;
	*fresh = true;
	return true;
}

void p25s_probe_found(uint16_t nac, uint16_t tg, uint32_t src, uint32_t now_ms)
{
	if (s.phase != P25S_PROBE)
		return;
	if (nac > 0xFFFu)
		nac = P25S_NAC_NONE;	/* no NAC known (the receiver's "any" after a terminator or a damaged frame) */
	begin_write();
	if (!s.playing)
		s.probes_p25++;
	s.playing = true;
	bool fresh = false, call;
	if (held && fits(&s.found[0], nac, tg)) {
		call = now_ms - s.found[0].heard_ms > P25S_CALL_GAP_MS;	/* the probe's entry (on top) */
	} else {
		/* Not held yet, or the report names another NAC / talkgroup than the held entry's: that key's entry. A call
		 * just counted on the held entry was this one's (its link control came late) */
		bool moved = held && credited && now_ms - credit_ms < P25S_MOVE_MS;
		if (!pick(nac, tg, &fresh)) {
			end_write();
			return;
		}
		if (moved && s.found[1].calls)
			s.found[1].calls--;	/* the held entry, one down now */
		held = true;
		credited = false;
		call = fresh || moved || s.found[0].heard_ms == 0 || now_ms - s.found[0].heard_ms > P25S_CALL_GAP_MS;
	}
	p25s_found_t *f = &s.found[0];
	if (f->nac == P25S_NAC_NONE)
		f->nac = nac;
	if (f->tg == 0)
		f->tg = tg;
	if (src)
		f->src = src;
	if (call) {
		f->calls++;
		credited = true;
		credit_ms = now_ms;
	}
	f->heard_ms = now_ms;
	end_write();
}

uint32_t p25s_probe_done(bool was_p25, uint32_t now_ms)
{
	if (s.phase != P25S_PROBE)
		return 0;
	if (!was_p25)
		add_skip(s.at_hz, now_ms);
	begin_write();
	s.playing = false;
	s.phase = P25S_SWEEP;
	held = false;
	if (revisiting) {
		revisiting = false;	/* a revisit's probe: the sweep goes on where it was */
		s.at_hz = resume_hz;
	} else {
		s.at_hz = next_step(s.at_hz);
	}
	end_write();
#if CONFIG_SEARCH_FAST
	stage = ST_QUICK;
	prev_hz = 0;	/* the last reading was the probe's */
#endif
	tuned_ms = read_ms = now_ms;
	reads_seen = reads;
	epoch++;
	return s.at_hz;
}

void p25s_retuned(uint32_t now_ms)
{
	tuned_ms = now_ms;
#if CONFIG_SEARCH_FAST
	if (stage == ST_CONFIRM)
		stage = ST_QUICK;	/* read anew from the start */
#endif
}

uint32_t p25s_epoch(void)
{
	return epoch;
}

void p25s_stop(void)
{
	begin_write();
	s.phase = P25S_OFF;
	s.playing = false;
	end_write();
}

bool p25s_on(void)
{
	return s.phase != P25S_OFF;
}

uint8_t p25s_phase(void)
{
	return s.phase;
}

uint32_t p25s_at_hz(void)
{
	return s.at_hz;
}

/* The PC's search log (debug link section, tools/plus_searchlog.py): the state and found entry i, read like the
 * snapshot but not counted as a look, so the search still stops once its screen was left. Small on the caller's stack
 * (the host link task): one entry at a time. False when i is past the list or the copy kept changing */
bool p25s_peek(unsigned i, p25s_peek_t *out)
{
	for (unsigned tries = 0; tries < 8u; tries++) {
		uint32_t a = seq;
		__asm__ volatile("" ::: "memory");
		if (a & 1u)
			continue;
		out->phase = s.phase;
		out->at_hz = s.at_hz;
		out->lo_hz = s.lo_hz;
		out->hi_hz = s.hi_hz;
		out->found_n = s.found_n;
		bool have = i < s.found_n && i < P25S_FOUND_MAX;
		if (have)
			memcpy(&out->f, &s.found[i], sizeof out->f);
		__asm__ volatile("" ::: "memory");
		if (seq == a)
			return have;
	}
	return false;
}

bool p25s_snapshot(p25s_view_t *out)
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
