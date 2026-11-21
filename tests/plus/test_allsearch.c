/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus all digital search engine (dmrsearch.c, CONFIG_ALL_SEARCH): a hit probed in the priority order P25 > DMR >
 * YSF > NXDN with each mode's window, the first confirmed mode winning (found list with the mode and its fields: P25
 * NAC, talkgroup and source; DMR color code and slots; YSF DG-ID and source callsign; NXDN RAN, group and source), the
 * call and the hang, a carrier none of them confirm skipped, a find's own mode tried first when it is hit again, a
 * mixed mode frequency, the receivers' hand-offs (dmrs_native_hz, dmrs_nxdn_hz, dmrs_retuned, dmrs_regained), the two
 * kinds sharing the state (snapshots and peeks of the other kind read as stopped and are no look), the callsign
 * packing, and the DMR search kind unchanged (one mode, no order). Built with and without CONFIG_SEARCH_FAST. */
#include <stdio.h>
#include <string.h>
#include "radio/dmrsearch.h"

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define POLL_MS 25u	/* RADIO_POLL_MS */

static uint32_t now = 1000;

/* Sweep until a hit (or max polls) with the screen looking; level(hz) is the reading. Returns the hit or 0 */
static uint32_t run(int16_t (*level)(uint32_t), unsigned max)
{
	for (unsigned i = 0; i < max; i++) {
		bool hit;
		dmrs_view_t look;
		dmrs_snapshot_all(&look);
		now += POLL_MS;
		dmrs_sweep(level(dmrs_at_hz()), now, &hit);
		if (hit)
			return dmrs_at_hz();
	}
	return 0;
}

/* Carriers at 433.500 and 433.750 (13 steps from the bottom apart) */
static int16_t two(uint32_t hz) { return hz == 433500000u || hz == 433750000u ? -80 : -124; }

/* Hold the probe `ms` with what the receiver heard; the first retune asked for (0 = none) */
static uint32_t hold(const dmrs_heard_t *h, uint32_t ms)
{
	for (uint32_t t = 0; t < ms; t += POLL_MS) {
		dmrs_view_t look;
		dmrs_snapshot_all(&look);
		now += POLL_MS;
		uint32_t hz = dmrs_probe(h, now);
		if (hz != 0)
			return hz;
	}
	return 0;
}

static dmrs_heard_t nothing(void)
{
	dmrs_heard_t h;
	memset(&h, 0, sizeof h);
	h.cc = DMRS_CC_NONE;
	return h;
}

/* A mode not there: its window runs out to the poll; returns the retune and checks the time it took */
static void runs_out(uint32_t window, uint8_t next_mode)
{
	dmrs_heard_t h = nothing();
	uint32_t t0 = now;
	uint32_t hz = hold(&h, 5000u);
	CHECK(now - t0 >= window && now - t0 <= window + POLL_MS);
	if (next_mode != DMRS_MODES)
		CHECK(hz == dmrs_at_hz() && dmrs_phase() == DMRS_PROBE && dmrs_mode() == next_mode);
	else
		CHECK(hz != 0 && dmrs_phase() == DMRS_SWEEP);
}

static void test_callsigns(void)
{
	uint32_t hi, lo;
	char out[11];
	dmrs_call_pack("TA2DOU", &hi, &lo);
	dmrs_call_unpack(hi, lo, out);
	CHECK(lo != 0 && hi == 0 && strcmp(out, "TA2DOU") == 0);
	dmrs_call_pack("ta1abc/m-1", &hi, &lo);	/* ten characters, lower case read as upper */
	dmrs_call_unpack(hi, lo, out);
	CHECK(strcmp(out, "TA1ABC/M-1") == 0);
	dmrs_call_pack("YM2KDZ ID.TOOLONG", &hi, &lo);	/* cut to ten, an inner space kept */
	dmrs_call_unpack(hi, lo, out);
	CHECK(strcmp(out, "YM2KDZ ID.") == 0);
	dmrs_call_pack("A#B", &hi, &lo);		/* outside the set: '.' */
	dmrs_call_unpack(hi, lo, out);
	CHECK(strcmp(out, "A.B") == 0);
	dmrs_call_pack("", &hi, &lo);
	dmrs_call_unpack(hi, lo, out);
	CHECK(hi == 0 && lo == 0 && out[0] == '\0');
	dmrs_call_pack(NULL, &hi, &lo);
	CHECK(hi == 0 && lo == 0);
	dmrs_call_unpack(0xFFFFFFFFu, 0xFFFFFFFFu, out);	/* garbage still gives ten characters of the set */
	CHECK(strlen(out) <= 10u);
	CHECK(strcmp(dmrs_mode_name(DMRS_MODE_P25), "P25") == 0 && strcmp(dmrs_mode_name(DMRS_MODE_NXDN), "NXDN") == 0 &&
	      strcmp(dmrs_mode_name(DMRS_MODES), "?") == 0);
}

