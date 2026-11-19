/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus priority channel and temporary skip (docs/plus.md "Priority channel and temporary skip"): the radio core's
 * part (radio_scanprio.inc) on the real radio state machine, its native P25 receiver and the fake board of
 * radioCoreTest. Included after tests/classic/test_radio.inc and tests/plus/test_mwatch_radio.inc (whose mains run
 * first: upstream's radio suite, the Classic / Plus P25 suites, the zone scan model and multi-watch's, all with
 * CONFIG_SCAN_PRIO=1 and no priority channel set, so upstream's scan stays as it was) by tools/test_classic.py.
 * Checks: the priority channel's stops every SCANPRIO_EVERY_MS while a zone scan steps, in another zone too; one of
 * the zone scanned gets no fewer stops; a signal there holds it as a scan stop; the looks between overs (one dwell, back
 * to the wait, the wait not made longer), a look that finds a signal, none during a call; dual watch and multi-watch
 * (a channel outside the ring, one inside it that a signal takes over the wait); none while transmitting, PTT during
 * a look talks back on the wait's channel; nuisance delete and Clear skips, the priority channel's too, skips gone
 * when the scan stops; a P25 priority channel (the zone scan model of test_zscan.inc: calls caught), a skip while the
 * native receiver holds a P25 channel. */
#include "radio/scanprio.h"

/* mw_codeplug's zone 0 (channels 1, 2, 4, 5, 6, 7, all FM) and channel 8, FM too, only in zone 2 ("Tones": 7, 8) */
static uint32_t scx_p_hz;
static void scx_codeplug(void)
{
    mw_codeplug(RADIO_MODE_FM, -1);
    channel_t *c = &chans[8];
    c->rx_hz = c->tx_hz = 446106250u;
    c->rx_tone = c->tx_tone = (subtone_t){ TONE_NONE, 0 };
    c->bw = RADIO_BW_12K5;
    scx_p_hz = c->rx_hz;
}

static void scx_start(uint32_t word)
{
    scx_codeplug();
    mw_start_radio();
    radio_scan_prio_set(word);   /* after start(): the radio core read the (empty) settings there */
    mw_busy_hz = 0;
}

/* Polls ms, counting the stops on hz (a retune onto it) and the longest and shortest time between two */
typedef struct { unsigned stops; uint32_t first, last, gap_min, gap_max; } scx_seen_t;
static void scx_watch(uint32_t hz, uint32_t ms, scx_seen_t *s)
{
    bool was = hw.rx_hz == hz;
    for (uint32_t t = 0; t < ms; t += RADIO_POLL_MS) {
        mw_poll();
        bool on = hw.rx_hz == hz;
        if (on && !was) {
            if (s->stops == 0) {
                s->first = now;
                s->gap_min = 0xFFFFFFFFu;
            } else {
                uint32_t gap = now - s->last;
                s->gap_min = gap < s->gap_min ? gap : s->gap_min;
                s->gap_max = gap > s->gap_max ? gap : s->gap_max;
            }
            s->stops++;
            s->last = now;
        }
        was = on;
    }
}

static bool scx_until(bool (*cond)(void), uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += RADIO_POLL_MS) {
        if (cond())
            return true;
        mw_poll();
    }
    return cond();
}
static bool scx_on_p(void) { return hw.rx_hz == scx_p_hz; }
static bool scx_held_p(void) { return hw.rx_hz == scx_p_hz && sm.scan.phase == SCAN_PH_HOLD; }
static uint32_t scx_w_hz;
static bool scx_held_w(void) { return hw.rx_hz == scx_w_hz && sm.scan.phase == SCAN_PH_HOLD; }
static bool scx_on_w(void) { return hw.rx_hz == scx_w_hz; }

