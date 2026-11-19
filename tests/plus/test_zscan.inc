/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus, P25 in a zone scan (docs/plus.md "P25 in a zone scan"): how often a call on the zone's P25 channel is
 * heard while the zone is scanned. The real radio state machine (its scan) and the real native controller
 * (native_lab.c take / give back) run on the fake board; the transmitter, the receiver's readings and the P25 worker
 * are models of what the code and upstream's notes say about them:
 * - the call: HDU (82 ms), then LDU1, LDU2, ... (180 ms each), a TDU at the end;
 * - the FM path's readings on the P25 frequency while the call is on (three receivers): the noise reading follows the
 *   carrier; it stays at its idle value and only the strength rises (upstream gotchas 17.3: seen on this chip with
 *   DMR, another 4FSK signal); or it hovers around the squelch line;
 * - the worker (p25_lab.c): receiving ZS_START_MS after the native receiver starts; a frame is decoded at its end when
 *   the receiver was there from its start; voice plays from the second LDU on (the talkgroup comes with LDU1, clear
 *   voice with LDU2, an HDU brings both: voice.c); 0.5 s after the last valid frame it starts over and reports no
 *   voice until it hears some again. ZS_START_MS is a guess (not measured on the radio; the debug link's sync_ms
 *   counter gives the real figure).
 * Included by tests/classic/test_scan.inc (tools/test_classic.py: test_radio_scan and test_radio_mwatch). The same
 * file builds against the code before the change (ZSCAN_BEFORE: no frame times there) for the "before" numbers. */

enum { ZS_START_MS = 60, ZS_HDU_MS = 82, ZS_LDU_MS = 180, ZS_TDU_MS = 15, ZS_IDLE_MS = 500, ZS_STEP_MS = 5 };
enum { ZS_RF_NOISE, ZS_RF_DEAF, ZS_RF_MARGINAL, ZS_RF_KINDS };

static struct {
    uint8_t rf;
    uint8_t p25_pos;
    uint32_t hz;
    uint32_t rand;
    /* the transmission on the air, t0 = 0: none */
    uint32_t t0, len;
    bool frames;            /* false: a carrier without P25 frames of ours (analog, another NAC) */
    uint32_t frame;         /* the next frame to end: 0 the HDU, then LDUs, the TDU last */
    /* the worker's receiver */
    uint32_t sid, listen_ms;    /* its session and since when it receives (0: not) */
    uint32_t valid_ms;
    bool records, have_group, clear;
    /* what was heard of the transmission */
    uint32_t heard_ms, first_voice_ms;
} zs;
static uint32_t zs_sync_ms;
#ifndef ZSCAN_BEFORE
uint32_t p25_lab_channel_last_sync_ms(void) { return zs_sync_ms; }
#endif

static uint32_t zs_random(uint32_t n)
{
    zs.rand = zs.rand * 1664525u + 1013904223u;
    return (zs.rand >> 8) % n;
}

/* Frame k of the transmission: when it starts and ends; false past the TDU. 0: the HDU; the last one: the TDU */
static bool zs_frame(uint32_t k, uint32_t *from, uint32_t *to, uint8_t *kind)
{
    uint32_t ldus = (zs.len - ZS_HDU_MS - ZS_TDU_MS) / ZS_LDU_MS;
    if (k > ldus + 1u)
        return false;
    if (k == 0) {
        *from = zs.t0;
        *to = zs.t0 + ZS_HDU_MS;
        *kind = 0;
    } else if (k <= ldus) {
        *from = zs.t0 + ZS_HDU_MS + (k - 1u) * ZS_LDU_MS;
        *to = *from + ZS_LDU_MS;
        *kind = (uint8_t)(1u + ((k - 1u) & 1u));     /* 1: LDU1, 2: LDU2 */
    } else {
        *from = zs.t0 + ZS_HDU_MS + ldus * ZS_LDU_MS;
        *to = *from + ZS_TDU_MS;
        *kind = 3;
    }
    return true;
}

