/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus multi-watch (docs/plus.md "Multi-watch"): the radio core's ring of 3..6 channels (radio_mwatch.inc) on the
 * real radio state machine, its native P25 receiver and the fake board of radioCoreTest. Included after
 * tests/classic/test_radio.inc (whose main runs first: upstream's radio suite, dual watch included, and the Classic /
 * Plus P25 suites, all with CONFIG_MULTI_WATCH=1, so 2 channels stay dual watch) by tools/test_classic.py.
 * Checks: the order A, B, C...; each quiet stop's dwell; a signal holds its channel, the wait after it looks at the
 * others in turn and comes back, then the ring goes on from the next one; PTT on the selected channel (tuned first)
 * and the ring going on after; a new selection alone doesn't retune; a P25 channel on the FM path and handed to the
 * native receiver; refusals; the stop. Then the ring's period for 2..6 channels (FM, DMR, FM with one P25) against
 * radio_mwatch.h's estimate, printed as a table for docs/plus.md. */
#include "radio/radio_mwatch.h"

static radio_dual_ref_t mw_req_ref[RADIO_DUAL_SLOTS];
static uint8_t mw_req_n, mw_req_sel;
void radio_req_mwatch(const radio_dual_ref_t ref[], uint8_t n, uint8_t sel)
{
    memcpy(mw_req_ref, ref, n * sizeof ref[0]);
    mw_req_n = n;
    mw_req_sel = sel;
    req(RREQ_MWATCH, 0);
}
bool radio_mwatch_take(radio_dual_ref_t ref[], uint8_t *n, uint8_t *sel)
{
    memcpy(ref, mw_req_ref, sizeof mw_req_ref);
    *n = mw_req_n;
    *sel = mw_req_sel;
    return mw_req_n >= 2;
}

/* Zone 0 with six channels (1, 2, 4, 5, 6, 7 of the fixture), all FM or all DMR; p25 > 0: that position is P25 */
static uint32_t mw_hz[6];
static void mw_codeplug(uint8_t mode, int p25)
{
    static const uint16_t numbers[6] = { 1, 2, 4, 5, 6, 7 };
    setup_codeplug();
    zones[0].count = 6;
    for (int i = 0; i < 6; i++) {
        channel_t *c = &chans[numbers[i]];
        zones[0].channels[i] = numbers[i];
        c->mode = mode;
        c->rx_hz = c->tx_hz = 446006250u + 12500u * (uint32_t)i;   /* in band, simplex, no tones */
        c->rx_tone = c->tx_tone = (subtone_t){ TONE_NONE, 0 };
        c->rx_only = false;
        c->color_code = 1;
        c->timeslot = 1;
        mw_hz[i] = c->rx_hz;
        if (i == p25) {
            c->mode = RADIO_MODE_P25;
            c->bw = RADIO_BW_12K5;
            c->p25_nac = 0x293;
            c->p25_tg = 42;
            c->p25_id = 3101234;
        }
    }
}

static void mw_start_radio(void)
{
    start();
    native_lab_init();
    fake_power.switch_on = classic_on;
    fake_power.battery_mv = classic_battery;
    classic_busy = classic_stop = classic_receiving = false;
    classic_starts = classic_restores = 0;
    scan_voice_ms = 0;
    now += 1000;
    radio_sm_poll(&sm, now);
}

static bool mw_watch(uint8_t n, uint8_t sel)
{
    radio_dual_ref_t r[RADIO_DUAL_SLOTS];
    for (uint8_t i = 0; i < n; i++)
        r[i] = (radio_dual_ref_t){ false, 0, i };
    radio_req_mwatch(r, n, sel);
    return sm.scan.kind == SCAN_DUAL && sm.dual_n == n && sm.dual_sel == sel;
}

/* A signal on one frequency only (0: none): the receiver hears it while it is tuned there. As radio_task.c: while the
 * native P25 receiver has the channel, the radio core's poll waits (its maintenance only) */