/* The setting's word: what reads as a channel and what doesn't */
static void test_scx_word(void)
{
    uint16_t n, z;
    CHECK(scanprio_unpack(scanprio_pack(8, 2), &n, &z) && n == 8 && z == 2);
    CHECK(scanprio_unpack(scanprio_pack(1024, 67), &n, &z) && n == 1024 && z == 67);
    CHECK(scanprio_pack(0, 0) == 0 && scanprio_number(0) == 0);
    CHECK(scanprio_number(1234567u) == 0);            /* an old own-ID override (radioCoreTest writes this one) */
    CHECK(scanprio_number(16776415u) == 0);           /* the largest ID */
    CHECK(scanprio_number(0x50000000u) == 0);         /* the tag without a channel */
    CHECK(scanprio_number(scanprio_pack(8, 2) | 0x4000u) == 0 && scanprio_number(scanprio_pack(8, 2) | 0x00800000u) == 0);
    CHECK(scanprio_number(scanprio_pack(8, 2) | SCANPRIO_VFOX_BITS) == 8);   /* VFO A's DMR Any flags share the word */
    /* The settings record carries it across a power cycle, as the UI saves it (ui_svc_storage.c) */
    scx_codeplug();
    start();
    settings_t *st = settings_begin();
    st->unused_dmr_id = scanprio_pack(8, 2);
    settings_end(now);
    boot();
    req(RREQ_SCAN, RSCAN_ZONE);
    CHECK(sm.scan.kind == SCAN_ZONE && hw.rx_hz == scx_p_hz && sm.view.zone_index == 2);
    printf("Plus priority: the setting's word, kept in the settings record\n");
}

/* Zone scan, nothing heard: the priority channel (zone 2) first, then every SCANPRIO_EVERY_MS; the zone scanned (6
 * channels, once round in 0.9 s) keeps its order, and is back in use after each priority stop */
static void test_scx_intervals(void)
{
    scx_start(scanprio_pack(8, 2));
    req(RREQ_SCAN, RSCAN_ZONE);
    CHECK(sm.scan.kind == SCAN_ZONE && hw.rx_hz == scx_p_hz && sm.view.zone_index == 2 && sm.view.zone_pos == 1);
    scx_seen_t s = { 0 };
    uint8_t seen = 0;
    int order_errors = 0, prev = 0;
    bool was_p = true;
    for (uint32_t t = 0; t < 12000u; t += RADIO_POLL_MS) {
        mw_poll();
        bool on = hw.rx_hz == scx_p_hz;
        if (on && !was_p) {
            uint32_t gap = s.stops ? now - s.last : 0;
            if (s.stops) {
                s.gap_min = gap < s.gap_min || s.stops == 1 ? gap : s.gap_min;
                s.gap_max = gap > s.gap_max ? gap : s.gap_max;
            }
            s.stops++;
            s.last = now;
        }
        if (!on && sm.view.zone_index == 0 && sm.view.zone_pos != prev) {
            if (sm.view.zone_pos != (prev + 1) % 6)
                order_errors++;
            prev = sm.view.zone_pos;
            seen |= (uint8_t)(1u << prev);
        }
        CHECK(on || sm.view.zone_index == 0);
        was_p = on;
    }
    CHECK(s.stops >= 4 && s.gap_min >= SCANPRIO_EVERY_MS && s.gap_max <= SCANPRIO_EVERY_MS + 400u);
    CHECK(seen == 0x3F && order_errors <= 1);   /* (the first change after the first priority stop may be any) */
    printf("Plus priority: zone scan, a channel of another zone every %u-%u ms (%u stops in 12 s), the sweep in order\n",
           (unsigned)s.gap_min, (unsigned)s.gap_max, s.stops);

    /* One of the zone scanned: its own turn each round (0.9 s), never fewer stops */
    scx_start(scanprio_pack(5, 0));   /* position 3 */
    req(RREQ_SCAN, RSCAN_ZONE);
    scx_seen_t z = { 0 };
    scx_watch(chans[5].rx_hz, 6000u, &z);
    CHECK(z.stops >= 6 && z.gap_max <= 1000u);
    /* Off: upstream's scan exactly (no extra stops anywhere) */
    scx_start(0);
    req(RREQ_SCAN, RSCAN_ZONE);
    scx_seen_t o = { 0 };
    scx_watch(scx_p_hz, 6000u, &o);
    CHECK(o.stops == 0);
    printf("Plus priority: one of the zone scanned keeps its turn (%u-%u ms); Off adds nothing\n",
           (unsigned)z.gap_min, (unsigned)z.gap_max);
}

