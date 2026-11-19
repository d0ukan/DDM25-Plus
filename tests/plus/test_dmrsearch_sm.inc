/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus DMR search in the real radio state machine (docs/plus.md): the board in DMR with the call stack hearing
 * every color code and slot, the sweep to a carrier, the bursts there into the found list, a call played (any
 * talkgroup), the hang and the resume, PTT and other requests ending it without keying, the idle stop, and the
 * P25 channel receiver kept off meanwhile. Included by test_radio.inc with CONFIG_DMR_SEARCH=1; the simulated modem
 * (sim_rx) brings the bursts. */
#include "radio/dmrsearch.h"

static int16_t ds_level(uint32_t hz) { return hz == 433500000u ? -80 : -124; }

/* One radio poll with the screen looking: the fake receiver's level follows the frequency it is on */
static void ds_poll(void)
{
    dmrs_view_t v;
    dmrs_snapshot(&v);
    hw.st.rssi_dbm = ds_level(hw.rx_hz);
    run(RADIO_POLL_MS);
}
static void ds_wait(uint32_t ms) { for (uint32_t t = 0; t < ms; t += RADIO_POLL_MS) ds_poll(); }

/* A burst from a repeater's outbound (BS sync, TS2, color code 5), as the HR_C7000 glue reports it */
static void ds_burst(uint8_t kind, uint8_t index, const uint8_t *lc)
{
    dmr_burst_t b;
    memset(&b, 0, sizeof b);
    b.kind = kind;
    b.voice_index = index;
    b.sync = kind == DMR_BURST_VOICE && index != 0 ? DMR_SYNC_NONE : DMR_SYNC_BS;
    b.cc = 5;
    b.ts = 2;
    b.crc_ok = true;
    b.in_step = true;
    b.rssi_dbm = -80;
    if (kind == DMR_BURST_VOICE) {
        b.len = 27;
        for (int f = 0; f < 3; f++) memcpy(&b.payload[f * 9], dmr_ambe_silence, 9);
        if (lc != NULL) { b.lc_valid = true; memcpy(b.lc, lc, 9); }
    } else if (kind == DMR_BURST_IDLE) {
        b.len = 12;
    } else {
        b.len = 12;
        dmr_lc_encode_full(lc, kind == DMR_BURST_TERMINATOR_LC ? DMR_RS_MASK_TERMINATOR : DMR_RS_MASK_VOICE_HEADER,
                           b.payload);
    }
    sim_rx(&b);
}

/* A group call TG 2620 from 2620123: headers, `sf` superframes, a terminator when `end` */
static void ds_call(int sf, bool end)
{
    uint8_t lc[9];
    mk_lc(lc, true, 2620, 2620123);
    for (int i = 0; i < 3; i++) { ds_burst(DMR_BURST_VOICE_LC_HEADER, 0, lc); ds_wait(60); }
    for (int s = 0; s < sf; s++)
        for (int i = 0; i < 6; i++) { ds_burst(DMR_BURST_VOICE, (uint8_t)i, i == 5 ? lc : NULL); ds_wait(60); }
    if (end) { ds_burst(DMR_BURST_TERMINATOR_LC, 0, lc); ds_wait(60); }
}

static void ds_sweep_to_hit(void)
{
    for (int i = 0; i < 4000 && dmrs_on() && dmrs_phase() == DMRS_SWEEP; i++) ds_poll();
}

