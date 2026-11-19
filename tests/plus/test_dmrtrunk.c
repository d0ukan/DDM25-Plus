/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus DMR Tier III trunk following (docs/plus.md): the CSBK CRC (CRC-16/GSM catalogue vector, the CSBK and MBC
 * masks), BPTC(196,96) (this test's own encoder from the standard's matrix against upstream's encoder and decoder,
 * single errors corrected), the opcode fields (C_ALOHA, C_BCAST, TV/BTV/PV_GRANT, P_CLEAR, MBC absolute channel
 * parameters), the system identity code, the channel plan, and the follower: TSCC lock, grant > payload channel with
 * its slot > voice > hang / P_CLEAR > back, other group / private / unknown LPCN / band / busy listed, encrypted call
 * listed and left, absolute (MBC) grant, hold-off, no call, lost TSCC > searching, RX group list, every group, idle
 * stop. Run by tools/test_classic.py. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dmr/fec/dmrFec.h"
#include "radio/dmr_csbk.h"
#include "radio/dmrtrunk.h"
#include "dmr_csbk_build.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

bool dmrtk_band_ok(uint32_t hz)
{
    return (hz >= 136000000u && hz <= 174000000u) || (hz >= 400000000u && hz <= 480000000u);
}

/* --- BPTC(196,96), written from ETSI TS 102 361-1 B.1.1: a 13 x 15 matrix (bit 0 unused, R(2..0) zero), 9 rows of 11
 * data bits + Hamming(15,11,3), 15 columns of 9 + Hamming(13,9,3), sent in the order (i * 181) mod 196 --- */
static void ham15113(uint8_t *d)
{
    d[11] = d[0] ^ d[1] ^ d[2] ^ d[3] ^ d[5] ^ d[7] ^ d[8];
    d[12] = d[1] ^ d[2] ^ d[3] ^ d[4] ^ d[6] ^ d[8] ^ d[9];
    d[13] = d[2] ^ d[3] ^ d[4] ^ d[5] ^ d[7] ^ d[9] ^ d[10];
    d[14] = d[0] ^ d[1] ^ d[2] ^ d[4] ^ d[6] ^ d[7] ^ d[10];
}
static void ham1393(uint8_t *d)
{
    d[9] = d[0] ^ d[1] ^ d[3] ^ d[5] ^ d[6];
    d[10] = d[0] ^ d[1] ^ d[2] ^ d[4] ^ d[6] ^ d[7];
    d[11] = d[0] ^ d[1] ^ d[2] ^ d[3] ^ d[5] ^ d[7] ^ d[8];
    d[12] = d[0] ^ d[2] ^ d[4] ^ d[5] ^ d[8];
}
static void my_bptc(const uint8_t in[12], uint8_t raw[196])
{
    uint8_t m[196];
    memset(m, 0, sizeof m);
    unsigned pos = 0;
#define IN(p) ((in[(p) >> 3] >> (7u - ((p) & 7u))) & 1u)
    for (unsigned a = 4; a <= 11; a++, pos++) m[a] = (uint8_t)IN(pos);
    for (unsigned r = 1; r < 9; r++)
        for (unsigned a = 1 + 15 * r; a <= 11 + 15 * r; a++, pos++) m[a] = (uint8_t)IN(pos);
#undef IN
    for (unsigned r = 0; r < 9; r++) ham15113(&m[1 + 15 * r]);
    for (unsigned c = 0; c < 15; c++) {
        uint8_t col[13];
        for (unsigned r = 0; r < 13; r++) col[r] = m[1 + c + 15 * r];
        ham1393(col);
        for (unsigned r = 9; r < 13; r++) m[1 + c + 15 * r] = col[r];
    }
    for (unsigned a = 0; a < 196; a++) raw[(a * 181u) % 196u] = m[a];
}
static unsigned burst_bit(const uint8_t *b, unsigned i) { return (b[i >> 3] >> (7u - (i & 7u))) & 1u; }
static void burst_flip(uint8_t *b, unsigned i) { b[i >> 3] ^= (uint8_t)(0x80u >> (i & 7u)); }
/* The burst position of BPTC bit i: info bits 0-97, then 166-263 (slot type and sync between) */
static unsigned info_pos(unsigned i) { return i < 98u ? i : i + 68u; }

