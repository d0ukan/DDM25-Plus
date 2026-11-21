/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus DMR search engine (dmrsearch.h). No RTOS, no hardware. A sibling of p25search.c (the same sweep, skip
 * list and revisits) kept separate so that either search can be built without the other. With CONFIG_ALL_SEARCH it is
 * the all digital search's engine too (a second kind: the probe goes through the modes in priority order). */
#include "config.h"
#if CONFIG_DMR_SEARCH
#include <string.h>
#include "radio/dmrsearch.h"

static dmrs_view_t s;
#if CONFIG_ALL_SEARCH
/* The modes a hit is probed in, the owner's priority order (the first confirmed wins); those not built left out */
static const uint8_t order[] = { DMRS_MODE_P25, DMRS_MODE_DMR,
#if CONFIG_YSF
				 DMRS_MODE_YSF,
#endif
#if CONFIG_NXDN
				 DMRS_MODE_NXDN,
#endif
};
static uint8_t tried;		/* 1 << mode of the modes probed on the frequency held now */
static uint8_t epoch;		/* dmrs_regained */
#endif
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
/* The found list's key (note): found[0] is the entry of the probe held now (held), a call counted there and when */
static bool held, credited;
static uint32_t credit_ms;
#if CONFIG_SEARCH_FAST
/* The faster sweep (DMRS_QUICK_MS): the step's stage, a strong first reading, the step before (read the quick way) and
 * the step a look back came from */
enum { ST_QUICK, ST_MORE, ST_BACK };
static uint8_t stage;
static bool maybe, peaked;
static uint32_t prev_hz, back_from;
#endif

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
#if CONFIG_SEARCH_FAST
	/* Its own entry (running or run out: a repeat gets the long skip), else one run out, else the one ending first */
	unsigned pick = DMRS_SKIP_MAX;
	for (unsigned i = 0; i < DMRS_SKIP_MAX && pick == DMRS_SKIP_MAX; i++)
		if (skip[i].hz == hz)
			pick = i;
	bool again = pick < DMRS_SKIP_MAX && (int32_t)(now_ms - skip[pick].until_ms) < (int32_t)DMRS_AGAIN_MS;
	for (unsigned i = 0; i < DMRS_SKIP_MAX && pick == DMRS_SKIP_MAX; i++)
		if ((int32_t)(skip[i].until_ms - now_ms) <= 0)
			pick = i;
	if (pick == DMRS_SKIP_MAX) {
		pick = 0;
		for (unsigned i = 1; i < DMRS_SKIP_MAX; i++)
			if ((int32_t)(skip[i].until_ms - skip[pick].until_ms) < 0)
				pick = i;
	}
	skip[pick].hz = hz;
	skip[pick].until_ms = now_ms + (again ? DMRS_SKIP_LONG_MS : DMRS_SKIP_MS);
}
#else
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
#endif