static uint32_t mw_busy_hz;
static void mw_poll(void)
{
    now += RADIO_POLL_MS;
    signal(mw_busy_hz != 0 && hw.rx_hz == mw_busy_hz ? 0 : 120, true);
    if (native_lab_owns())
        radio_sm_native_maintenance(&sm, now);
    else
        radio_sm_poll(&sm, now);
    classic_pump();
}

/* Polls until the receiver is on channel i (false: not within ms) */
static bool mw_until_at(uint8_t i, uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += RADIO_POLL_MS) {
        if (sm.dual_at == i && sm.scan.kind == SCAN_DUAL)
            return true;
        mw_poll();
    }
    return sm.dual_at == i;
}

/* The ring's period: the mean time between arrivals on channel 0 over `rounds` rounds */
static uint32_t mw_period(int rounds)
{
    uint8_t prev = sm.dual_at;
    uint32_t first = 0, last = 0;
    int seen = 0;
    for (int i = 0; i < 40000 && seen <= rounds; i++) {
        mw_poll();
        if (sm.dual_at != prev) {
            prev = sm.dual_at;
            if (prev == 0) {
                if (seen == 0)
                    first = now;
                last = now;
                seen++;
            }
        }
    }
    return seen > 1 ? (last - first) / (uint32_t)(seen - 1) : 0;
}

static void test_mwatch_order_dwell(void)
{
    mw_codeplug(RADIO_MODE_FM, -1);
    mw_start_radio();
    mw_busy_hz = 0;
    CHECK(mw_watch(4, 0) && sm.dual_at == 0 && hw.rx_hz == mw_hz[0]);
    /* A, B, C, D, A, B: each stop the dwell, the step at the poll after it */
    uint8_t seq[6];
    uint32_t at[6];
    int k = 0;
    uint8_t prev = sm.dual_at;
    uint32_t t0 = now;
    for (int i = 0; i < 400 && k < 6; i++) {
        mw_poll();
        if (sm.dual_at != prev) {
            prev = sm.dual_at;
            seq[k] = prev;
            at[k++] = now;
            CHECK(hw.rx_hz == mw_hz[prev]);
        }
    }
    CHECK(k == 6 && seq[0] == 1 && seq[1] == 2 && seq[2] == 3 && seq[3] == 0 && seq[4] == 1 && seq[5] == 2);
    for (int i = 1; i < k; i++)
        CHECK(at[i] - at[i - 1] == radio_mw_stop_ms());
    CHECK(at[0] - t0 <= radio_mw_stop_ms());
    CHECK(sm.view.scan == SCAN_DUAL && sm.view.dual_sel == 0);
    /* The same ring with C selected: the selection only, no retune now */
    int calls = hw.rx_calls;
    uint8_t here = sm.dual_at;
    CHECK(mw_watch(4, 2) && sm.dual_at == here && hw.rx_calls == calls && sm.view.dual_sel == 2);
    printf("Plus multi-watch: order A B C D, %u ms a stop\n", (unsigned)radio_mw_stop_ms());
}