static void test_crc_bptc(void)
{
    /* CRC-16/GSM (poly 0x1021, init 0, xorout 0xFFFF) of "123456789" is 0xCE3C: the first nine octets, the tenth
     * would change it, so the check runs over a 10-octet block whose value was worked out with the same register */
    uint8_t v[12] = { '1', '2', '3', '4', '5', '6', '7', '8', '9', 0 };
    /* the catalogue vector, nine octets: */
    uint16_t reg = 0;
    for (unsigned i = 0; i < 9; i++) {
        reg ^= (uint16_t)(v[i] << 8);
        for (unsigned k = 0; k < 8; k++) reg = (uint16_t)((reg & 0x8000u) ? (uint16_t)(reg << 1) ^ 0x1021u : (uint16_t)(reg << 1));
    }
    CHECK((uint16_t)~reg == 0xCE3Cu);
    /* the same register over ten octets is what dmr_csbk_crc gives, and this test's bitwise builder agrees */
    for (unsigned k = 0; k < 8; k++) reg = (uint16_t)((reg & 0x8000u) ? (uint16_t)(reg << 1) ^ 0x1021u : (uint16_t)(reg << 1));
    CHECK(dmr_csbk_crc(v) == (uint16_t)~reg);
    cb_crc(v, DMR_CSBK_MASK);
    CHECK(((uint16_t)(v[10] << 8 | v[11])) == (uint16_t)(~reg ^ 0xA5A5u));
    CHECK(dmr_csbk_crc_ok(v, DMR_CSBK_MASK) && !dmr_csbk_crc_ok(v, DMR_MBC_MASK) && !dmr_csbk_crc_ok(v, 0));
    v[3] ^= 0x10u;
    CHECK(!dmr_csbk_crc_ok(v, DMR_CSBK_MASK));

    /* BPTC: a C_ALOHA through our encoder equals upstream's, decodes back, and every single error is corrected */
    uint8_t csbk[12], raw[196], burst[DMR_FEC_BURST_BYTES], back[12];
    cb_aloha(csbk, cb_syscode(1, 12, 3, 0));
    my_bptc(csbk, raw);
    memset(burst, 0, sizeof burst);
    dmrFecBptcEncode(csbk, burst);
    unsigned same = 0;
    for (unsigned i = 0; i < 196; i++) same += burst_bit(burst, info_pos(i)) == raw[i];
    CHECK(same == 196u);
    CHECK(dmrFecBptcDecode(burst, back) == 0 && memcmp(back, csbk, 12) == 0);
    unsigned fixed = 0;
    for (unsigned i = 0; i < 196; i++) {
        uint8_t bad[DMR_FEC_BURST_BYTES];
        memcpy(bad, burst, sizeof bad);
        burst_flip(bad, info_pos(i));
        int n = dmrFecBptcDecode(bad, back);
        fixed += (i == 0 || n >= 0) && memcmp(back, csbk, 12) == 0 && dmr_csbk_crc_ok(back, DMR_CSBK_MASK);
    }
    CHECK(fixed == 196u);
    /* random blocks: ours and upstream's always agree */
    srand(7);
    unsigned agree = 0;
    for (int t = 0; t < 200; t++) {
        for (int i = 0; i < 12; i++) csbk[i] = (uint8_t)rand();
        my_bptc(csbk, raw);
        dmrFecBptcEncode(csbk, burst);
        unsigned s = 0;
        for (unsigned i = 0; i < 196; i++) s += burst_bit(burst, info_pos(i)) == raw[i];
        agree += s == 196u;
    }
    CHECK(agree == 200u);
    printf("DMR Tier III CSBK: CRC-16/GSM vector, masks, BPTC(196,96) ours = upstream's, single errors corrected\n");
}

