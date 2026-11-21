/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus air watch (CONFIG_AIR_WATCH, item 44b, docs/plus.md "Round 14: air band and spectrum"), the radio core's
 * part (radio_airwatch.inc), on the real radio state machine, its native P25 receiver and the fake board of
 * radioCoreTest (an AM receiver, the 108-136 MHz receive only band). Included by tools/test_air.py after
 * tests/classic/test_radio.inc, tests/plus/test_mwatch_radio.inc and tests/plus/test_vfox_radio.inc, whose mains run
 * first with it built in. Then: an airfield's four frequencies on the letters' own VFOs and the ring over them (each
 * tuned in AM, its own frequency, TX refused), the AM squelch per letter (a signal holds the ring on its letter, the
 * ring goes on after it), nothing noted while it runs, the end (VFOs A / B and the position back from the settings,
 * C..F's VFOs empty), a power off during it (back as before it), the user's own ring kept on its selected channel,
 * and the refusals. */

static void air_req(uint8_t type, int32_t arg, uint32_t arg2)
{
    radio_req_t r = { type, arg, arg2 };
    for (int i = 0; i < 20; i++) {
        int h = native_lab_request(&sm, &r);
        if (h == NATIVE_REQUEST_WAIT) { classic_pump(); continue; }
        if (!h) radio_sm_request(&sm, &r, now);
        return;
    }
    CHECK(!"the native receiver never let go");
}
void radio_req_air_vfo(uint8_t idx, uint32_t hz) { air_req(RREQ_AIR_VFO, idx, hz); }
void radio_req_air_end(void) { air_req(RREQ_AIR_END, 0, 0); }

/* mw_poll for AM letters: a signal on air_busy_hz while the board's AM receiver is tuned there (the fake board's AM
 * receiver leaves hw.rx_hz alone), at -60 dBm: the AM squelch reads the strength, not the noise */
static uint32_t air_busy_hz;
static void air_poll(void)
{
    now += RADIO_POLL_MS;
    bool on = air_busy_hz != 0 && sm.view.am && vx_am_hz == air_busy_hz;
    signal(on ? 0 : 120, true);
    hw.st.rssi_dbm = on ? -60 : -125;	/* the default AM squelch (level 5) opens above -70 dBm */
    if (native_lab_owns())
        radio_sm_native_maintenance(&sm, now);
    else
        radio_sm_poll(&sm, now);
    classic_pump();
}

static const uint32_t air_hz[4] = { 118100000, 121900000, 123600000, 119100000 };	/* TWR, GND, ATIS, APP */

/* The user's state: VFO A at 145.500 MHz, then back on the zone's third channel; noted (a few polls) */
static void air_user(void)
{
    mw_codeplug(RADIO_MODE_FM, -1);
    mw_start_radio();
    fake_radio.rx_am = vx_rx_am;
    vx_request(RREQ_VFO, 1);
    vx_request(RREQ_VFO_SET, 145500000);
    vx_request(RREQ_VFO, 0);
    req(RREQ_ZONE_POS, 2);
    for (int i = 0; i < 40; i++)
        mw_poll();
    settings_t st;
    settings_read(&st);
    CHECK(!st.vfo && st.zone_pos == 2 && st.vfo_rx_hz == 145500000 && sm.view.ch.rx_hz == mw_hz[2]);
}

/* The air watch as the UI asks for it: the VFOs, then the ring of their letters */
static void air_start(uint8_t n)
{
    radio_dual_ref_t r[RADIO_DUAL_SLOTS];
    for (uint8_t k = 0; k < n; k++) {
        radio_req_air_vfo(k, air_hz[k]);
        r[k] = (radio_dual_ref_t){ true, 0, k };
    }
    radio_req_mwatch(r, n, 0);
}

/* What the user had: noted, untouched */
static bool air_noted_as_before(void)
{
    settings_t st;
    settings_read(&st);
    return !st.vfo && st.zone_pos == 2 && st.vfo_rx_hz == 145500000 && st.vfo_index == 0;
}

static void test_air_ring(void)
{
    air_user();
    air_start(4);
    CHECK(sm.air && sm.scan.kind == SCAN_DUAL && sm.dual_n == 4 && sm.dual_sel == 0);
    /* The ring visits each letter on its own frequency, in AM, and none transmits */
    unsigned seen = 0, am = 0;
    for (int i = 0; i < 400 && seen != 0xFu; i++) {
        mw_poll();
        if (sm.scan.kind == SCAN_DUAL && sm.view.vfo && sm.dual_at < 4) {
            uint8_t k = sm.dual_at;
            CHECK(sm.view.ch.rx_hz == air_hz[k] && sm.view.ch.mode == RADIO_MODE_FM && sm.view.am);
            CHECK(radio_sm_tx_check(&sm) == TXDENY_AM);
            am += vx_am_hz == air_hz[k];
            seen |= 1u << k;
        }
    }
    CHECK(seen == 0xFu && am > 0);
    /* A signal on C (123.600): the AM squelch opens on its strength and holds the ring there */
    air_busy_hz = air_hz[2];
    bool held = false;
    for (int i = 0; i < 400 && !held; i++) {
        air_poll();
        held = sm.dual_at == 2 && sm.view.state == RS_RX;
    }
    CHECK(held);
    unsigned stayed = 0;
    for (int i = 0; i < 80; i++) {	/* 2 s: it stays, receiving */
        air_poll();
        stayed += sm.dual_at == 2 && sm.view.state == RS_RX;
    }
    CHECK(stayed == 80);
    air_busy_hz = 0;
    bool moved = false;
    for (int i = 0; i < 800 && !moved; i++) {
        air_poll();
        moved = sm.dual_at != 2 && sm.scan.kind == SCAN_DUAL;
    }
    CHECK(moved);
    /* Nothing noted while it runs (the polls above, and a request that would note) */
    vx_request(RREQ_DMR_FOCUS, 0);
    CHECK(air_noted_as_before());
    /* The end: the ring stops, the radio back on the third channel, VFO A as it was, C..F's VFOs empty */
    radio_req_air_end();
    CHECK(!sm.air && sm.scan.kind == SCAN_OFF && !sm.view.vfo && sm.view.zone_pos == 2 && !sm.view.am);
    CHECK(sm.view.ch.rx_hz == mw_hz[2] && hw.rx_hz == mw_hz[2]);
    CHECK(sm.vfo_idx == 0 && sm.vfox_at == 0 && sm.vfo_ch.rx_hz == 145500000 && sm.vfo_ch.mode == RADIO_MODE_FM);
    static const vfox_slot_t empty;
    for (int k = 0; k < 4; k++)
        CHECK(memcmp(&sm.vfox_x[k], &empty, sizeof empty) == 0);
    for (int i = 0; i < 40; i++)
        mw_poll();
    CHECK(air_noted_as_before());
    /* Again, ended by a power off: back as before it too */
    air_start(3);
    CHECK(sm.air && sm.dual_n == 3);
    for (int i = 0; i < 40; i++)
        mw_poll();
    boot();
    CHECK(!sm.air && !sm.view.vfo && sm.view.zone_pos == 2 && sm.vfo_ch.rx_hz == 145500000);
    printf("Plus air watch: four air frequencies on the letters' VFOs, the ring in AM, a signal holds its letter, "
           "nothing noted, the end and a power off back as before\n");
}

/* The user's own ring (B selected, the receiver elsewhere): stopped on B, noted there, back there at the end */
static void test_air_user_ring(void)
{
    air_user();
    CHECK(mw_watch(4, 1));
    for (int i = 0; i < 100 && sm.dual_at != 3; i++)
        mw_poll();
    CHECK(sm.dual_at == 3);
    air_start(2);	/* two frequencies: the pair */
    settings_t st;
    settings_read(&st);
    CHECK(sm.air && !st.vfo && st.zone_pos == 1 && sm.scan.kind == SCAN_DUAL && sm.dual_n == 2);
    radio_req_air_end();
    CHECK(!sm.air && sm.scan.kind == SCAN_OFF && !sm.view.vfo && sm.view.zone_pos == 1);
    CHECK(mw_watch(4, 1) && sm.dual_at == 1);	/* the UI asks for the user's ring again: on B */
    printf("Plus air watch: the user's ring kept on its selected channel\n");
}

/* Refusals: while transmitting, off the air band, a seventh VFO; an end without an air watch changes nothing */
static void test_air_refused(void)
{
    air_user();
    radio_req_air_vfo(0, 433500000);
    radio_req_air_vfo(6, 118100000);
    CHECK(!sm.air && sm.vfo_ch.rx_hz == 145500000);
    radio_req_air_end();
    CHECK(!sm.view.vfo && sm.view.zone_pos == 2);
    sm.view.state = RS_TX;
    radio_req_air_vfo(0, 118100000);
    CHECK(!sm.air);
    sm.view.state = RS_IDLE;
    printf("Plus air watch: refused while transmitting, off the air band, past F\n");
}

int main(void)
{
    int prior = vfox_radio_main();
    int before = failures;
    test_air_ring();
    test_air_user_ring();
    test_air_refused();
    printf("Plus air watch, radio core: %d failures\n", failures - before);
    return prior || failures != before ? 1 : 0;
}