static void zs_call(uint32_t at, uint32_t len, bool frames)
{
    zs.t0 = at;
    zs.len = len;
    zs.frames = frames;
    zs.frame = 0;
    zs.heard_ms = zs.first_voice_ms = 0;
}

static bool zs_on_air(void)
{
    return zs.t0 != 0 && (int32_t)(now - zs.t0) >= 0 && now - zs.t0 < zs.len;
}

/* The FM path's status while the radio core has the receiver */
static void zs_rf(void)
{
    bool here = zs_on_air() && hw.rx_hz == zs.hz;
    signal(120, true);
    if (!here)
        return;
    hw.st.noise = zs.rf == ZS_RF_NOISE ? 20 : zs.rf == ZS_RF_DEAF ? 90 : (uint8_t)(44u + zs_random(24));
    hw.st.rssi_dbm = -80;
}

/* The worker: p25_lab.c's channel receive, by its rules */
static void zs_worker(void)
{
    bool receiving = classic_busy && !classic_stop && classic_live && classic_live->state == NT_RX &&
                     classic_live->config.frequency_hz == zs.hz;
    uint8_t state, reason;
    if (receiving)
        receiving = native_lab_voice_poll(classic_sid, &state, &reason) && state == NT_RX;  /* and the lease is kept */
    if (!receiving) {
        zs.listen_ms = 0;
        zs.sid = 0;
    } else if (zs.sid != classic_sid) {
        /* p25_lab_channel_rx: a new receive knows nothing of the last one */
        zs.sid = classic_sid;
        zs.listen_ms = now + ZS_START_MS;
        zs.records = zs.have_group = zs.clear = false;
        scan_voice_ms = 0;
    }
    uint32_t from, to;
    uint8_t kind;
    while (zs.t0 != 0 && zs.frames && zs_frame(zs.frame, &from, &to, &kind) && (int32_t)(now - to) >= 0) {
        zs.frame++;
        if (zs.listen_ms == 0 || (int32_t)(from - zs.listen_ms) < 0)
            continue;       /* not there from the frame's sync on */
        if (!zs.records) {
            zs.records = true;
        }
        zs.valid_ms = zs_sync_ms = now;
        if (kind == 0) {
            zs.have_group = zs.clear = true;
        } else if (kind == 3) {
            zs.have_group = zs.clear = false;
        } else {
            if (kind == 1)
                zs.have_group = true;
            else
                zs.clear = true;
            if (zs.have_group && zs.clear) {
                scan_voice_ms = now;
                zs.heard_ms += ZS_LDU_MS;
                if (zs.first_voice_ms == 0)
                    zs.first_voice_ms = now;
            }
        }
    }
    /* "Noise records cannot keep old PHY synchronization alive": the receive ends and is armed again */
    if (zs.listen_ms != 0 && zs.records && now - zs.valid_ms >= ZS_IDLE_MS)
        classic_stop = true;
}

/* One step of the radio task: as radio_task.c, the radio core's poll waits while the native receiver has the channel */
static void zs_step(void)
{
    now += ZS_STEP_MS;
    zs_worker();
    if (now % RADIO_POLL_MS == 0) {
        if (native_lab_owns()) {
            radio_sm_native_maintenance(&sm, now);
        } else {
            zs_rf();
            radio_sm_poll(&sm, now);
        }
    }
    classic_pump();
}

static void zs_run(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += ZS_STEP_MS)
        zs_step();
}