static void test_parse(void)
{
    uint8_t b[12];
    dmr_csbk_t c;
    dmr_sys_t s;
    /* C_ALOHA: Small model, net 12, site 3, partition 2 */
    cb_aloha(b, cb_syscode(1, 12, 3, 2));
    CHECK(dmr_csbk_crc_ok(b, DMR_CSBK_MASK));
    dmr_csbk_parse(b, &c);
    CHECK(c.op == CSBKO_C_ALOHA && c.fid == 0 && c.last && !c.protect);
    dmr_csbk_sys(c.syscode, &s);
    CHECK(s.model == 1 && s.net == 12 && s.site == 3 && s.par == 2);
    for (uint8_t m = 0; m < 4; m++) {
        static const uint16_t nmax[4] = { 511, 127, 15, 3 }, smax[4] = { 7, 31, 255, 1023 };
        dmr_csbk_sys(cb_syscode(m, nmax[m], smax[m], 1), &s);
        CHECK(s.model == m && s.net == nmax[m] && s.site == smax[m] && s.par == 1);
        dmr_csbk_sys(cb_syscode(m, 1, 0, 0), &s);
        CHECK(s.net == 1 && s.site == 0);
    }
    /* C_BCAST Ann_WD_TSCC: color codes 1 and 2, both announced (parms1), channels 17 and 18 (parms2) */
    cb_bcast(b, DMR_ANN_WD_TSCC, (uint16_t)(1u << 6 | 2u << 2 | 3u), cb_syscode(0, 300, 5, 0), 17u << 12 | 18u, true);
    dmr_csbk_parse(b, &c);
    CHECK(c.op == CSBKO_C_BCAST && c.ann == DMR_ANN_WD_TSCC && c.parms1 == (1u << 6 | 2u << 2 | 3u) &&
          c.parms2 == (17u << 12 | 18u));
    dmr_csbk_sys(c.syscode, &s);
    CHECK(s.model == 0 && s.net == 300 && s.site == 5);
    /* Vote_Now, Site params: their type and parameters */
    cb_bcast(b, DMR_ANN_VOTE_NOW, 0, cb_syscode(2, 9, 200, 0), 0x123456u, true);
    dmr_csbk_parse(b, &c);
    CHECK(c.ann == DMR_ANN_VOTE_NOW && c.parms2 == 0x123456u);
    cb_bcast(b, DMR_ANN_SITE_PARMS, 0x2AAAu, cb_syscode(2, 9, 200, 0), 0, true);
    dmr_csbk_parse(b, &c);
    CHECK(c.ann == DMR_ANN_SITE_PARMS && c.parms1 == 0x2AAAu);
    /* Chan_Freq is an MBC: the header's CRC uses the MBC mask */
    cb_bcast(b, DMR_ANN_CHAN_FREQ, 0, cb_syscode(1, 12, 3, 0), 0, false);
    CHECK(dmr_csbk_crc_ok(b, DMR_MBC_MASK) && !dmr_csbk_crc_ok(b, DMR_CSBK_MASK));
    dmr_csbk_parse(b, &c);
    CHECK(c.op == CSBKO_C_BCAST && !c.last && c.ann == DMR_ANN_CHAN_FREQ);
    /* grants: LPCN, slot, emergency, target, source */
    cb_grant(b, CSBKO_TV_GRANT, 37, 2, true, 2621, 2620123, true);
    dmr_csbk_parse(b, &c);
    CHECK(c.op == CSBKO_TV_GRANT && c.lpcn == 37 && c.ts == 2 && c.emergency && c.target == 2621 && c.source == 2620123);
    cb_grant(b, CSBKO_BTV_GRANT, 4094, 1, false, 0xFFFFFDu, 1, true);
    dmr_csbk_parse(b, &c);
    CHECK(c.op == CSBKO_BTV_GRANT && c.lpcn == 4094 && c.ts == 1 && !c.emergency && c.target == 0xFFFFFDu && c.source == 1);
    cb_grant(b, CSBKO_PV_GRANT, 5, 1, false, 2620124, 2620123, true);
    dmr_csbk_parse(b, &c);
    CHECK(c.op == CSBKO_PV_GRANT && c.target == 2620124 && c.source == 2620123);
    cb_grant(b, CSBKO_P_CLEAR, 37, 2, true, 2621, 0, true);
    dmr_csbk_parse(b, &c);
    CHECK(c.op == CSBKO_P_CLEAR && c.lpcn == 37 && c.ts == 2 && !c.emergency);
    /* MBC absolute channel parameters: 125 Hz steps, both frequencies */
    dmr_chan_t ch;
    cb_chan(b, 12, 443512500u, 438512500u);
    CHECK(dmr_csbk_chan(b, &ch) && ch.lpcn == 12 && ch.tx_hz == 443512500u && ch.rx_hz == 438512500u);
    cb_chan(b, 4093, 146006250u, 146006250u);
    CHECK(dmr_csbk_chan(b, &ch) && ch.lpcn == 4093 && ch.rx_hz == 146006250u);
    cb_put(b, 2, 4, 1);     /* another CDEFTYPE: not channel parameters */
    CHECK(!dmr_csbk_chan(b, &ch));
    /* a vendor's FID or the protect flag: parsed, but the follower ignores them (below) */
    cb_aloha(b, 0);
    b[1] = 0x10;
    dmr_csbk_parse(b, &c);
    CHECK(c.fid == 0x10);
    printf("DMR Tier III CSBK fields: C_ALOHA, C_BCAST (WD_TSCC, Vote_Now, Site params, Chan_Freq), grants, P_CLEAR, "
           "MBC channel parameters, system codes of every model\n");
}