/* The next found frequency to read again (round robin), 0 = none in the range */
static uint32_t revisit_pick(uint32_t now_ms)
{
	for (unsigned k = 0; k < s.found_n; k++) {
		revisit_i = (uint8_t)((revisit_i + 1u) % s.found_n);
		uint32_t hz = s.found[revisit_i].hz;
		bool again = false;	/* a frequency with more entries (modes, codes, groups) is read once a round */
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
#if CONFIG_SEARCH_FAST
	maybe = peaked = false;
#endif
}

static uint32_t start(uint8_t kind, uint32_t lo_hz, uint32_t hi_hz, uint32_t step_hz, uint32_t now_ms)
{
	if (hi_hz < lo_hz) {
		uint32_t t = lo_hz;
		lo_hz = hi_hz;
		hi_hz = t;
	}
	begin_write();
	if (s.kind != kind)
		s.found_n = 0;	/* the other kind's finds: not this search's */
	s.kind = kind;
	s.mode = DMRS_MODE_DMR;
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
	held = false;
#if CONFIG_SEARCH_FAST
	stage = ST_QUICK;
	prev_hz = 0;
#endif
	read_ms = now_ms;
	reads_seen = reads;
	tuned(now_ms);
	return lo_hz;
}

uint32_t dmrs_start(uint32_t lo_hz, uint32_t hi_hz, uint32_t step_hz, uint32_t now_ms)
{
	return start(DMRS_KIND_DMR, lo_hz, hi_hz, step_hz, now_ms);
}

#if CONFIG_ALL_SEARCH
uint32_t dmrs_start_all(uint32_t lo_hz, uint32_t hi_hz, uint32_t step_hz, uint32_t now_ms)
{
	return start(DMRS_KIND_ALL, lo_hz, hi_hz, step_hz, now_ms);
}

/* The next mode to probe on the frequency held now (DMRS_MODES: none left); it counts as tried */
static uint8_t next_mode(void)
{
	for (unsigned i = 0; i < sizeof order; i++) {
		if (!(tried & (1u << order[i]))) {
			tried |= (uint8_t)(1u << order[i]);
			return order[i];
		}
	}
	return DMRS_MODES;
}
#endif

/* A hit's first mode: the DMR search's is DMR; the all digital search tries the mode a find there had first (a
 * revisit), else the priority order */
static uint8_t first_mode(void)
{
#if CONFIG_ALL_SEARCH
	tried = 0;
	if (s.kind != DMRS_KIND_ALL)
		return DMRS_MODE_DMR;
	for (unsigned i = 0; i < s.found_n; i++) {
		if (s.found[i].hz == s.at_hz) {
			tried = (uint8_t)(1u << s.found[i].mode);
			return s.found[i].mode;
		}
	}
	return next_mode();
#else
	return DMRS_MODE_DMR;
#endif
}

/* How long the probe of the mode held now waits for it */
static uint32_t window_ms(void)
{
#if CONFIG_ALL_SEARCH
	if (s.mode == DMRS_MODE_P25)
		return DMRS_P25_PROBE_MS;
	if (s.mode == DMRS_MODE_YSF)
		return DMRS_YSF_PROBE_MS;
	if (s.mode == DMRS_MODE_NXDN)
		return DMRS_NXDN_PROBE_MS;
#endif
	return DMRS_PROBE_MS;
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
#if CONFIG_SEARCH_FAST
	if (now_ms - tuned_ms < (stage == ST_BACK ? DMRS_SETTLE_MS : DMRS_QUICK_MS))
		return 0;
#else
	if (now_ms - tuned_ms < DMRS_SETTLE_MS)
		return 0;
#endif
	if (rssi_dbm == 0)
		rssi_dbm = -140;
	int16_t floor = (int16_t)(floor_q4 / 16);
#if CONFIG_SEARCH_FAST
	if (now_ms - tuned_ms < DMRS_SETTLE_MS) {
		/* The quick reading: it only marks the step for more readings, it never makes a hit by itself. Half the hit's
		 * margin: a burst that began just before it reads only part of its level yet */
		maybe = rssi_dbm >= DMRS_HIT_MIN_DBM - DMRS_HIT_DB / 2 && rssi_dbm - floor >= DMRS_HIT_DB / 2;
		nreads++;
		return 0;
	}
	if (!peaked || rssi_dbm > peak)
		peak = rssi_dbm;	/* the settled readings' peak */
	peaked = true;
#else
	if (nreads == 0 || rssi_dbm > peak)
		peak = rssi_dbm;
#endif
	nreads++;
	bool strong = peak >= DMRS_HIT_MIN_DBM && peak - floor >= DMRS_HIT_DB;
#if CONFIG_SEARCH_FAST
	if (!strong && nreads < DMRS_READS + (stage == ST_MORE ? DMRS_MORE_READS : 0u))
		return 0;	/* a direct mode burst may still come in the other half of the frame */
	if (!strong && maybe && stage == ST_QUICK) {
		stage = ST_MORE;	/* only the quick reading was strong: more settled readings */
		return 0;
	}
	if (!strong && stage == ST_MORE && prev_hz != 0) {
		/* Nothing settled here: the step before, read the quick way, may be where the signal is */
		begin_write();
		back_from = s.at_hz;
		s.at_hz = prev_hz;
		prev_hz = 0;
		stage = ST_BACK;
		end_write();
		tuned(now_ms);
		return s.at_hz;
	}
#else
	if (!strong && nreads < DMRS_READS)
		return 0;	/* a direct mode burst may still come in the other half of the frame */
#endif
	begin_write();
	if (!strong) {
		floor_q4 += ((int32_t)peak * 16 - floor_q4) / 8;	/* only quiet steps move the floor */
		s.floor_dbm = (int16_t)(floor_q4 / 16);
	}
	if (strong && !skipped(s.at_hz, now_ms)) {
		s.phase = DMRS_PROBE;
		s.mode = first_mode();
		s.hits++;
		held = false;	/* a new probe: no entry of it yet */
#if CONFIG_SEARCH_FAST
		stage = ST_QUICK;
		prev_hz = 0;
#endif
		end_write();
		hit_ms = now_ms;
		heard_here = was_call = ended = false;
		*hit = true;
		return 0;
	}
#if CONFIG_SEARCH_FAST
	if (stage == ST_BACK)
		s.at_hz = back_from;	/* nothing there either: on from where the look back began */
	prev_hz = stage != ST_BACK && !strong ? s.at_hz : 0u;	/* a strong (skipped) step is known: no look back to it */
	stage = ST_QUICK;
#endif
	s.at_hz = advance(now_ms);
	end_write();
	tuned(now_ms);
	return s.at_hz;
}

/* The found list's key (dmrsearch.h): frequency + mode + code (P25 NAC, DMR color code, YSF DG-ID, NXDN RAN) + group
 * (the talkgroup or called ID; YSF none: its dst / src hold the callsign). The group of an entry or a report */
static uint32_t group_of(uint8_t mode, uint32_t dst)
{
	return mode == DMRS_MODE_YSF ? 0u : dst;
}

/* An entry fits a report when frequency and mode are the same and code and group the same or not known on one side */
static bool fits(const dmrs_found_t *f, uint16_t cc, uint32_t grp)
{
	uint32_t g = group_of(f->mode, f->dst);
	return f->hz == s.at_hz && f->mode == s.mode && (f->cc == cc || f->cc == DMRS_CC_NONE || cc == DMRS_CC_NONE) &&
	       (g == grp || g == 0 || grp == 0);
}

/* The entry a report goes to, moved to the top: the newest that fits (two entries of a frequency and mode always differ
 * in a code or group both know, so a report knowing both fits one at most), else a new one on top (the oldest drops
 * when the list is full). True when it is new */
static bool pick(uint16_t cc, uint32_t grp)
{
	unsigned i = 0;
	while (i < s.found_n && !fits(&s.found[i], cc, grp))
		i++;
	dmrs_found_t f;
	bool fresh = i >= s.found_n;
	if (!fresh) {
		f = s.found[i];
	} else {
		memset(&f, 0, sizeof f);
		f.hz = s.at_hz;
		f.cc = DMRS_CC_NONE;
		f.mode = s.mode;
		if (s.found_n < DMRS_FOUND_MAX)
			s.found_n++;
		i = s.found_n - 1u;	/* the oldest drops when full */
	}
	memmove(&s.found[1], &s.found[0], i * sizeof f);
	s.found[0] = f;
	return fresh;
}

/* The frequency held now into the found list, with what was heard there (owner 2026-11-21: ONE entry per frequency +
 * mode + code + group with a call counter, the newest on top; a mixed mode repeater has an entry for each mode, a
 * frequency with two talkgroups one for each). A report that doesn't know the code or the group yet updates the entry
 * this probe holds (found[0]), or the newest that fits; a call counted on the held entry whose group then turns out
 * another within DMRS_MOVE_MS (its link control came late) moves to the right entry */
static void note(const dmrs_heard_t *h, bool new_call, uint32_t now_ms)
{
	uint32_t grp = group_of(s.mode, h->dst);
	if (!held || !fits(&s.found[0], h->cc, grp)) {
		bool moved = held && credited && now_ms - credit_ms < DMRS_MOVE_MS;
		bool fresh = pick(h->cc, grp);
		if (moved && s.found[1].calls)
			s.found[1].calls--;	/* the held entry, one down now */
		/* the call moves with its key; a key change in a call playing is a call there too */
		if (moved || (held && h->call && !new_call && (fresh || now_ms - s.found[0].heard_ms > DMRS_HANG_MS)))
			new_call = true;
		held = true;
		credited = false;
	}
	dmrs_found_t *f = &s.found[0];
	if (h->cc != DMRS_CC_NONE)
		f->cc = h->cc;
	f->ts_mask |= h->ts_mask;
	f->sync_mask |= h->sync_mask;
#if CONFIG_ALL_SEARCH
	if (s.mode == DMRS_MODE_YSF) {
		if (h->src != 0) {	/* a callsign: both halves are its (a shorter one leaves no tail of the last) */
			f->src = h->src;
			f->dst = h->dst;
		}
	} else
#endif
	{
		if (h->dst != 0) {
			f->dst = h->dst;
			f->group = h->group;
		}
		if (h->src != 0)
			f->src = h->src;
	}
	if (new_call) {
		f->calls++;
		credited = true;
		credit_ms = now_ms;
	}
	if (h->other)
		f->enc = true;
	else if (h->call)
		f->enc = false;
	f->heard_ms = now_ms;
}

/* Back to sweeping: where the sweep was before a revisit, else the next step */
static uint32_t resume(uint32_t now_ms)
{
	s.phase = DMRS_SWEEP;
	s.playing = false;
	held = false;
#if CONFIG_SEARCH_FAST
	stage = ST_QUICK;
	prev_hz = 0;	/* the last readings were the probe's */
#endif
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
		} else if (!heard_here) {
			leave = now_ms - hit_ms >= window_ms();
#if CONFIG_ALL_SEARCH
			if (leave && s.kind == DMRS_KIND_ALL) {
				uint8_t m = next_mode();
				if (m != DMRS_MODES) {
					s.mode = m;	/* not this mode: the next one, on the same frequency */
					held = false;
					hit_ms = now_ms;
					was_call = ended = false;
					end_write();
					return s.at_hz;	/* the receiver is set up for it */
				}
			}
#endif
			if (leave)
				add_skip(s.at_hz, now_ms);	/* a carrier, not DMR (FM, P25, data of another kind); the all
								 * digital search: none of its modes (FM, a beacon, data) */
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

/* The PC's search log (debug link section, tools/plus_searchlog.py): the state and found entry i, read like the
 * snapshot but not counted as a look, so the search still stops once its screen was left. Small on the caller's stack
 * (the host link task): one entry at a time. False when i is past the list or the copy kept changing. Another kind's
 * state reads as stopped with nothing found */
static bool peek(uint8_t kind, unsigned i, dmrs_peek_t *out)
{
	for (unsigned tries = 0; tries < 8u; tries++) {
		uint32_t a = seq;
		__asm__ volatile("" ::: "memory");
		if (a & 1u)
			continue;
		bool mine = s.kind == kind;
		out->phase = mine ? s.phase : (uint8_t)DMRS_OFF;
		out->mode = s.mode;
		out->at_hz = s.at_hz;
		out->lo_hz = s.lo_hz;
		out->hi_hz = s.hi_hz;
		out->found_n = mine ? s.found_n : 0u;
		bool have = i < out->found_n && i < DMRS_FOUND_MAX;
		if (have)
			memcpy(&out->f, &s.found[i], sizeof out->f);
		__asm__ volatile("" ::: "memory");
		if (seq == a)
			return have;
	}
	return false;
}

bool dmrs_peek(unsigned i, dmrs_peek_t *out)
{
	return peek(DMRS_KIND_DMR, i, out);
}

/* A consistent copy; a look only when it is the kind running (another kind's screen keeps nothing alive) */
static bool snapshot(uint8_t kind, dmrs_view_t *out)
{
	if (s.kind == kind)
		reads++;
	for (unsigned tries = 0; tries < 8u; tries++) {
		uint32_t a = seq;
		__asm__ volatile("" ::: "memory");
		if (a & 1u)
			continue;
		memcpy(out, &s, sizeof *out);
		__asm__ volatile("" ::: "memory");
		if (seq == a) {
			if (out->kind != kind) {
				out->phase = DMRS_OFF;	/* the other kind's: stopped, nothing found */
				out->playing = false;
				out->found_n = 0;
				out->kind = kind;
			}
			return true;
		}
	}
	return false;
}

bool dmrs_snapshot(dmrs_view_t *out)
{
	return snapshot(DMRS_KIND_DMR, out);
}

#if CONFIG_ALL_SEARCH
bool dmrs_snapshot_all(dmrs_view_t *out)
{
	return snapshot(DMRS_KIND_ALL, out);
}

bool dmrs_peek_all(unsigned i, dmrs_peek_t *out)
{
	return peek(DMRS_KIND_ALL, i, out);
}

uint8_t dmrs_kind(void)
{
	return s.kind;
}

uint8_t dmrs_mode(void)
{
	return s.mode;
}

void dmrs_retuned(uint32_t now_ms)
{
	if (s.phase == DMRS_SWEEP) {
		tuned(now_ms);	/* the step settles anew, read from its start */
#if CONFIG_SEARCH_FAST
		stage = ST_QUICK;
		prev_hz = 0;
#endif
	} else {
		hit_ms = now_ms;	/* the probe's window (or the late wait) from now */
	}
}

void dmrs_regained(void)
{
	epoch++;
}

uint8_t dmrs_epoch(void)
{
	return epoch;
}

uint32_t dmrs_native_hz(bool *ysf)
{
	bool probing = s.kind == DMRS_KIND_ALL && (s.phase == DMRS_PROBE || s.phase == DMRS_CALL);
	*ysf = s.mode == DMRS_MODE_YSF;
	return probing && (s.mode == DMRS_MODE_P25 || *ysf) ? s.at_hz : 0u;
}

uint32_t dmrs_nxdn_hz(void)
{
	bool probing = s.kind == DMRS_KIND_ALL && (s.phase == DMRS_PROBE || s.phase == DMRS_CALL);
	return probing && s.mode == DMRS_MODE_NXDN ? s.at_hz : 0u;
}

/* Base 40: space, A-Z, 0-9, '/', '-', '.' */
static const char call_set[41] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/-.";

static uint32_t call_part(const char *call, unsigned from, unsigned n, bool *done)
{
	uint32_t v = 0;
	for (unsigned k = 0; k < n; k++) {
		char c = *done ? '\0' : call[from + k];
		if (c == '\0')
			*done = true;
		if (c >= 'a' && c <= 'z')
			c = (char)(c - 'a' + 'A');
		unsigned d = 39;	/* '.': a character the set lacks */
		if (*done)
			d = 0;
		else
			for (unsigned j = 0; j < 40u; j++)
				if (call_set[j] == c)
					d = j;
		v = v * 40u + d;
	}
	return v;
}

void dmrs_call_pack(const char *call, uint32_t *hi, uint32_t *lo)
{
	bool done = call == NULL;
	*lo = call_part(call, 0, 6, &done);
	*hi = call_part(call, 6, 4, &done);
}

void dmrs_call_unpack(uint32_t hi, uint32_t lo, char out[11])
{
	for (int k = 5; k >= 0; k--) {
		out[k] = call_set[lo % 40u];
		lo /= 40u;
	}
	for (int k = 9; k >= 6; k--) {
		out[k] = call_set[hi % 40u];
		hi /= 40u;
	}
	int n = 10;
	while (n > 0 && out[n - 1] == ' ')
		n--;
	out[n] = '\0';
}

const char *dmrs_mode_name(uint8_t mode)
{
	static const char names[DMRS_MODES][5] = { "DMR", "P25", "YSF", "NXDN" };
	return mode < DMRS_MODES ? names[mode] : "?";
}
#endif
#endif