static void test_mwatch_hold_wait(void)
{
    mw_codeplug(RADIO_MODE_FM, -1);
    mw_start_radio();
    mw_busy_hz = 0;
    CHECK(mw_watch(5, 0));
    /* A signal on C: the ring stops there while it lasts */
    mw_busy_hz = mw_hz[2];
    CHECK(mw_until_at(2, 2000));
    for (int i = 0; i < 80; i++) {   /* 2 s */
        mw_poll();
        CHECK(sm.dual_at == 2 && hw.rx_hz == mw_hz[2]);
    }
    CHECK(sm.scan.phase == SCAN_PH_HOLD);
    /* It ends: the wait for the reply, with looks at the others in turn (each back to C), then the ring goes on at D */
    mw_busy_hz = 0;
    uint8_t looked = 0;
    uint32_t end = now;
    while (now - end < 1500u && !(sm.dual_at == 3 && !sm.dual_peek)) {
        mw_poll();
        if (sm.dual_peek) {
            CHECK(sm.dual_at != 2 && sm.dual_home == 2);
            looked |= (uint8_t)(1u << sm.dual_at);
        }
    }
    CHECK(sm.dual_at == 3 && !sm.dual_peek);
    CHECK(looked != 0 && (looked & (1u << 2)) == 0);
    CHECK(now - end >= sm.set.dual_delay_ms);
    /* A look that finds a signal marks it Busy and doesn't stay: a signal on E during a wait on A */
    mw_busy_hz = mw_hz[0];
    CHECK(mw_until_at(0, 2000));
    for (int i = 0; i < 20; i++)
        mw_poll();
    mw_busy_hz = mw_hz[4];
    uint32_t w0 = now;
    while (now - w0 < 3000u && (sm.view.dual_busy & (1u << 4)) == 0)
        mw_poll();
    CHECK((sm.view.dual_busy & (1u << 4)) != 0);
    /* ... and the ring holds there once the wait ends */
    CHECK(mw_until_at(4, 3000));
    for (int i = 0; i < 20; i++)
        mw_poll();
    CHECK(sm.dual_at == 4 && sm.scan.phase == SCAN_PH_HOLD && (sm.view.dual_busy & (1u << 4)) == 0);
    mw_busy_hz = 0;
    printf("Plus multi-watch: a signal holds, the wait looks at the others in turn, Busy, the ring goes on\n");
}

static void test_mwatch_ptt(void)
{
    mw_codeplug(RADIO_MODE_FM, -1);
    mw_start_radio();
    mw_busy_hz = 0;
    CHECK(mw_watch(4, 3));   /* D selected */
    CHECK(mw_until_at(1, 2000));
    int tx = hw.tx_calls;
    req(RREQ_PTT, 1);
    CHECK(sm.view.state == RS_TX && hw.tx_calls == tx + 1 && hw.tx_hz == mw_hz[3] && sm.dual_at == 3);
    for (int i = 0; i < 40; i++) {
        mw_poll();
        CHECK(sm.dual_at == 3);
    }
    req(RREQ_PTT, 0);
    CHECK(sm.view.state != RS_TX && sm.scan.kind == SCAN_DUAL && sm.dual_n == 4);
    /* The wait after the transmission on D, then the ring goes on from A */
    CHECK(mw_until_at(0, 3000) && sm.scan.kind == SCAN_DUAL);
    CHECK(mw_until_at(2, 1000));
    printf("Plus multi-watch: PTT on the selected channel, the ring goes on after\n");
}

static void test_mwatch_refuse_stop(void)
{
    mw_codeplug(RADIO_MODE_FM, -1);
    mw_start_radio();
    radio_dual_ref_t r[RADIO_DUAL_SLOTS] = { { false, 0, 0 }, { false, 0, 1 }, { false, 0, 1 } };
    radio_req_mwatch(r, 3, 0);
    CHECK(sm.scan.kind != SCAN_DUAL);   /* a channel twice */
    r[2] = (radio_dual_ref_t){ false, 0, 9 };
    radio_req_mwatch(r, 3, 0);
    CHECK(sm.scan.kind != SCAN_DUAL);   /* past the zone */
    r[2] = (radio_dual_ref_t){ false, 1, 0 };   /* another zone's channel is fine */
    radio_req_mwatch(r, 3, 2);   /* C selected: the receiver stays on A, where it is (one of the ring) */
    CHECK(sm.scan.kind == SCAN_DUAL && sm.dual_n == 3 && sm.dual_at == 0 && sm.view.zone_index == 0);
    CHECK(mw_until_at(2, 1000) && sm.view.zone_index == 1 && sm.view.zone_pos == 0);
    /* Back to the pair (dual watch): the third channel leaves the ring */
    radio_req_dual(r, 1);
    CHECK(sm.scan.kind == SCAN_DUAL && sm.dual_n == 2);
    CHECK(mw_until_at(0, 1000) && mw_until_at(1, 1000) && mw_until_at(0, 1000));
    /* The stop: on the selected channel, every channel's changes for now dropped */
    CHECK(mw_watch(5, 4));
    sm.dual_keep[3].valid = true;
    radio_req_dual(NULL, 0);
    CHECK(sm.scan.kind == SCAN_OFF && sm.view.zone_pos == 4 && hw.rx_hz == mw_hz[4] && !sm.dual_keep[3].valid);
    printf("Plus multi-watch: refusals, back to the pair, the stop\n");
}