/* A signal on the priority channel while the scan steps: held as a scan stop, the wait after it, then the sweep goes
 * on in the zone scanned */
static void test_scx_hit(void)
{
    scx_start(scanprio_pack(8, 2));
    req(RREQ_SCAN, RSCAN_ZONE);
    for (int i = 0; i < 40; i++)   /* 1 s: past the first priority stop */
        mw_poll();
    CHECK(!scx_on_p());
    mw_busy_hz = scx_p_hz;
    uint32_t t0 = now;
    CHECK(scx_until(scx_held_p, 2500u));
    uint32_t found = now - t0;
    CHECK(sm.view.channel_number == 8 && sm.view.zone_index == 2 && hw.audio);
    for (int i = 0; i < 120; i++) {   /* 3 s: it stays while the signal does */
        mw_poll();
        CHECK(scx_on_p());
    }
    mw_busy_hz = 0;
    uint32_t end = now;
    while (now - end < 4000u && scx_on_p())
        mw_poll();
    CHECK(!scx_on_p() && sm.view.zone_index == 0 && sm.scan.kind == SCAN_ZONE);
    CHECK(now - end >= sm.set.scan_delay_ms && now - end <= sm.set.scan_delay_ms + 200u);
    printf("Plus priority: a signal there found in %u ms, held, the scan goes on %u ms after it ends\n",
           (unsigned)found, (unsigned)(now - end));
}

/* Between overs: a call on another channel is never left; in the wait after it, a look at the priority channel (one
 * dwell), back to the wait, which ends when it would have; a look that finds a signal stays; a reply after a look */
static void test_scx_between_overs(void)
{
    scx_start(scanprio_pack(8, 2));
    req(RREQ_SCAN, RSCAN_ZONE);
    for (int i = 0; i < 40; i++)
        mw_poll();
    scx_w_hz = mw_hz[2];
    mw_busy_hz = scx_w_hz;
    CHECK(scx_until(scx_held_w, 2000u));
    for (int i = 0; i < 160; i++) {   /* 4 s of the call: no look */
        mw_poll();
        CHECK(scx_on_w() && sm.scan.phase == SCAN_PH_HOLD);
    }
    /* The call ends: the wait (2 s), a look early in it, back to the wait */
    mw_busy_hz = 0;
    uint32_t end = now;
    CHECK(scx_until(scx_on_p, 600u));
    uint32_t look = now - end;
    CHECK(sm.scan.kind == SCAN_ZONE && sm.view.zone_index == 2);
    CHECK(scx_until(scx_on_w, 400u));
    uint32_t looked = now - end - look;
    CHECK(sm.scan.phase == SCAN_PH_RESUME && sm.view.zone_index == 0 && sm.view.zone_pos == 2);
    while (now - end < 4000u && scx_on_w())
        mw_poll();
    uint32_t waited = now - end;
    CHECK(waited >= sm.set.scan_delay_ms && waited <= sm.set.scan_delay_ms + 100u);
    printf("Plus priority: between overs a look %u ms into the wait, %u ms long; the wait ends at %u ms\n",
           (unsigned)look, (unsigned)looked, (unsigned)waited);

    /* A signal on the priority channel when the look comes: it stays there */
    mw_busy_hz = scx_w_hz;
    CHECK(scx_until(scx_held_w, 3000u));
    for (int i = 0; i < 100; i++)
        mw_poll();
    mw_busy_hz = scx_p_hz;
    CHECK(scx_until(scx_held_p, 600u));
    for (int i = 0; i < 40; i++) {
        mw_poll();
        CHECK(scx_on_p());
    }
    /* The reply on the wait's channel after a look: held there */
    mw_busy_hz = scx_w_hz;
    CHECK(scx_until(scx_held_w, 4000u));
    for (int i = 0; i < 100; i++)
        mw_poll();
    mw_busy_hz = 0;
    CHECK(scx_until(scx_on_p, 600u));   /* the look */
    mw_busy_hz = scx_w_hz;              /* the reply starts during it */
    CHECK(scx_until(scx_held_w, 600u));
    CHECK(sm.view.zone_pos == 2 && sm.view.zone_index == 0);
    mw_busy_hz = 0;
    printf("Plus priority: a look that finds a signal stays; a reply after a look is held\n");

    /* PTT during a look: back on the wait's channel, the scan stops there and PTT talks on it */
    scx_start(scanprio_pack(8, 2));
    req(RREQ_SCAN, RSCAN_ZONE);
    for (int i = 0; i < 40; i++)
        mw_poll();
    mw_busy_hz = scx_w_hz;
    CHECK(scx_until(scx_held_w, 2000u));
    for (int i = 0; i < 100; i++)
        mw_poll();
    mw_busy_hz = 0;
    CHECK(scx_until(scx_on_p, 600u));
    int tx = hw.tx_calls;
    req(RREQ_PTT, 1);
    CHECK(sm.scan.kind == SCAN_OFF && sm.view.state == RS_TX && hw.tx_calls == tx + 1 && hw.tx_hz == scx_w_hz &&
          sm.view.zone_index == 0 && sm.view.zone_pos == 2);
    req(RREQ_PTT, 0);
    printf("Plus priority: PTT during a look talks on the wait's channel\n");
}