/* Zone 0 with n positions over the fixture's channels: FM ones, a DMR one, the P25 channel at p25_pos */
static void zs_start(uint8_t n, uint8_t p25_pos, uint8_t rf)
{
    static const uint16_t numbers[7] = { 1, 2, 4, 5, 6, 7, 8 };
    setup_codeplug();
    zones[0].count = n;
    for (unsigned i = 0; i < 7; i++) {
        channel_t *c = &chans[numbers[i]];
        c->mode = i == 2 ? RADIO_MODE_DMR : RADIO_MODE_FM;
        c->rx_hz = c->tx_hz = 446006250u + 12500u * i;
        c->rx_tone = c->tx_tone = (subtone_t){ TONE_NONE, 0 };
        c->bw = RADIO_BW_12K5;
        c->rx_only = false;
        c->color_code = 1;
        c->timeslot = 1;
    }
    channel_t *p = &chans[numbers[6]];
    p->mode = RADIO_MODE_P25;
    p->p25_nac = 0x293;
    p->p25_tg = 42;
    p->p25_id = 3101234;
    unsigned other = 0;
    for (unsigned i = 0; i < n; i++)
        zones[0].channels[i] = i == p25_pos ? numbers[6] : numbers[other++ % 6u];
    memset(&zs, 0, sizeof zs);
    zs.rf = rf;
    zs.p25_pos = p25_pos;
    zs.hz = p->rx_hz;
    zs.rand = 12345u + 977u * rf + n;
    start();
    native_lab_init();
    fake_power.switch_on = classic_on;
    fake_power.battery_mv = classic_battery;
    classic_busy = classic_stop = classic_receiving = false;
    classic_starts = classic_restores = 0;
    scan_voice_ms = zs_sync_ms = 0;
    now += 1000;
    radio_sm_poll(&sm, now);
    req(RREQ_SCAN, RSCAN_ZONE);
    CHECK(sm.scan.kind == SCAN_ZONE);
    zs_run(3000);
}

typedef struct { unsigned calls, caught; uint32_t on_air_ms, heard_ms, late_ms; } zs_rate_t;

/* `calls` calls of len ms, each gap_lo..gap_hi ms after the last one's end; caught: at least two LDUs of it played */
static zs_rate_t zs_traffic(unsigned calls, uint32_t len, uint32_t gap_lo, uint32_t gap_hi)
{
    zs_rate_t r = { 0 };
    for (unsigned i = 0; i < calls; i++) {
        zs_run(gap_lo + zs_random(gap_hi - gap_lo + 1u));
        zs_call(now + ZS_STEP_MS, len, true);
        zs_run(len + 100u);
        r.calls++;
        r.on_air_ms += len;
        r.heard_ms += zs.heard_ms;
        if (zs.heard_ms >= 2u * ZS_LDU_MS) {
            r.caught++;
            r.late_ms += zs.first_voice_ms - zs.t0;
        }
        zs.t0 = 0;
    }
    return r;
}

static unsigned zs_percent(const zs_rate_t *r) { return r->calls ? (r->caught * 100u + r->calls / 2u) / r->calls : 0; }

/* The table for docs/plus.md, and the floor each case must keep: a zone of 8 (once round in 1.2 s) and of 16 (2.4 s) */
static void test_zscan_rates(void)
{
    static const char *const rfs[ZS_RF_KINDS] = { "noise follows", "noise deaf", "noise marginal" };
    static const uint32_t lens[3] = { 2000, 3000, 5000 };
    enum { CALLS = 60 };
    printf("Plus P25 in a zone scan: calls caught (two LDUs or more played), %u calls a cell\n", (unsigned)CALLS);
    printf("  zone  receiver        traffic       2 s   3 s   5 s   heard of the air time\n");
    for (uint8_t n = 8; n <= 16; n += 8) {
        for (uint8_t rf = 0; rf < ZS_RF_KINDS; rf++) {
            for (unsigned traffic = 0; traffic < 2; traffic++) {
                unsigned pct[3], heard = 0, air = 0;
                for (unsigned l = 0; l < 3; l++) {
                    zs_start(n, 5, rf);
                    /* single calls: far apart; a conversation: the next over 1.5 to 6 s after the last */
                    zs_rate_t r = traffic == 0 ? zs_traffic(CALLS, lens[l], 12000, 20000)
                                               : zs_traffic(CALLS, lens[l], 1500, 6000);
                    pct[l] = zs_percent(&r);
                    heard += r.heard_ms / 100u;
                    air += r.on_air_ms / 100u;
                    req(RREQ_SCAN, RSCAN_STOP);
                    zs_run(4000);
                }
                printf("  %-5u %-15s %-12s %3u%%  %3u%%  %3u%%  %3u%%\n", n, rfs[rf],
                       traffic == 0 ? "single" : "conversation", pct[0], pct[1], pct[2], air ? heard * 100u / air : 0u);
#ifndef ZSCAN_BEFORE
                if (n == 8)
                    CHECK(pct[0] >= 95u && pct[1] >= 95u && pct[2] >= 95u);
                else
                    CHECK(pct[0] >= 50u && pct[1] >= 80u && pct[2] >= 95u);
#endif
            }
        }
    }
}

