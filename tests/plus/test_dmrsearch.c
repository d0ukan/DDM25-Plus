/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus DMR search engine (dmrsearch.c): stepping and wrap, settle, the readings a step takes (a direct mode
 * burst caught whichever half of the frame it falls in), the floor and hits, the probe outcomes (DMR -> found list
 * with color code, slots, repeater / mobile and the call's talkgroup and source; nothing -> skipped for a while),
 * a call played then the hang and the resume, the found list order and limits, revisits, the idle stop. */
#include <stdio.h>
#include <string.h>
#include "radio/dmrsearch.h"

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define POLL_MS 25u	/* RADIO_POLL_MS */

static uint32_t now = 1000;

/* Sweep (one reading a poll) until a hit or `max` polls; `level(hz)` is the reading. Returns the hit frequency or 0 */
static uint32_t run(int16_t (*level)(uint32_t), unsigned max)
{
	for (unsigned i = 0; i < max; i++) {
		bool hit;
		dmrs_view_t look;
		dmrs_snapshot(&look);	/* the screen looks */
		now += POLL_MS;
		dmrs_sweep(level(dmrs_at_hz()), now, &hit);
		if (hit)
			return dmrs_at_hz();
	}
	return 0;
}

static int16_t quiet(uint32_t hz) { (void)hz; return -124; }
static int16_t one(uint32_t hz) { return hz == 433500000u ? -80 : -124; }
static int16_t two(uint32_t hz) { return hz == 433500000u || hz == 433025000u ? -90 : -123; }
static int16_t weak(uint32_t hz) { return hz == 433500000u ? -118 : -124; }
/* A direct mode transmitter: on air 30 ms in every 60 */
static int16_t dmo(uint32_t hz) { return hz == 433500000u && (now / 30u) % 2u == 0 ? -85 : -124; }

/* Hold the probe for `ms` with what the receiver heard; returns the first retune asked for (0 = none) */
static uint32_t hold(const dmrs_heard_t *h, uint32_t ms)
{
	for (uint32_t t = 0; t < ms; t += POLL_MS) {
		dmrs_view_t look;
		dmrs_snapshot(&look);
		now += POLL_MS;
		uint32_t hz = dmrs_probe(h, now);
		if (hz != 0)
			return hz;
	}
	return 0;
}

int main(void)
{
	dmrs_view_t v;
	dmrs_heard_t h;
	bool hit;
	CHECK(!dmrs_on());
	/* 433.000 .. 434.000 in 12.5 kHz: 81 steps */
	CHECK(dmrs_start(433000000u, 434000000u, 12500u, now) == 433000000u && dmrs_on() && dmrs_phase() == DMRS_SWEEP);
	CHECK(dmrs_sweep(-124, now + DMRS_SETTLE_MS - 1u, &hit) == 0 && !hit);	/* settling */
	/* a quiet step takes DMRS_READS readings, then the next */
	uint32_t t0 = now;
	CHECK(dmrs_sweep(-124, now + DMRS_SETTLE_MS, &hit) == 0 && dmrs_sweep(-124, now + DMRS_SETTLE_MS + 25u, &hit) == 0);
	CHECK(dmrs_sweep(-124, now + DMRS_SETTLE_MS + 50u, &hit) == 433012500u && !hit);
	now = t0 + DMRS_SETTLE_MS + 50u;
	CHECK(run(quiet, 81u * 4u * 2u) == 0);
	CHECK(dmrs_snapshot(&v) && v.sweeps >= 1 && v.hits == 0 && v.floor_dbm >= -126 && v.floor_dbm <= -122);
	/* a pass of 81 steps: about 100 ms a step */
	uint32_t s0 = v.sweeps;
	t0 = now;
	while (dmrs_snapshot(&v) && v.sweeps == s0)
		dmrs_sweep(-124, now += POLL_MS, &hit);
	s0 = v.sweeps;
	t0 = now;
	while (dmrs_snapshot(&v) && v.sweeps == s0)
		dmrs_sweep(-124, now += POLL_MS, &hit);
	CHECK(now - t0 >= 81u * 75u && now - t0 <= 81u * 110u);

	/* A carrier at 433.500 with DMR: a hit, the probe sees bursts, the found list gets it */
	CHECK(run(one, 81u * 8u) == 433500000u && dmrs_phase() == DMRS_PROBE);
	CHECK(dmrs_sweep(-80, now + 1000u, &hit) == 0 && !hit);	/* probing: the sweep waits */
	memset(&h, 0, sizeof h);
	h.cc = DMRS_CC_NONE;
	CHECK(hold(&h, 200u) == 0);			/* nothing heard yet: held */
	h.bursts = 1;
	h.cc = 1;
	CHECK(hold(&h, 50u) == 0 && dmrs_snapshot(&v) && v.found_n == 0);	/* one burst is not enough */
	h.bursts = 6;
	h.ts_mask = 2;
	h.sync_mask = DMRS_SYNC_BS;
	CHECK(hold(&h, 50u) == 0);
	CHECK(dmrs_snapshot(&v) && v.found_n == 1 && v.found[0].hz == 433500000u && v.found[0].cc == 1 &&
	      v.found[0].ts_mask == 2 && v.found[0].sync_mask == DMRS_SYNC_BS && v.found[0].calls == 0 && !v.playing &&
	      v.found_dmr == 1);
	/* A voice call plays: held while it lasts, its talkgroup and source noted */
	h.call = true;
	h.group = true;
	h.dst = 2620;
	h.src = 2620123;
	CHECK(hold(&h, 5000u) == 0 && dmrs_phase() == DMRS_CALL);
	CHECK(dmrs_snapshot(&v) && v.playing && v.found[0].dst == 2620 && v.found[0].group && v.found[0].src == 2620123 &&
	      v.found[0].calls == 1 && !v.found[0].enc);
	/* the call ends: the hang, a reply in it plays too (a second call) */
	h.call = false;
	h.dst = h.src = 0;
	CHECK(hold(&h, DMRS_HANG_MS - 100u) == 0 && dmrs_snapshot(&v) && !v.playing);
	h.call = true;
	h.dst = 2620;
	h.src = 2627777;
	CHECK(hold(&h, 1000u) == 0 && dmrs_snapshot(&v) && v.playing && v.found[0].calls == 2 && v.found[0].src == 2627777);
	h.call = false;
	h.dst = h.src = 0;
	t0 = now;
	CHECK(hold(&h, 5000u) == 433512500u && dmrs_phase() == DMRS_SWEEP);	/* then on from the next step */
	CHECK(now - t0 >= DMRS_HANG_MS && now - t0 <= DMRS_HANG_MS + POLL_MS);
	CHECK(dmrs_snapshot(&v) && v.found_n == 1 && v.found[0].dst == 2620 && v.found[0].src == 2627777);

	/* DMR without a call (an idle repeater, data): noted, given up after DMRS_LATE_MS from the hit, not skipped */
	CHECK(run(one, 81u * 8u) == 433500000u);
	t0 = now;
	memset(&h, 0, sizeof h);
	h.bursts = 20;
	h.cc = 1;
	h.sync_mask = DMRS_SYNC_BS;
	/* (the hit may be a revisit: then the sweep goes on where it was, not from the next step) */
	CHECK(hold(&h, 5000u) != 0 && now - t0 >= DMRS_LATE_MS && now - t0 <= DMRS_LATE_MS + POLL_MS);
	CHECK(run(one, 81u * 8u) == 433500000u);	/* probed again on the next pass */
	/* an encrypted call: noted as such, not held for it */
	h.other = true;
	h.dst = 9;
	h.group = true;
	h.src = 1234;
	CHECK(hold(&h, 5000u) != 0);
	CHECK(dmrs_snapshot(&v) && v.found_n == 1 && v.found[0].enc && v.found[0].dst == 9 && v.found[0].calls == 2);

	/* A carrier that isn't DMR: given up after DMRS_PROBE_MS and skipped for DMRS_SKIP_MS */
	uint32_t first = run(two, 81u * 8u);	/* 433.5 may come first: it was found, so it is revisited */
	CHECK(first == 433025000u || first == 433500000u);
	memset(&h, 0, sizeof h);
	h.cc = DMRS_CC_NONE;
	t0 = now;
	CHECK(hold(&h, 5000u) != 0 && now - t0 >= DMRS_PROBE_MS && now - t0 <= DMRS_PROBE_MS + POLL_MS);
	uint32_t second = run(two, 81u * 8u);
	CHECK((second == 433025000u || second == 433500000u) && second != first);	/* the other one, not again */
	CHECK(hold(&h, 5000u) != 0);
	CHECK(run(two, 81u * 4u) == 0);		/* both skipped now: a whole pass, no hit */
	now += DMRS_SKIP_MS;
	CHECK(run(two, 81u * 8u) != 0);		/* after the skip time: probed again */
	hold(&h, 5000u);

	/* Weak bumps under DMRS_HIT_MIN_DBM are no hit */
	dmrs_start(433000000u, 434000000u, 12500u, now);
	CHECK(run(weak, 81u * 8u) == 0);
	/* A direct mode transmitter (30 ms on in 60) is a hit on every pass, whatever its phase against the steps */
	for (unsigned pass = 0; pass < 6u; pass++) {
		CHECK(run(dmo, 81u * 6u) == 433500000u);
		memset(&h, 0, sizeof h);
		h.bursts = 5;
		h.cc = 7;
		h.sync_mask = DMRS_SYNC_MS;
		hold(&h, 5000u);
		now += 7u * pass;	/* another phase next time */
	}
	CHECK(dmrs_snapshot(&v) && v.found[0].hz == 433500000u && v.found[0].cc == 7 &&
	      v.found[0].sync_mask == (DMRS_SYNC_BS | DMRS_SYNC_MS));

	/* The found list: newest first, at most DMRS_FOUND_MAX, survives a restart */
	dmrs_start(430000000u, 440000000u, 12500u, now);
	CHECK(dmrs_snapshot(&v) && v.found_n == 1);	/* 433.500; 433.025 never was DMR */
	for (unsigned k = 0; k < 10u; k++) {
		uint32_t hz = 430000000u + 25000u * k;
		while (dmrs_at_hz() != hz) {
			dmrs_snapshot(&v);
			dmrs_sweep(-124, now += POLL_MS, &hit);
		}
		hit = false;
		for (unsigned i = 0; i < 4u && !hit; i++)
			dmrs_sweep(-70, now += POLL_MS, &hit);
		CHECK(hit);
		memset(&h, 0, sizeof h);
		h.bursts = 3;
		h.cc = (uint8_t)k;
		hold(&h, 5000u);
	}
	CHECK(dmrs_snapshot(&v) && v.found_n == DMRS_FOUND_MAX && v.found[0].cc == 9 && v.found[7].cc == 2);

	/* Revisits: a found frequency in the range is read again every DMRS_REVISIT_STEPS steps, the sweep goes on
	 * where it was, and a new call there is caught within about that many steps (not a whole pass) */
	dmrs_start(432500000u, 434500000u, 12500u, now);	/* 161 steps; the 430.xxx finds are out of range */
	while (dmrs_at_hz() != 433500000u) {
		dmrs_snapshot(&v);
		dmrs_sweep(-124, now += POLL_MS, &hit);
	}
	hit = false;
	for (unsigned i = 0; i < 4u && !hit; i++)
		dmrs_sweep(-70, now += POLL_MS, &hit);
	CHECK(hit);
	memset(&h, 0, sizeof h);
	h.bursts = 3;
	h.cc = 1;
	CHECK(hold(&h, 5000u) == 433512500u);	/* the sweep goes on from the next step */
	unsigned visits = 0, steps = 0, gap = 0, max_gap = 0;
	uint32_t sweeps0 = (dmrs_snapshot(&v), v.sweeps), last = 0;
	bool order_ok = true;
	while (steps < 400u) {
		dmrs_snapshot(&v);
		uint32_t at = dmrs_at_hz();
		uint32_t next = dmrs_sweep(-124, now += POLL_MS, &hit);
		if (next == 0)
			continue;	/* settling, reading */
		CHECK(!hit);
		steps++;
		if (at == 433500000u) {
			visits++;
			if (gap > max_gap)
				max_gap = gap;
			gap = 0;
			if (last == 433487500u)
				last = 433500000u;	/* the sweep's own step there, not a revisit */
		} else {
			gap++;
			if (last != 0 && at != 432500000u && at != last + 12500u)
				order_ok = false;	/* sweep steps stay in order around the revisits */
			last = at;
		}
	}
	CHECK(visits >= 400u / (DMRS_REVISIT_STEPS + 1u) && max_gap <= DMRS_REVISIT_STEPS + 1u && order_ok);
	CHECK(dmrs_snapshot(&v) && v.sweeps > sweeps0);	/* the sweep still wraps */
	/* A call starts on 433.5: caught within a revisit period */
	unsigned waited = 0;
	for (hit = false; !hit && waited < 200u; ) {
		dmrs_snapshot(&v);
		if (dmrs_sweep(dmrs_at_hz() == 433500000u ? -70 : -124, now += POLL_MS, &hit) != 0 || hit)
			waited++;
	}
	CHECK(hit && dmrs_at_hz() == 433500000u && waited <= DMRS_REVISIT_STEPS + 2u);
	h.call = true;
	h.dst = 101;
	h.src = 77285;
	hold(&h, 1000u);
	h.call = false;
	uint32_t back = hold(&h, 5000u);
	CHECK(back != 0 && back != 433500000u && dmrs_phase() == DMRS_SWEEP);	/* the sweep resumes where it was */

	/* The PC's search log (dmrs_peek): the state and one entry at a time, the call just played among them */
	dmrs_peek_t pk;
	bool seen = false;
	for (unsigned i = 0; dmrs_peek(i, &pk); i++)
		seen |= pk.f.hz == 433500000u && pk.f.dst == 101u && pk.f.src == 77285u && pk.f.calls > 0;
	CHECK(seen && pk.found_n > 0 && pk.phase == DMRS_SWEEP && pk.lo_hz == 432500000u);
	CHECK(!dmrs_peek(DMRS_FOUND_MAX, &pk));

	/* Nobody looks (the screen was left): the search stops by itself, also while the PC reads it all the time */
	dmrs_start(433000000u, 434000000u, 12500u, now);
	t0 = now;
	while (dmrs_on() && now - t0 < 20000u) {
		(void)dmrs_peek(0u, &pk);
		dmrs_sweep(-124, now += POLL_MS, &hit);
	}
	CHECK(!dmrs_on() && now - t0 >= DMRS_IDLE_MS && now - t0 < DMRS_IDLE_MS + 50u);
	/* reversed range, stop: probe and sweep do nothing when off */
	CHECK(dmrs_start(434000000u, 433000000u, 0, now) == 433000000u);
	dmrs_stop();
	CHECK(!dmrs_on() && dmrs_probe(&h, now) == 0 && dmrs_sweep(-70, now + 100u, &hit) == 0 && !hit);
	printf("Plus DMR search: sweep/wrap, readings, floor, hits, probe DMR/not, call and hang, skips, found list, "
	       "revisits: %d failures\n", failures);
	return failures ? 1 : 0;
}