/* Dual watch / multi-watch: a channel outside the ring every 2 s, the ring's order kept; a signal there holds the
 * watch, its wait has no looks at the ring, then the ring goes on; a priority channel of the ring takes over the wait
 * on another letter (Off: only Busy); never while transmitting */
static void test_scx_watch(void)
{
    scx_start(scanprio_pack(8, 2));
    CHECK(mw_watch(3, 0));
    uint8_t prev = sm.dual_at;
    int order_errors = 0;
    scx_seen_t s = { 0 };
    bool was = false;
    for (uint32_t t = 0; t < 10000u; t += RADIO_POLL_MS) {
        mw_poll();
        bool on = scx_on_p();
        if (on && !was) {
            if (s.stops) {
                uint32_t gap = now - s.last;
                s.gap_min = s.stops == 1 || gap < s.gap_min ? gap : s.gap_min;
                s.gap_max = gap > s.gap_max ? gap : s.gap_max;
            }
            s.stops++;
            s.last = now;
        }
        if (!on && sm.dual_at != prev) {
            if (sm.dual_at != (prev + 1) % 3)
                order_errors++;
            prev = sm.dual_at;
        }
        was = on;
    }
    CHECK(s.stops >= 4 && s.gap_min >= SCANPRIO_EVERY_MS && s.gap_max <= SCANPRIO_EVERY_MS + 400u && order_errors == 0);
    CHECK(sm.scan.kind == SCAN_DUAL && sm.dual_n == 3);
    printf("Plus priority: multi-watch of 3, a channel outside the ring every %u-%u ms, the ring in order\n",
           (unsigned)s.gap_min, (unsigned)s.gap_max);

    /* A signal there: the watch holds it, no ring looks in its wait, then the ring goes on */
    mw_busy_hz = scx_p_hz;
    CHECK(scx_until(scx_held_p, 2500u));
    uint8_t at = sm.dual_at;
    for (int i = 0; i < 80; i++) {
        mw_poll();
        CHECK(scx_on_p() && !sm.dual_peek);
    }
    mw_busy_hz = 0;
    uint32_t end = now;
    while (now - end < 3000u && scx_on_p()) {
        mw_poll();
        CHECK(!sm.dual_peek);
    }
    CHECK(!scx_on_p() && sm.dual_at == (at + 1) % 3 && now - end >= sm.set.dual_delay_ms);
    printf("Plus priority: multi-watch holds the priority channel, the ring goes on %u ms after\n", (unsigned)(now - end));

    /* A priority channel of the ring (D) takes over the wait on B; Off: the look only marks D Busy */
    for (int off = 0; off < 2; off++) {
        scx_start(off ? 0 : scanprio_pack(5, 0));   /* channel 5: position 3, D */
        CHECK(mw_watch(4, 0));
        mw_busy_hz = mw_hz[1];
        CHECK(mw_until_at(1, 2000));
        for (int i = 0; i < 120; i++)
            mw_poll();
        CHECK(sm.dual_at == 1 && sm.scan.phase == SCAN_PH_HOLD);
        mw_busy_hz = mw_hz[3];
        uint32_t e = now;
        while (now - e < 600u && !(sm.dual_at == 3 && sm.scan.phase == SCAN_PH_HOLD))
            mw_poll();
        if (!off) {
            CHECK(sm.dual_at == 3 && sm.scan.phase == SCAN_PH_HOLD);
        } else {
            CHECK(!(sm.dual_at == 3 && sm.scan.phase == SCAN_PH_HOLD));
            CHECK((sm.view.dual_busy & (1u << 3)) != 0 || sm.dual_peek || sm.dual_at == 1);
        }
        mw_busy_hz = 0;
    }
    printf("Plus priority: a priority letter takes over the wait on another (Off: Busy only)\n");

    /* Never while transmitting: PTT on the selected one for 4 s, no retune at all */
    scx_start(scanprio_pack(8, 2));
    CHECK(mw_watch(3, 0));
    for (int i = 0; i < 100; i++)
        mw_poll();
    req(RREQ_PTT, 1);
    CHECK(sm.view.state == RS_TX);
    int rx_calls = hw.rx_calls;
    for (int i = 0; i < 160; i++) {
        mw_poll();
        CHECK(sm.view.state == RS_TX && !scx_on_p());
    }
    CHECK(hw.rx_calls == rx_calls);
    req(RREQ_PTT, 0);
    CHECK(sm.scan.kind == SCAN_DUAL);
    printf("Plus priority: no look while transmitting\n");
}

