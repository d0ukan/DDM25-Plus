/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus cross-band store and forward in the real radio core (radio_sm.c with CONFIG_XBAND=1: radio_xband.inc
 * and the engine xband.c), on radioCoreTest's fake board, its simulated DMR modem and the real DMR call stack, after
 * upstream's and Classic's radio suites (tests/classic/test_radio.inc) have run again with it built in. Included after
 * them by tools/test_classic.py (test_radio_xband), linked with -Wl,--wrap=beep_play so the beeps played are seen.
 *
 * Checked: a session takes the radio from an FM channel onto its own DMR channel (A, the colour code and slot, every
 * talkgroup heard, the lowest power) and borrows the P25 worker's memory; PTT is refused with the deny tone and never
 * keys; requests that would move the radio are dropped; a DMR call heard on A is recorded frame by frame and sent on
 * B through the call stack (the voice header to the talkgroup heard, from our own ID, every voice burst's 27 bytes
 * as heard, in order, then the terminator), the transmitter let go after; then the CW ID on B (FM, the callsign in
 * Morse, the session's DMR channel back after); the receiver goes between A and B while nothing is heard; the stop
 * puts the user's FM channel back exactly and gives the memory back; a transmission the board refuses drops the over
 * (no air, an error beep, no ID); a P25 worker that won't let go of its memory ends the session (nothing keyed). */
#include "radio/xband.h"
#include "radio/beep.h"

/* --- The beeps the radio core plays (the real beep.c behind) --- */
static uint32_t xr_beeps[BEEP_COUNT];
void __real_beep_play(beep_t b, uint8_t level, bool shared);
void __wrap_beep_play(beep_t b, uint8_t level, bool shared)
{
	if ((unsigned)b < BEEP_COUNT)
		xr_beeps[b]++;
	__real_beep_play(b, level, shared);
}

/* The UI's request reaches the radio task (radio_task.c's radio_req_xband) */
void radio_req_xband(bool on)
{
	req(RREQ_XBAND, on ? 1 : 0);
}

/* --- The P25 worker's memory (p25_lab.c): three pieces as on the radio, refused while it runs --- */
static uint8_t xr_mem[17104];
static int xr_lends, xr_gives;
static bool xr_lent;
uint8_t p25_lab_lend(uint8_t *seg[], size_t len[])
{
	if (classic_busy || xr_lent)
		return 0;
	xr_lent = true;
	xr_lends++;
	seg[0] = xr_mem;
	len[0] = 6916;
	seg[1] = xr_mem + 6916;
	len[1] = 5788;
	seg[2] = xr_mem + 6916 + 5788;
	len[2] = 4400;
	return 3;
}
void p25_lab_give_back(void)
{
	CHECK(xr_lent);
	memset(xr_mem, 0, sizeof xr_mem);
	xr_lent = false;
	xr_gives++;
}

static void xr_reset(void)
{
	memset(xr_beeps, 0, sizeof xr_beeps);
	xr_lends = xr_gives = 0;
	xr_lent = false;
	classic_busy = false;
	tx_fail = false;
}

static void xr_req(uint32_t a, uint32_t b)
{
	xband_req_t *r = xband_req();
	r->hz[0] = a;
	r->hz[1] = b;
	r->mode = XB_MODE_DMR;
	r->cc = 1;
	r->ts = 1;
	r->power = 1;
	strcpy(r->call, "TA2DOU");
}

/* The radio on A (the engine goes between A and B while nothing is heard) */
static bool xr_on(uint32_t hz)
{
	for (int i = 0; i < 100 && hw.rx_hz != hz; i++)
		run(25);
	return hw.rx_hz == hz;
}

/* A voice burst whose three AMBE frames hold (burst, frame) in every byte */
static void xr_voice(uint8_t index, uint8_t cc, const uint8_t *lc, uint8_t n)
{
	dmr_burst_t b;
	memset(&b, 0, sizeof b);
	b.kind = DMR_BURST_VOICE;
	b.voice_index = index;
	b.sync = index == 0 ? DMR_SYNC_MS : DMR_SYNC_NONE;
	b.cc = cc;
	b.crc_ok = true;
	b.in_step = true;
	b.rssi_dbm = -80;
	b.len = 27;
	for (int f = 0; f < 3; f++)
		memset(&b.payload[f * 9], (int)(n * 3u + (unsigned)f + 1u), 9);
	if (lc != NULL) {
		b.lc_valid = true;
		memcpy(b.lc, lc, 9);
	}
	sim_rx(&b);
}

/* A group call on the session's colour code: three headers, superframes of voice (bursts numbered from 0), the end */
static unsigned xr_call(uint32_t dst, uint32_t src, int superframes)
{
	uint8_t lc[9];
	mk_lc(lc, true, dst, src);
	for (int h = 0; h < 3; h++) {
		burst_lc(lc, 1, false);
		run(60);
	}
	unsigned n = 0;
	for (int sf = 0; sf < superframes; sf++) {
		for (int i = 0; i < 6; i++) {
			xr_voice((uint8_t)i, 1, i == 5 ? lc : NULL, (uint8_t)n++);
			run(60);
		}
	}
	burst_lc(lc, 1, true);
	return n;
}

/* The user's place as the radio core holds it */
typedef struct {
	bool vfo;
	uint16_t number;
	uint32_t rx_hz, tx_hz;
	uint8_t mode, power;
} xr_place_t;
static xr_place_t xr_place(void)
{
	xr_place_t p = { sm.view.vfo, sm.view.channel_number, sm.view.ch.rx_hz, sm.view.ch.tx_hz, sm.view.ch.mode,
			 sm.view.power };
	return p;
}
static bool xr_same(const xr_place_t *a, const xr_place_t *b)
{
	return a->vfo == b->vfo && a->number == b->number && a->rx_hz == b->rx_hz && a->tx_hz == b->tx_hz &&
	       a->mode == b->mode && a->power == b->power;
}

/* A call on A relayed on B, the CW ID, the stop */
static void test_xband_relay(void)
{
	setup_codeplug();
	codec_present = true;
	start();
	xr_reset();
	run(500);
	CHECK(sm.view.channel_number == 1 && sm.view.ch.mode == RADIO_MODE_FM && hw.rx_hz == 146520000);
	xr_place_t before = xr_place();
	xr_req(145500000, 433500000);
	CHECK(xband_start());
	run(100);
	CHECK(xband_owns_radio() && sm.view.ch.mode == RADIO_MODE_DMR && hw.rx_hz == 145500000 && hw_mode == RADIO_MODE_DMR);
	CHECK(sm.view.ch.color_code == 1 && sm.view.ch.timeslot == 1 && sm.view.dmr.hear_all_latch && xr_lends == 1);
	xband_view_t v;
	xband_view(&v);
	CHECK(v.capacity == 1899 && v.phase == XB_P_LISTEN);	/* 768 + 643 + 488 frames, 37.98 s */

	/* PTT: the deny tone, never keyed; requests that would move the radio: dropped */
	int tx0 = hw.tx_calls;
	req(RREQ_PTT, 1);
	run(100);
	CHECK(xr_beeps[BEEP_TX_DENY] == 1 && hw.tx_calls == tx0 && sm.view.state != RS_TX);
	req(RREQ_PTT, 0);
	req(RREQ_CH_STEP, 1);
	req(RREQ_VFO, 1);
	run(100);
	CHECK(xband_owns_radio() && !sm.view.vfo && sm.view.channel_number == 1);

	/* a call to TG 2862 on A: recorded, then sent on B */
	CHECK(xr_on(145500000));
	unsigned air0 = sim.air_count;
	unsigned bursts = xr_call(2862, 3105555, 4);
	xband_view(&v);
	CHECK(v.phase == XB_P_RECORD && v.frames == 3u * bursts);
	run(200);
	CHECK(sm.view.state == RS_TX && hw.tx_hz == 433500000 && hw_mode == RADIO_MODE_DMR && hw.tx_power == 0);
	for (int i = 0; i < 400 && sm.view.state == RS_TX; i++)
		run(25);
	CHECK(sm.view.state != RS_TX);
	/* on the air: the header to TG 2862 from us, the voice bursts as heard, the terminator */
	unsigned first = air0;
	while (first < sim.air_count && sim.air[first].kind != DMR_BURST_VOICE_LC_HEADER)
		first++;
	CHECK(first < sim.air_count);
	dmr_lc_t l;
	air_lc(first, &l);
	CHECK(l.dst == 2862 && l.src == own_dmr_id && l.flco == DMR_FLCO_GROUP_VOICE);
	unsigned voice = 0, ok = 0;
	for (unsigned k = first; k < sim.air_count; k++) {
		if (sim.air[k].kind != DMR_BURST_VOICE)
			continue;
		if (voice < bursts) {
			bool same = true;
			for (int f = 0; f < 3; f++)
				for (int j = 0; j < 9; j++)
					same = same && sim.air[k].payload[f * 9 + j] == (uint8_t)(voice * 3u + (unsigned)f + 1u);
			ok += same;
		}
		voice++;
	}
	CHECK(ok == bursts && voice >= bursts && voice < bursts + 6);	/* the superframe's rest: silence */
	CHECK(sim.air[sim.air_count - 1].kind == DMR_BURST_TERMINATOR_LC);
	xband_view(&v);
	CHECK(v.relayed == 1);

	/* the CW ID on B: FM, the callsign keyed in Morse, then the session's DMR channel on B again */
	memset(&tone, 0, sizeof tone);
	run(300);
	CHECK(sm.cw_phase != 0 && hw_mode == RADIO_MODE_FM && hw.tx_hz == 433500000 && tone.hz != 0);
	for (int i = 0; i < 400 && sm.cw_phase != 0; i++)
		run(25);
	CHECK(sm.cw_phase == 0 && tone.changes > 20);	/* TA2DOU: 20 elements */
	run(100);
	xband_view(&v);
	CHECK(v.ids == 1 && v.phase == XB_P_LISTEN && hw_mode == RADIO_MODE_DMR && hw.rx_hz == 433500000);
	/* nothing heard: back and forth */
	run(1100);
	CHECK(hw.rx_hz == 145500000);
	run(1100);
	CHECK(hw.rx_hz == 433500000);

	/* the stop: the FM channel exactly as it was, the memory back */
	xband_stop();
	run(100);
	CHECK(!xband_owns_radio() && !xband_on() && xr_gives == 1 && !xr_lent);
	xr_place_t after = xr_place();
	CHECK(xr_same(&before, &after) && hw.rx_hz == 146520000 && hw_mode == RADIO_MODE_FM && !sm.view.dmr.hear_all_latch);
	req(RREQ_CH_STEP, 1);
	run(100);
	CHECK(sm.view.channel_number == 2);	/* requests work again */
	printf("Plus cross-band radio core: %u bursts heard on A, %u voice bursts sent on B, the CW ID, channel 1 back\n",
	       bursts, voice);
}

/* The board won't key on B: the over dropped (nothing on the air), an error beep, no ID; the session listens on */
static void test_xband_refused(void)
{
	setup_codeplug();
	codec_present = true;
	start();
	xr_reset();
	run(200);
	xr_req(145500000, 433500000);
	CHECK(xband_start());
	run(100);
	CHECK(xr_on(145500000));
	tx_fail = true;
	unsigned air0 = sim.air_count;
	xr_call(91, 3105555, 2);
	run(300);
	xband_view_t v;
	xband_view(&v);
	CHECK(sim.air_count == air0 && xr_beeps[BEEP_ERROR] >= 1 && v.relayed == 0 && xband_owns_radio());
	CHECK(sm.view.state != RS_TX && !sm.ptt);
	run(2000);
	xband_view(&v);
	CHECK(sm.cw_phase == 0 && v.ids == 0);
	xband_stop();
	run(100);
	CHECK(!xband_owns_radio() && xr_gives == 1 && hw.rx_hz == 146520000);
}

/* The P25 worker keeps its memory: the session ends (no memory), nothing keyed, the channel back */
static void test_xband_no_memory(void)
{
	setup_codeplug();
	codec_present = true;
	start();
	xr_reset();
	run(200);
	classic_busy = true;
	int tx0 = hw.tx_calls;
	xr_req(145500000, 433500000);
	CHECK(xband_start());
	run(500);
	CHECK(xband_owns_radio() && classic_stop);	/* asked to let go */
	run(2000);
	xband_view_t v;
	xband_view(&v);
	CHECK(!xband_owns_radio() && !xband_on() && v.ended == XB_E_MEMORY && xr_lends == 0 && xr_gives == 0);
	CHECK(hw.tx_calls == tx0 && hw.rx_hz == 146520000 && sm.view.ch.mode == RADIO_MODE_FM);
	classic_busy = false;
}

int main(void)
{
	int prior = classic_radio_main();
	int before = failures;
	test_xband_relay();
	test_xband_refused();
	test_xband_no_memory();
	printf("Plus cross-band in the radio core: %d failures (upstream's and Classic's suites with it built in: %s)\n",
	       failures - before, prior ? "FAILED" : "passed");
	return prior != 0 || failures != before;
}