/* A P25 channel in the ring: listened on the FM path like the others while quiet; a signal there holds the ring and
 * the native receiver takes it; no voice for us: it gives the channel back and the ring goes on after the wait */
static uint32_t test_mwatch_p25(void)
{
    mw_codeplug(RADIO_MODE_FM, 2);
    mw_start_radio();
    mw_busy_hz = 0;
    CHECK(mw_watch(4, 0));
    CHECK(mw_until_at(2, 1000) && !native_lab_owns());
    CHECK(mw_until_at(3, 1000) && classic_starts == 0);
    /* A signal on C (a P25 transmission without voice for us), 2 s long */
    CHECK(mw_until_at(0, 1000));
    mw_busy_hz = mw_hz[2];
    CHECK(mw_until_at(2, 1000));
    uint32_t arrived = now;
    bool took = false;
    while (now - arrived < 10000u && !(sm.dual_at == 3 && !sm.dual_peek)) {	/* the looks in the wait come back to C */
        mw_poll();
        took = took || native_lab_owns();
        if (now - arrived >= 2000u)
            mw_busy_hz = 0;
    }
    uint32_t stayed = now - arrived;
    CHECK(took && sm.dual_at == 3 && !native_lab_owns() && sm.scan.kind == SCAN_DUAL);
    CHECK(stayed >= RADIO_MW_P25_HOLD_MS && stayed <= radio_mw_p25_extra_ms(sm.set.dual_delay_ms) + 1500u);
    printf("Plus multi-watch: P25 channel on the FM path, handed to the native receiver, %u ms with a voiceless signal\n",
           (unsigned)stayed);
    return stayed;
}

static void test_mwatch_period(uint32_t p25_stayed)
{
    static const char *const kinds[3] = { "FM", "DMR", "FM + 1 P25" };
    printf("Plus multi-watch ring period (host model, quiet channels; estimate radio_mw_ring_ms):\n");
    printf("  n   FM      DMR     FM+P25  estimate\n");
    for (uint8_t n = 2; n <= 6; n++) {
        uint32_t ms[3];
        for (int kind = 0; kind < 3; kind++) {
            mw_codeplug(kind == 1 ? RADIO_MODE_DMR : RADIO_MODE_FM, kind == 2 ? n - 1 : -1);
            mw_start_radio();
            mw_busy_hz = 0;
            CHECK(mw_watch(n, 0));
            ms[kind] = mw_period(4);
            if (ms[kind] != radio_mw_ring_ms(n))
                printf("FAIL %s n=%u: %u ms, estimate %u\n", kinds[kind], n, (unsigned)ms[kind], (unsigned)radio_mw_ring_ms(n));
            CHECK(ms[kind] == radio_mw_ring_ms(n));
        }
        printf("  %u   %-6u  %-6u  %-6u  %u ms\n", n, (unsigned)ms[0], (unsigned)ms[1], (unsigned)ms[2],
               (unsigned)radio_mw_ring_ms(n));
    }
    printf("  P25 channel with a voiceless signal: +%u ms measured (estimate +%u ms)\n", (unsigned)p25_stayed,
           (unsigned)radio_mw_p25_extra_ms(RADIO_DUAL_DELAY_MS));
}

int main(void)
{
    int prior = classic_radio_main();
    int before = failures;
    test_mwatch_order_dwell();
    test_mwatch_hold_wait();
    test_mwatch_ptt();
    test_mwatch_refuse_stop();
    uint32_t stayed = test_mwatch_p25();
    test_mwatch_period(stayed);
    printf("Plus multi-watch radio core: %d failures\n", failures - before);
    return prior || failures != before ? 1 : 0;
}