static void test_plus_dmrsearch_sm(void)
{
    dmrs_view_t v;
    setup_codeplug();
    start();
    req(RREQ_ZONE_POS, 0);  /* Simplex, FM 146.520 */
    run(50);
    CHECK(sm.view.ch.mode == RADIO_MODE_FM && hw_mode == RADIO_MODE_FM && !sim.started);
    int tx0 = hw.tx_calls;
    unsigned begins0 = sim.tx_begins;
    radio_req_t r = { .type = RREQ_DMR_SEARCH, .arg = 433000000, .arg2 = 434000000 };
    radio_sm_request(&sm, &r, now);
    /* the board in DMR, the stack hearing every color code and both slots, direct mode framing */
    CHECK(dmrs_on() && hw_mode == RADIO_MODE_DMR && sim.started && sim.cc_filter == -1 && sim.locked_ts == 0 &&
          !sim.cfg.repeater && hw.rx_hz == 433000000u && !hw.audio);

    /* the sweep reaches the carrier at 433.500 and holds it */
    ds_sweep_to_hit();
    CHECK(dmrs_phase() == DMRS_PROBE && dmrs_at_hz() == 433500000u && hw.rx_hz == 433500000u);
    /* a repeater's call on TS2, color code 5, a talkgroup nobody listens to: noted and played */
    ds_call(2, false);
    CHECK(dmrs_snapshot(&v) && v.playing && dmrs_phase() == DMRS_CALL && v.found_n == 1 &&
          v.found[0].hz == 433500000u && v.found[0].cc == 5 && v.found[0].ts_mask == 2u &&
          v.found[0].sync_mask == DMRS_SYNC_BS && v.found[0].dst == 2620 && v.found[0].group &&
          v.found[0].src == 2620123 && v.found[0].calls == 1);
    CHECK(sm.view.state == RS_RX && sm.view.dmr.state == RDMR_RX && sm.view.dmr.call.dst == 2620 && aud.started);
    CHECK(hw.rx_hz == 433500000u && !hw.audio);   /* held there, the FM path muted (DMR voice is PCM) */
    /* the call ends: the hang, then the sweep goes on from the next step */
    uint8_t lc[9];
    mk_lc(lc, true, 2620, 2620123);
    ds_burst(DMR_BURST_TERMINATOR_LC, 0, lc);
    ds_wait(DMRS_HANG_MS - 300u);
    CHECK(dmrs_phase() == DMRS_CALL && hw.rx_hz == 433500000u && sm.view.dmr.state != RDMR_RX);
    ds_wait(600u);
    CHECK(dmrs_phase() == DMRS_SWEEP && dmrs_at_hz() >= 433512500u && hw.rx_hz >= 433512500u);
    CHECK(sm.view.dmr.state == RDMR_IDLE && sm.view.state == RS_IDLE);

    /* a carrier there again with only idle bursts (the repeater's hang): noted, given up after DMRS_LATE_MS */
    ds_sweep_to_hit();
    CHECK(dmrs_at_hz() == 433500000u);
    for (int i = 0; i < 10; i++) { ds_burst(DMR_BURST_IDLE, 0, NULL); ds_wait(60); }
    for (int i = 0; i < 40 && dmrs_phase() != DMRS_SWEEP; i++) ds_poll();
    CHECK(dmrs_phase() == DMRS_SWEEP && dmrs_snapshot(&v) && v.found_n == 1 && v.found[0].calls == 1 && !v.playing);

    /* PTT while searching: consumed, the search ends, nothing keyed, the channel tuned again (FM) */
    ds_poll();
    radio_req_t ptt = { .type = RREQ_PTT, .arg = 1 };
    CHECK(native_lab_request(&sm, &ptt) == 0);      /* the native owner leaves it to the radio core */
    radio_sm_request(&sm, &ptt, now);
    run(200);
    CHECK(!dmrs_on() && hw.tx_calls == tx0 && sim.tx_begins == begins0 && !sm.ptt && sm.view.state != RS_TX);
    CHECK(hw.rx_hz == 146520000u && hw_mode == RADIO_MODE_FM && !sim.started);
    ptt.arg = 0;
    if (native_lab_request(&sm, &ptt) == 0)         /* as radio_task.c dispatches it */
        radio_sm_request(&sm, &ptt, now);
    run(50);
    CHECK(hw.tx_calls == tx0);

    /* while sweeping, another request ends the search and tunes the channel */
    radio_sm_request(&sm, &r, now);
    ds_poll();
    CHECK(dmrs_on());
    req(RREQ_MONITOR, 1);
    req(RREQ_MONITOR, 0);
    CHECK(!dmrs_on() && hw.rx_hz == 146520000u && hw_mode == RADIO_MODE_FM);
    /* nobody looks (the screen was left): it stops by itself, the channel again */
    radio_sm_request(&sm, &r, now);
    CHECK(dmrs_on());
    hw.st.rssi_dbm = -124;
    run(DMRS_IDLE_MS + 200u);
    CHECK(!dmrs_on() && hw.rx_hz == 146520000u && hw_mode == RADIO_MODE_FM);

    /* on a DMR channel: the stop gives the stack back its channel (color code 2) */
    req(RREQ_ZONE_POS, 3);
    run(50);
    CHECK(sim.started && sim.cc_filter == 2);
    radio_sm_request(&sm, &r, now);
    ds_poll();
    CHECK(dmrs_on() && sim.started && sim.cc_filter == -1 && hw.rx_hz >= 433000000u && hw.rx_hz < 434000000u);
    /* PTT on a DMR channel during the search: still nothing keyed */
    ptt.arg = 1;
    radio_sm_request(&sm, &ptt, now);
    run(300);
    CHECK(!dmrs_on() && hw.tx_calls == tx0 && sim.tx_begins == begins0 && sm.view.state != RS_TX);
    CHECK(sim.started && sim.cc_filter == 2 && hw.rx_hz == 438500000u);
    ptt.arg = 0;
    radio_sm_request(&sm, &ptt, now);
    run(50);
    radio_sm_request(&sm, &r, now);
    r.arg = 0;
    radio_sm_request(&sm, &r, now);   /* the screen's stop */
    CHECK(!dmrs_on() && sim.cc_filter == 2 && hw.rx_hz == 438500000u);
    r.arg = 433000000;

    /* on a P25 channel: the native receiver stays off while searching, PTT goes to the radio core and keys nothing */
    classic_select(false);
    classic_pump();
    classic_pump();
    CHECK(native_lab_owns());                       /* the channel's receiver */
    CHECK(native_lab_request(&sm, &r) == NATIVE_REQUEST_WAIT);  /* it lets go first */
    classic_pump();
    CHECK(native_lab_request(&sm, &r) == 0);
    radio_sm_request(&sm, &r, now);
    CHECK(dmrs_on() && hw_mode == RADIO_MODE_DMR);
    unsigned starts = classic_starts;
    for (int i = 0; i < 10; i++) { ds_poll(); classic_pump(); }
    CHECK(dmrs_on() && !native_lab_owns() && classic_starts == starts);
    ptt.arg = 1;
    CHECK(native_lab_request(&sm, &ptt) == 0);
    radio_sm_request(&sm, &ptt, now);
    CHECK(!dmrs_on() && hw.tx_calls == tx0 && !sm.ptt);
    ptt.arg = 0;
    native_lab_request(&sm, &ptt);
    classic_pump();
    classic_pump();
    CHECK(native_lab_owns());                       /* the P25 channel's receiver again */
    hw.st.rssi_dbm = 0;
    printf("Plus DMR search in the state machine: hear-all stack, sweep, found list, call played, hang, resume, "
           "PTT never keys, stops, P25 channel kept off\n");
}