int main(void)
{
	dmrs_view_t v;
	dmrs_heard_t h;
	dmrs_peek_t pk;
	bool hit, ysf;
	test_callsigns();

	/* --- The all digital search: started, the DMR search's view and peek read it as stopped and empty --- */
	CHECK(dmrs_start_all(433000000u, 434000000u, 12500u, now) == 433000000u && dmrs_on() &&
	      dmrs_kind() == DMRS_KIND_ALL && dmrs_phase() == DMRS_SWEEP);
	CHECK(dmrs_snapshot(&v) && v.phase == DMRS_OFF && v.found_n == 0 && v.kind == DMRS_KIND_DMR);
	CHECK(dmrs_snapshot_all(&v) && v.phase == DMRS_SWEEP && v.kind == DMRS_KIND_ALL);
	CHECK(!dmrs_peek(0, &pk) && pk.phase == DMRS_OFF && pk.found_n == 0);
	CHECK(dmrs_native_hz(&ysf) == 0 && dmrs_nxdn_hz() == 0);	/* sweeping: no mode's receiver */

	/* --- A carrier none of the modes confirm: P25, DMR, YSF, NXDN each for its window, then skipped --- */
	CHECK(run(two, 81u * 8u) == 433500000u && dmrs_phase() == DMRS_PROBE && dmrs_mode() == DMRS_MODE_P25);
	CHECK(dmrs_native_hz(&ysf) == 433500000u && !ysf && dmrs_nxdn_hz() == 0);	/* the native receiver's P25 */
	CHECK(dmrs_snapshot_all(&v) && v.mode == DMRS_MODE_P25 && !v.playing);
	runs_out(DMRS_P25_PROBE_MS, DMRS_MODE_DMR);
	CHECK(dmrs_native_hz(&ysf) == 0 && dmrs_nxdn_hz() == 0);	/* DMR: the radio core's */
	runs_out(DMRS_PROBE_MS, DMRS_MODE_YSF);
	CHECK(dmrs_native_hz(&ysf) == 433500000u && ysf);
	runs_out(DMRS_YSF_PROBE_MS, DMRS_MODE_NXDN);
	CHECK(dmrs_nxdn_hz() == 433500000u && dmrs_native_hz(&ysf) == 0);
	runs_out(DMRS_NXDN_PROBE_MS, DMRS_MODES);	/* none: back to the sweep, skipped */
	CHECK(dmrs_snapshot_all(&v) && v.found_n == 0 && v.found_dmr == 0);
	CHECK(run(two, 81u * 8u) == 433750000u);	/* the next carrier, not 433.500 again */

	/* --- 433.750 is DMR: P25 runs out, DMR confirmed (as the DMR search: color code, slots, the call) --- */
	runs_out(DMRS_P25_PROBE_MS, DMRS_MODE_DMR);
	h = nothing();
	h.bursts = 4;
	h.cc = 3;
	h.ts_mask = 1;
	h.sync_mask = DMRS_SYNC_BS;
	CHECK(hold(&h, 100u) == 0 && dmrs_mode() == DMRS_MODE_DMR);
	h.call = h.group = true;
	h.dst = 2620;
	h.src = 2620123;
	CHECK(hold(&h, 3000u) == 0 && dmrs_phase() == DMRS_CALL && dmrs_snapshot_all(&v) && v.playing &&
	      v.mode == DMRS_MODE_DMR);
	CHECK(v.found_n == 1 && v.found[0].hz == 433750000u && v.found[0].mode == DMRS_MODE_DMR && v.found[0].cc == 3 &&
	      v.found[0].ts_mask == 1 && v.found[0].sync_mask == DMRS_SYNC_BS && v.found[0].dst == 2620 &&
	      v.found[0].src == 2620123 && v.found[0].group && v.found[0].calls == 1);
	h.call = false;
	uint32_t t0 = now;
	CHECK(hold(&h, 5000u) != 0 && dmrs_phase() == DMRS_SWEEP && now - t0 >= DMRS_HANG_MS);

	/* --- P25 confirmed at once (the first mode): NAC, talkgroup, source, the call held, the hang --- */
	now += DMRS_SKIP_MS + 1000u;	/* 433.500's skip has run out */
	uint32_t at = run(two, 81u * 8u);
	for (int k = 0; k < 8 && at == 433750000u; k++) {	/* the DMR find, hit again (revisits): its own mode first */
		CHECK(dmrs_mode() == DMRS_MODE_DMR);
		h = nothing();
		h.bursts = 3;
		h.cc = 3;
		hold(&h, 5000u);
		at = run(two, 81u * 8u);
	}
	CHECK(at == 433500000u && dmrs_mode() == DMRS_MODE_P25);
	h = nothing();
	CHECK(hold(&h, 400u) == 0);
	dmrs_retuned(now);	/* the native receiver took it late: its window counts from the take */
	CHECK(hold(&h, DMRS_P25_PROBE_MS - 2u * POLL_MS) == 0 && dmrs_mode() == DMRS_MODE_P25);
	h.bursts = 9;
	h.cc = 0x1C8;
	h.call = h.group = true;
	h.dst = 655;
	h.src = 1234567;
	CHECK(hold(&h, 2000u) == 0 && dmrs_phase() == DMRS_CALL && dmrs_snapshot_all(&v) && v.playing &&
	      v.mode == DMRS_MODE_P25 && v.found_n == 2);
	CHECK(v.found[0].hz == 433500000u && v.found[0].mode == DMRS_MODE_P25 && v.found[0].cc == 0x1C8 &&
	      v.found[0].dst == 655 && v.found[0].src == 1234567 && v.found[0].calls == 1 && v.found_dmr >= 2);
	CHECK(dmrs_native_hz(&ysf) == 433500000u);	/* the native receiver keeps it while the call plays */
	h.call = false;
	t0 = now;
	CHECK(hold(&h, 5000u) != 0 && now - t0 >= DMRS_HANG_MS && dmrs_phase() == DMRS_SWEEP);
	CHECK(dmrs_native_hz(&ysf) == 0);
	uint8_t e0 = dmrs_epoch();
	dmrs_regained();
	CHECK(dmrs_epoch() != e0);

	/* --- The PC's log: dmrs_peek_all has both, the DMR search's peek none --- */
	unsigned n = 0, p25 = 0, dmr = 0;
	for (unsigned i = 0; dmrs_peek_all(i, &pk); i++, n++) {
		p25 += pk.f.mode == DMRS_MODE_P25;
		dmr += pk.f.mode == DMRS_MODE_DMR;
	}
	CHECK(n == 2 && p25 == 1 && dmr == 1 && pk.phase == DMRS_SWEEP && pk.lo_hz == 433000000u);
	CHECK(!dmrs_peek(0, &pk) && pk.found_n == 0);

	/* --- YSF and NXDN on fresh frequencies (a new range): the found entries' fields --- */
	dmrs_start_all(438000000u, 439000000u, 12500u, now);	/* the same kind: the list stays */
	CHECK(dmrs_snapshot_all(&v) && v.found_n == 2);
	for (unsigned k = 0; k < 2u; k++) {
		uint32_t hz = 438100000u + 200000u * k;
		while (dmrs_at_hz() != hz) {
			dmrs_snapshot_all(&v);
			dmrs_sweep(-124, now += POLL_MS, &hit);
		}
		hit = false;
		for (unsigned i = 0; i < 6u && !hit; i++)
			dmrs_sweep(-70, now += POLL_MS, &hit);
		CHECK(hit && dmrs_mode() == DMRS_MODE_P25);
		runs_out(DMRS_P25_PROBE_MS, DMRS_MODE_DMR);
		runs_out(DMRS_PROBE_MS, DMRS_MODE_YSF);
		h = nothing();
		if (k == 0) {
			h.bursts = DMRS_MIN_BURSTS;
			h.cc = 0;	/* DG-ID 0 */
			h.call = true;
			dmrs_call_pack("TA2DOU", &h.dst, &h.src);
			CHECK(hold(&h, 500u) == 0 && dmrs_phase() == DMRS_CALL && dmrs_snapshot_all(&v) &&
			      v.mode == DMRS_MODE_YSF);
			char call[11];
			dmrs_call_unpack(v.found[0].dst, v.found[0].src, call);
			CHECK(v.found[0].mode == DMRS_MODE_YSF && v.found[0].cc == 0 && strcmp(call, "TA2DOU") == 0);
			dmrs_call_pack("YM2KDZ", &h.dst, &h.src);	/* the next caller: no tail of the last */
			hold(&h, 200u);
			CHECK(dmrs_snapshot_all(&v));
			dmrs_call_unpack(v.found[0].dst, v.found[0].src, call);
			CHECK(strcmp(call, "YM2KDZ") == 0);
		} else {
			runs_out(DMRS_YSF_PROBE_MS, DMRS_MODE_NXDN);
			CHECK(dmrs_nxdn_hz() == hz);
			h.bursts = DMRS_MIN_BURSTS;
			h.cc = 5;
			h.call = h.group = true;
			h.dst = 101;
			h.src = 1234;
			CHECK(hold(&h, 500u) == 0 && dmrs_phase() == DMRS_CALL && dmrs_snapshot_all(&v) &&
			      v.found[0].mode == DMRS_MODE_NXDN && v.found[0].cc == 5 && v.found[0].dst == 101 &&
			      v.found[0].src == 1234 && v.found[0].group);
			/* the frames stop for a moment (heard in the last 300 ms only): still NXDN, not the next mode */
			h.bursts = 0;
		}
		h.call = false;
		CHECK(hold(&h, 5000u) != 0 && dmrs_phase() == DMRS_SWEEP);
	}
	CHECK(dmrs_snapshot_all(&v) && v.found_n == 4 && v.found[0].mode == DMRS_MODE_NXDN &&
	      v.found[1].mode == DMRS_MODE_YSF);

	/* --- A mixed mode frequency: DMR at 438.100 (a YSF find) is an entry of its own --- */
	while (dmrs_at_hz() != 438100000u) {
		dmrs_snapshot_all(&v);
		dmrs_sweep(-124, now += POLL_MS, &hit);
	}
	hit = false;
	for (unsigned i = 0; i < 6u && !hit; i++)
		dmrs_sweep(-70, now += POLL_MS, &hit);
	CHECK(hit && dmrs_mode() == DMRS_MODE_YSF);	/* the find's own mode first */
	runs_out(DMRS_YSF_PROBE_MS, DMRS_MODE_P25);	/* then the order, YSF already tried */
	runs_out(DMRS_P25_PROBE_MS, DMRS_MODE_DMR);
	h = nothing();
	h.bursts = 5;
	h.cc = 1;
	hold(&h, 5000u);
	CHECK(dmrs_snapshot_all(&v) && v.found_n == 5 && v.found[0].hz == 438100000u &&
	      v.found[0].mode == DMRS_MODE_DMR && v.found[2].hz == 438100000u && v.found[2].mode == DMRS_MODE_YSF);

	/* --- The found list's key (owner 2026-11-21): frequency + mode + code + group, one entry with a call counter --- */
	{
		const uint32_t F = 438500000u;
		while (dmrs_at_hz() != F) {
			dmrs_snapshot_all(&v);
			dmrs_sweep(-124, now += POLL_MS, &hit);
		}
		hit = false;
		for (unsigned i = 0; i < 6u && !hit; i++)
			dmrs_sweep(-70, now += POLL_MS, &hit);
		CHECK(hit && dmrs_mode() == DMRS_MODE_P25);
		h = nothing();
		h.bursts = 9;
		h.cc = 0x1C8;
		h.call = h.group = true;
		h.dst = 655;
		h.src = 1234567;
		hold(&h, 500u);
		/* the terminator: the native receiver's NAC reads "any" (0xFFFF = DMRS_CC_NONE), the same entry */
		h.cc = DMRS_CC_NONE;
		h.bursts = 10;
		hold(&h, 200u);
		CHECK(dmrs_snapshot_all(&v) && v.found[0].hz == F && v.found[0].mode == DMRS_MODE_P25 && v.found[0].cc == 0x1C8 &&
		      v.found[0].dst == 655 && v.found[0].calls == 1 && v.found_n == 6 && v.found[1].hz != F);
		h.call = false;
		CHECK(hold(&h, 5000u) != 0 && dmrs_phase() == DMRS_SWEEP);
		/* hit again: P25 first (its find's mode); the same NAC, another talkgroup: its own entry */
		while (dmrs_at_hz() != F) {
			dmrs_snapshot_all(&v);
			dmrs_sweep(-124, now += POLL_MS, &hit);
		}
		hit = false;
		for (unsigned i = 0; i < 6u && !hit; i++)
			dmrs_sweep(-70, now += POLL_MS, &hit);
		CHECK(hit && dmrs_mode() == DMRS_MODE_P25);
		h = nothing();
		h.bursts = 9;
		h.cc = 0x1C8;
		h.call = h.group = true;
		h.dst = 656;
		h.src = 7654321;
		hold(&h, 500u);
		h.call = false;
		CHECK(hold(&h, 5000u) != 0);
		CHECK(dmrs_snapshot_all(&v) && v.found_n == 7 && v.found[0].hz == F && v.found[0].dst == 656 &&
		      v.found[0].calls == 1 && v.found[1].hz == F && v.found[1].dst == 655 && v.found[1].calls == 1);
		for (unsigned i = 0; i < v.found_n; i++)
			for (unsigned j = i + 1u; j < v.found_n; j++)
				CHECK(!(v.found[i].hz == v.found[j].hz && v.found[i].mode == v.found[j].mode &&
					v.found[i].cc == v.found[j].cc && v.found[i].dst == v.found[j].dst));
	}

	/* --- Nobody looks (the screen was left): it stops; the DMR search's screen keeps nothing alive --- */
	t0 = now;
	while (dmrs_on() && now - t0 < 20000u) {
		dmrs_snapshot(&v);	/* the other kind's screen */
		(void)dmrs_peek_all(0u, &pk);	/* the PC */
		dmrs_sweep(-124, now += POLL_MS, &hit);
	}
	CHECK(!dmrs_on() && now - t0 >= DMRS_IDLE_MS && now - t0 < DMRS_IDLE_MS + 50u);

	/* --- The DMR search kind: the list cleared, one mode, a non-DMR carrier skipped after DMRS_PROBE_MS --- */
	dmrs_start(433000000u, 434000000u, 12500u, now);
	CHECK(dmrs_kind() == DMRS_KIND_DMR && dmrs_snapshot(&v) && v.found_n == 0 && v.phase == DMRS_SWEEP);
	CHECK(dmrs_snapshot_all(&v) && v.phase == DMRS_OFF && v.found_n == 0);
	at = run(two, 81u * 8u);
	CHECK(at != 0 && dmrs_mode() == DMRS_MODE_DMR && dmrs_native_hz(&ysf) == 0 && dmrs_nxdn_hz() == 0);
	h = nothing();
	t0 = now;
	for (uint32_t t = 0; t < 5000u && dmrs_phase() != DMRS_SWEEP; t += POLL_MS) {
		dmrs_snapshot(&v);
		dmrs_probe(&h, now += POLL_MS);
	}
	CHECK(dmrs_phase() == DMRS_SWEEP && now - t0 >= DMRS_PROBE_MS && now - t0 <= DMRS_PROBE_MS + POLL_MS);
	dmrs_stop();
	printf("Plus all digital search: priority order and windows, P25/DMR/YSF/NXDN finds, call and hang, skip, "
	       "find's mode first, mixed mode, hand-offs, kinds, callsigns: %d failures\n", failures);
	return failures ? 1 : 0;
}