#ifndef ZSCAN_BEFORE
static uint16_t zs_counter(unsigned word)
{
    uint8_t cmd = 12, reply[64];
    size_t n = native_lab_command(&cmd, 1, reply, sizeof reply);
    CHECK(n == 53 && reply[0] == DL_ST_OK && reply[1] == 1);
    return n == 53 ? (uint16_t)(reply[1 + 4 * word] | reply[2 + 4 * word] << 8) : 0;
}
enum { ZS_N_STOPS = 1, ZS_N_CARRIERS, ZS_N_COOLED, ZS_N_TAKES, ZS_N_SYNCED, ZS_N_VOICED, ZS_N_ENDED, ZS_N_TAILS,
       ZS_N_QUIET, ZS_N_FOREIGN, ZS_N_SYNC_MS, ZS_N_VOICE_MS };

/* Until the scan is on the P25 channel and the native receiver has it (false: not within ms) */
static bool zs_until_taken(uint32_t ms)
{
    for (uint32_t t = 0; t < ms && !(native_lab_owns() && sm.view.zone_pos == zs.p25_pos); t += ZS_STEP_MS)
        zs_step();
    return native_lab_owns() && sm.view.zone_pos == zs.p25_pos;
}

/* Until the scan has just tuned zone position pos */
static void zs_until_pos(uint16_t pos)
{
    for (unsigned i = 0; i < 4000 && sm.view.zone_pos == pos; i++)
        zs_step();
    for (unsigned i = 0; i < 4000 && sm.view.zone_pos != pos; i++)
        zs_step();
    CHECK(sm.view.zone_pos == pos && !native_lab_owns());
}