/* Nuisance delete and Clear skips; the priority channel's own skip; skips go with the scan */
static void test_scx_skip(void)
{
    scx_start(0);
    req(RREQ_SCAN, RSCAN_ZONE);
    scx_w_hz = mw_hz[1];
    mw_busy_hz = scx_w_hz;
    CHECK(scx_until(scx_held_w, 2000u));
    req(RREQ_SCAN, RSCAN_NUISANCE);
    CHECK(sm.scan.skipped == 1 && sm.view.scan_skipped == 1 && !scx_on_w());
    scx_seen_t a = { 0 };
    scx_watch(scx_w_hz, 4000u, &a);
    CHECK(a.stops == 0);
    mw_busy_hz = 0;
    req(RREQ_SCAN, RSCAN_UNSKIP);
    CHECK(sm.scan.skipped == 0 && sm.view.scan_skipped == 0 && sm.scan.kind == SCAN_ZONE);
    scx_seen_t b = { 0 };
    scx_watch(scx_w_hz, 2000u, &b);
    CHECK(b.stops >= 2);
    /* Skipped, then the scan stops and starts again: nothing skipped any more */
    mw_busy_hz = scx_w_hz;
    CHECK(scx_until(scx_held_w, 2000u));
    req(RREQ_SCAN, RSCAN_NUISANCE);
    mw_busy_hz = 0;
    CHECK(sm.scan.skipped == 1);
    req(RREQ_SCAN, RSCAN_STOP);
    CHECK(sm.scan.kind == SCAN_OFF && sm.view.scan_skipped == 0);
    req(RREQ_SCAN, RSCAN_ZONE);
    CHECK(sm.scan.skipped == 0);
    scx_seen_t c = { 0 };
    scx_watch(scx_w_hz, 2000u, &c);
    CHECK(c.stops >= 2);
    printf("Plus skip: nuisance delete, Clear skips, skips gone when the scan stops\n");

    /* The priority channel skipped (held on it): no more priority stops; Clear skips brings them back */
    scx_start(scanprio_pack(8, 2));
    req(RREQ_SCAN, RSCAN_ZONE);
    mw_busy_hz = scx_p_hz;
    CHECK(scx_until(scx_held_p, 2500u));
    req(RREQ_SCAN, RSCAN_NUISANCE);
    CHECK(!scx_on_p() && sm.view.zone_index == 0 && sm.scan.skipped == 1 && sm.scan.kind == SCAN_ZONE);
    scx_seen_t p = { 0 };
    scx_watch(scx_p_hz, 6000u, &p);
    CHECK(p.stops == 0);
    req(RREQ_SCAN, RSCAN_UNSKIP);
    CHECK(sm.scan.skipped == 0);
    CHECK(scx_until(scx_held_p, 2500u));
    /* ... and a stop and a new scan: the priority channel first again */
    req(RREQ_SCAN, RSCAN_NUISANCE);
    req(RREQ_SCAN, RSCAN_STOP);
    req(RREQ_SCAN, RSCAN_ZONE);
    CHECK(sm.scan.kind == SCAN_ZONE && scx_on_p());
    mw_busy_hz = 0;
    /* Clear skips with nothing to clear, or no scan: harmless */
    req(RREQ_SCAN, RSCAN_STOP);
    req(RREQ_SCAN, RSCAN_UNSKIP);
    CHECK(sm.scan.kind == SCAN_OFF);
    printf("Plus skip: the priority channel's own skip, cleared; a new scan starts on it again\n");
}