/* --- the follower --- */
static uint32_t now = 1000;
static dmrtk_view_t view;
static dmrtk_rx_t rx;

static void feed(uint8_t kind, int8_t ts, const uint8_t b[12]) { dmrtk_burst(kind, ts, b, now); }
static void csbk(const uint8_t b[12]) { feed(3, 1, b); }
/* One radio task turn 10 ms on with the screen looking; the receiver goes where the follower says (as radio_sm.c) */
static uint32_t step(void)
{
    now += 10;
    dmrtk_snapshot(&view);
    uint32_t hz = dmrtk_poll(&rx, now);
    if (hz != rx.on_hz || dmrtk_ts() != rx.on_ts) {
        rx.on_hz = hz;
        rx.on_ts = dmrtk_ts();
        rx.call = DMRTK_RX_NONE;
        rx.enc = false;
        rx.src = 0;
    }
    dmrtk_snapshot(&view);
    return hz;
}
static void wait(uint32_t ms) { for (uint32_t t = 0; t < ms; t += 10) step(); }

static const uint32_t TSCC = 438500000u;

static void announce(uint16_t lpcn, uint32_t rx_hz)
{
    uint8_t h[12], c[12];
    cb_bcast(h, DMR_ANN_CHAN_FREQ, 0, cb_syscode(1, 12, 3, 0), 0, false);
    cb_chan(c, lpcn, rx_hz + 5000000u, rx_hz);
    feed(4, 1, h);
    feed(5, 1, c);
}