/* The rules one by one, each a miss before the change */
static void test_zscan_rules(void)
{
    /* 1. A receiver whose noise reading does not follow the C4FM carrier: the strength holds the scan */
    zs_start(8, 5, ZS_RF_DEAF);
    zs_until_pos((uint16_t)(zs.p25_pos - 1u));
    zs_call(now + ZS_STEP_MS, 3000, true);
    CHECK(zs_until_taken(1500) && sm.scan.phase == SCAN_PH_HOLD);
    zs_run(3000);
    CHECK(zs.heard_ms >= 1500u && zs_counter(ZS_N_CARRIERS) == 1 && zs_counter(ZS_N_TAKES) == 1 &&
          zs_counter(ZS_N_SYNCED) == 1 && zs_counter(ZS_N_VOICED) == 1);
    CHECK(zs_counter(ZS_N_SYNC_MS) >= ZS_START_MS && zs_counter(ZS_N_SYNC_MS) <= 500u &&
          zs_counter(ZS_N_VOICE_MS) > zs_counter(ZS_N_SYNC_MS) && zs_counter(ZS_N_VOICE_MS) <= 700u);
    /* 2. The call ended: the worker has started over and reports no voice any more, yet this was a call. Given back
     * 1.5 s after its last voice with no cooldown, the scan waits there for the reply, and the reply is taken */
    uint32_t end = zs.t0 + zs.len;
    while (native_lab_owns() && now - end < 4000u)
        zs_step();
    CHECK(!native_lab_owns() && now - end >= 1200u && now - end <= 1700u && zs_counter(ZS_N_ENDED) == 1);
    CHECK(sm.view.zone_pos == zs.p25_pos && scan_voice_ms == 0);
    zs_run(300);
    zs_call(now + ZS_STEP_MS, 3000, true);
    zs_run(3100);
    CHECK(zs.heard_ms >= 1500u && zs_counter(ZS_N_TAKES) == 2 && zs_counter(ZS_N_COOLED) == 0);
    req(RREQ_SCAN, RSCAN_STOP);
    zs_run(4000);

    /* 3. A take on a call's tail (its terminator, then nothing, never voice): no cooldown, the reply is heard */
    zs_start(8, 5, ZS_RF_NOISE);
    zs_call(now + ZS_STEP_MS, 30000, false);    /* the carrier the scan stops on ... */
    CHECK(zs_until_taken(2500));
    while (zs.listen_ms == 0)
        zs_step();
    zs_call(zs.listen_ms + 40u - 2962u, 3000, true);    /* ... is a call with only its TDU left to come */
    while (native_lab_owns() && now - zs.t0 < 8000u)
        zs_step();
    CHECK(zs.heard_ms == 0 && zs_counter(ZS_N_TAILS) == 1 && zs_counter(ZS_N_QUIET) == 0);
    CHECK(now - (zs.t0 + zs.len) <= 900u);      /* not the 2.5 s of a carrier without voice */
    zs_call(now + 1000u, 3000, true);           /* the reply, a second later */
    zs_run(4100);
    CHECK(zs.heard_ms >= 1500u && zs_counter(ZS_N_COOLED) == 0);
    req(RREQ_SCAN, RSCAN_STOP);
    zs_run(4000);

    /* 4. A carrier that is not ours (no frame): given back after the probe time, left alone for the short retry,
     * the scan goes on at once instead of holding there muted; still there: the long cooldown. A call of ours after
     * the carrier has gone is heard */
    zs_start(8, 5, ZS_RF_NOISE);
    zs_call(now + ZS_STEP_MS, 30000, false);
    CHECK(zs_until_taken(1500));
    uint32_t taken = now;
    while (native_lab_owns())
        zs_step();
    CHECK(now - taken >= 1200u && now - taken <= 1400u && zs_counter(ZS_N_QUIET) == 1);
    zs_run(100);
    CHECK(sm.view.zone_pos != zs.p25_pos && sm.scan.phase == SCAN_PH_LISTEN);  /* moved on, no wait there */
    uint32_t free_ms = now;
    CHECK(zs_until_taken(4000) && now - free_ms >= 1500u);      /* the retry, about 2 s later */
    while (native_lab_owns())
        zs_step();
    CHECK(zs_counter(ZS_N_QUIET) == 2);
    free_ms = now;
    CHECK(!zs_until_taken(7000));                                /* now the whole cooldown */
    CHECK(zs_until_taken(3000) && now - free_ms >= 7500u && zs_counter(ZS_N_COOLED) >= 3);
    while (native_lab_owns())
        zs_step();
    zs.t0 = 0;                                                   /* the carrier goes */
    zs_run(9000);
    zs_call(now + ZS_STEP_MS, 3000, true);
    zs_run(3100);
    CHECK(zs.heard_ms >= 900u);
    req(RREQ_SCAN, RSCAN_STOP);
    zs_run(4000);
    /* The FM and DMR stops of the same zone are as they were: a quiet zone steps at the dwell */
    zs_start(8, 5, ZS_RF_DEAF);
    uint32_t steps = sm.scan.steps, t0 = now;
    zs_run(6000);
    CHECK(sm.scan.steps - steps >= (now - t0) / 160u && sm.scan.steps - steps <= (now - t0) / 150u);
    CHECK(zs_counter(ZS_N_STOPS) >= 4 && zs_counter(ZS_N_CARRIERS) == 0 && zs_counter(ZS_N_TAKES) == 0);
    req(RREQ_SCAN, RSCAN_STOP);
    zs_run(1000);
    printf("Plus P25 in a zone scan: strength holds, call end keeps no cooldown, tail, foreign carrier, counters\n");
}
#endif

static void test_zscan(void)
{
#ifndef ZSCAN_BEFORE
    test_zscan_rules();
#endif
    test_zscan_rates();
}