/* A request while the native P25 receiver has the channel: handed back first, as the radio task does */
static void scx_native_req(uint8_t type, int32_t arg)
{
    radio_req_t r = { type, arg, 0 };
    for (int i = 0; i < 100; i++) {
        int got = native_lab_request(&sm, &r);
        if (got == 0) {
            radio_sm_request(&sm, &r, now);
            return;
        }
        if (got == 1)
            return;
        zs_step();
    }
    CHECK(false);
}

/* P25: a priority P25 channel in a zone of 16 (once round in 2.4 s): 2 s calls caught, against no priority; a skip
 * while the native receiver holds it */
static void test_scx_p25(void)
{
    static const uint8_t sizes[2] = { 16, 32 };
    unsigned pct[2][2];
    for (int n = 0; n < 2; n++) {
        for (int k = 0; k < 2; k++) {
            zs_start(sizes[n], (uint8_t)(sizes[n] - 1u), ZS_RF_NOISE);   /* the P25 channel (number 8) last */
            if (k == 1)
                radio_scan_prio_set(scanprio_pack(8, 0));   /* set while the scan runs: found at once */
            zs_rate_t r = zs_traffic(60, 2000u, 12000u, 20000u);
            pct[n][k] = zs_percent(&r);
        }
        printf("Plus priority: P25 channel in a zone of %u (%u.%u s round), 2 s calls caught %u %% (without priority %u %%)\n",
               sizes[n], sizes[n] * 150u / 1000u, sizes[n] * 150u % 1000u / 100u, pct[n][1], pct[n][0]);
    }
    CHECK(pct[0][1] + 5u >= pct[0][0] && pct[1][1] >= pct[1][0] + 15u);

    /* A skip while the native receiver holds the P25 channel: given back, the scan moves on and leaves it alone */
    zs_start(8, 3, ZS_RF_NOISE);
    zs_call(now + ZS_STEP_MS, 8000u, true);
    for (int i = 0; i < 1000 && !native_lab_owns(); i++)
        zs_step();
    CHECK(native_lab_owns() && sm.view.zone_pos == 3);
    scx_native_req(RREQ_SCAN, RSCAN_NUISANCE);
    for (int i = 0; i < 200 && native_lab_owns(); i++)
        zs_step();
    CHECK(!native_lab_owns() && sm.scan.kind == SCAN_ZONE && sm.scan.skipped == 1 && sm.view.zone_pos != 3);
    bool took = false;
    for (int i = 0; i < 1000; i++) {   /* 5 s more of the call */
        zs_step();
        took = took || native_lab_owns() || sm.view.zone_pos == 3;
    }
    CHECK(!took);
    zs.t0 = 0;
    printf("Plus skip: a P25 channel skipped while its receiver held it, not taken again\n");
}

int main(void)
{
    int prior = mwatch_radio_main();
    int before = failures;
    test_scx_word();
    test_scx_intervals();
    test_scx_hit();
    test_scx_between_overs();
    test_scx_watch();
    test_scx_skip();
    test_scx_p25();
    printf("Plus priority channel and temporary skip, radio core: %d failures\n", failures - before);
    return prior || failures != before ? 1 : 0;
}