static void test_follow(void)
{
    uint8_t b[12];
    static const uint32_t list[2] = { 2622, 2623 };
    memset(&rx, 0, sizeof rx);
    dmrtk_start(TSCC, 2621, false, list, 2, now);
    CHECK(dmrtk_on() && step() == TSCC && dmrtk_ts() == 0 && view.state == DMRTK_HUNT && !view.have_sys);
    /* the TSCC: C_ALOHA locks and names the system */
    cb_aloha(b, cb_syscode(1, 12, 3, 0));
    csbk(b);
    step();
    CHECK(view.state == DMRTK_CONTROL && view.have_sys && view.model == 1 && view.net == 12 && view.site == 3 &&
          view.csbks == 1);
    /* a CSBK failing its CRC is counted, a vendor's (FID 0x10) and a protected one are not Tier III */
    b[4] ^= 1;
    csbk(b);
    cb_aloha(b, 0);
    b[1] = 0x10;
    cb_seal(b);
    csbk(b);
    cb_aloha(b, 0);
    b[0] |= 0x40;
    cb_seal(b);
    csbk(b);
    step();
    CHECK(view.bad == 1 && view.csbks == 1 && view.model == 1);
    /* the rate: CSBKs in the last second */
    cb_aloha(b, cb_syscode(1, 12, 3, 0));
    for (int i = 0; i < 200; i++) { if (i % 5 == 0) csbk(b); step(); }
    CHECK(view.rate >= 19 && view.rate <= 21 && view.state == DMRTK_CONTROL);

    /* a grant for the group before its channel is known: listed "?" (NO_PLAN), not followed */
    cb_grant(b, CSBKO_TV_GRANT, 37, 2, false, 2621, 2620123, true);
    csbk(b);
    CHECK(step() == TSCC && view.n == 1 && view.recent[0].flags == DMRTK_G_NO_PLAN && view.recent[0].hz == 0 &&
          view.recent[0].lpcn == 37 && view.recent[0].ts == 2);
    /* the plan: Chan_Freq (MBC) for LPCN 37 and 38; another group's grant is listed only */
    announce(37, 438512500u);
    announce(38, 438525000u);
    step();
    CHECK(view.plan_n == 2 && view.mbcs == 2 && view.bcasts == 2 && dmrtk_channel_hz(37) == 438512500u &&
          dmrtk_channel_hz(99) == 0);
    cb_grant(b, CSBKO_TV_GRANT, 38, 1, false, 9, 2620999, true);
    csbk(b);
    CHECK(step() == TSCC && view.recent[0].tg == 9 && view.recent[0].flags == DMRTK_G_OTHER &&
          view.recent[0].hz == 438525000u);
    /* a private call: listed, followed with every group only */
    cb_grant(b, CSBKO_PV_GRANT, 38, 1, false, 2620124, 2620123, true);
    csbk(b);
    CHECK(step() == TSCC && view.recent[0].flags == DMRTK_G_PRIVATE);

    /* the followed group's grant: the receiver to LPCN 37, slot 2 */
    cb_grant(b, CSBKO_TV_GRANT, 37, 2, true, 2621, 2620123, true);
    csbk(b);
    CHECK(step() == 438512500u && dmrtk_ts() == 2 && view.state == DMRTK_VOICE && view.calls == 1 &&
          view.call.tg == 2621 && view.call.src == 2620123 && view.call.emergency && view.call.flags == DMRTK_G_FOLLOWED);
    CHECK(view.n == 3 && view.recent[0].tg == 2621 && view.recent[0].hz == 438512500u);   /* one entry, updated */
    /* voice plays: it stays as long as the call does */
    rx.call = DMRTK_RX_PLAY;
    rx.src = 2620123;
    wait(3000);
    CHECK(rx.on_hz == 438512500u && view.state == DMRTK_VOICE);
    /* the call's voice ends (terminator): the hang, then back to the TSCC on both slots */
    rx.call = DMRTK_RX_NONE;
    wait(DMRTK_HANG_MS - 100u);
    CHECK(rx.on_hz == 438512500u && view.state == DMRTK_VOICE);
    wait(200);
    CHECK(rx.on_hz == TSCC && rx.on_ts == 0 && view.state == DMRTK_CONTROL && view.call.ms != 0);
    /* its repeated grant just after: held off; the next one later is followed again */
    csbk(b);
    CHECK(step() == TSCC && view.recent[0].flags == DMRTK_G_BUSY);
    wait(DMRTK_HOLDOFF_MS);
    csbk(b);
    CHECK(step() == 438512500u && view.calls == 2);
    /* another over in the hang: it stays; then P_CLEAR on the payload channel: back at once */
    rx.call = DMRTK_RX_PLAY;
    wait(500);
    rx.call = DMRTK_RX_NONE;
    wait(1000);
    rx.call = DMRTK_RX_PLAY;
    wait(500);
    CHECK(view.state == DMRTK_VOICE);
    rx.call = DMRTK_RX_NONE;
    cb_grant(b, CSBKO_P_CLEAR, 37, 2, false, 2621, 0, true);
    feed(3, 2, b);
    CHECK(step() == TSCC && view.state == DMRTK_CONTROL);
    /* a grant while on a payload channel (heard there) for another group: busy */
    wait(DMRTK_HOLDOFF_MS);
    cb_grant(b, CSBKO_TV_GRANT, 37, 2, false, 2622, 2620123, true);   /* the RX group list's */
    csbk(b);
    CHECK(step() == 438512500u && view.call.tg == 2622);
    cb_grant(b, CSBKO_TV_GRANT, 38, 1, false, 2623, 2620123, true);
    feed(3, 2, b);
    step();
    CHECK(view.recent[0].tg == 2623 && view.recent[0].flags == DMRTK_G_BUSY && rx.on_hz == 438512500u);
    /* no voice at all on the payload channel: back after DMRTK_NO_CALL_MS, its grants held off longer */
    wait(DMRTK_NO_CALL_MS + 20u);
    CHECK(rx.on_hz == TSCC && view.state == DMRTK_CONTROL);
    cb_grant(b, CSBKO_TV_GRANT, 37, 2, false, 2622, 2620123, true);
    wait(DMRTK_HOLDOFF_MS + 100u);
    csbk(b);
    CHECK(step() == TSCC && view.recent[0].flags == DMRTK_G_BUSY);

    /* an encrypted call (no key: muted): listed ENC, the receiver goes back, held off */
    cb_grant(b, CSBKO_TV_GRANT, 38, 1, false, 2621, 2620555, true);
    csbk(b);
    CHECK(step() == 438525000u && dmrtk_ts() == 1);
    rx.call = DMRTK_RX_MUTED;
    rx.enc = true;
    step();
    CHECK(rx.on_hz == TSCC && view.call.flags == DMRTK_G_ENC && view.recent[0].flags == DMRTK_G_ENC);
    csbk(b);
    CHECK(step() == TSCC && view.recent[0].flags == DMRTK_G_BUSY);

    /* an absolute grant: TV_GRANT with LPCN 0xFFF as an MBC header, the continuation's channel parameters */
    uint8_t c[12];
    cb_grant(b, CSBKO_TV_GRANT, DMR_LPCN_ABSOLUTE, 2, false, 2621, 2620777, false);
    cb_chan(c, 77, 443550000u, 438550000u);
    feed(4, 2, b);
    CHECK(step() == TSCC);      /* waits for its continuation */
    feed(5, 2, c);
    CHECK(step() == 438550000u && dmrtk_ts() == 2 && view.call.lpcn == 77 && view.call.src == 2620777 &&
          dmrtk_channel_hz(77) == 438550000u);
    rx.call = DMRTK_RX_PLAY;
    wait(200);
    rx.call = DMRTK_RX_NONE;
    wait(DMRTK_HANG_MS + 20u);
    CHECK(rx.on_hz == TSCC);
    /* an absolute grant whose continuation never comes: listed with its frequency unknown */
    wait(DMRTK_HOLDOFF_MS);
    cb_grant(b, CSBKO_TV_GRANT, DMR_LPCN_ABSOLUTE, 1, false, 2621, 2620888, false);
    feed(4, 1, b);
    cb_aloha(c, cb_syscode(1, 12, 3, 0));
    csbk(c);
    CHECK(step() == TSCC && view.recent[0].src == 2620888 && view.recent[0].flags == DMRTK_G_NO_PLAN);
    /* out of this radio's bands */
    announce(40, 851012500u);
    cb_grant(b, CSBKO_TV_GRANT, 40, 1, false, 2621, 1, true);
    csbk(b);
    CHECK(step() == TSCC && view.recent[0].flags == DMRTK_G_BAND && view.recent[0].hz == 851012500u);

    /* every group: another group and a private call are followed */
    dmrtk_follow(2621, true);
    cb_grant(b, CSBKO_TV_GRANT, 38, 1, false, 9, 2620999, true);
    csbk(b);
    CHECK(step() == 438525000u && view.call.tg == 9);
    wait(DMRTK_NO_CALL_MS + 20u);
    cb_grant(b, CSBKO_PV_GRANT, 37, 1, false, 2620124, 2620123, true);
    csbk(b);
    CHECK(step() == 438512500u && view.call.op == CSBKO_PV_GRANT);
    wait(DMRTK_NO_CALL_MS + 20u);
    dmrtk_follow(2621, false);

    /* the TSCC lost: searching after DMRTK_LOST_MS without a Tier III CSBK, locked again when one comes */
    wait(DMRTK_LOST_MS + 20u);
    CHECK(view.state == DMRTK_HUNT && view.lost == 1 && rx.on_hz == TSCC);
    cb_aloha(b, cb_syscode(1, 12, 3, 0));
    csbk(b);
    step();
    CHECK(view.state == DMRTK_CONTROL);
    /* searching again: a grant locks it (any Tier III CSBK does) and is followed */
    wait(DMRTK_LOST_MS + 20u);
    cb_grant(b, CSBKO_TV_GRANT, 37, 2, false, 2621, 1, true);
    csbk(b);
    CHECK(step() == 438512500u);

    /* the TSCC learnt as a TX frequency: the plan names the base station's side, its TX is the downlink */
    dmrtk_start(TSCC, 0, false, NULL, 0, now);
    step();
    CHECK(view.all && view.plan_n == 0);
    uint8_t h[12];
    cb_bcast(h, DMR_ANN_CHAN_FREQ, 0, 0, 0, false);
    cb_chan(c, 1, TSCC, TSCC - 5000000u);
    feed(4, 1, h);
    feed(5, 1, c);
    cb_chan(c, 2, 438512500u, 433512500u);
    feed(4, 1, h);
    feed(5, 1, c);
    step();
    CHECK(dmrtk_channel_hz(2) == 438512500u);

    /* nobody looks (the screen was left): it stops */
    for (uint32_t t = 0; t < DMRTK_IDLE_MS + 100u && dmrtk_on(); t += 10) {
        now += 10;
        dmrtk_poll(&rx, now);
    }
    CHECK(!dmrtk_on() && dmrtk_hz() == 0);
    printf("DMR Tier III follower: TSCC lock, plan, grant > payload slot > voice > hang / P_CLEAR > back, hold-off, "
           "no call, encrypted, absolute grant, band, other / private / every group, lost, idle stop\n");
}

int main(void)
{
    test_crc_bptc();
    test_parse();
    test_follow();
    printf("DMR Tier III trunk following: %d failures\n", failures);
    return failures ? 1 : 0;
}
