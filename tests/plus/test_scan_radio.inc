/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus round 15 scan job in the real radio state machine (docs/plus.md "Search ranges and speed"), included
 * after tests/classic/test_radio.inc by tools/test_scan.py (whose main runs first: upstream's radio suite and the
 * Classic / Plus P25 suites, the searches' own among them, with this job's switches). Item 1: the search requests
 * carry the step on their start (radio/srange.h): the receiver steps by it; an untagged start steps 12.5 kHz. Item 2
 * (CONFIG_SEARCH_FAST): a quiet step takes one poll. */
#include "radio/srange.h"

#if CONFIG_P25_SEARCH
/* The P25 search from lo (tagged) to hi: the first `n` frequencies the receiver is tuned to */
static uint32_t sj_at[8];	/* when each of them was tuned */
static void sj_p25_steps(uint32_t lo, uint32_t hi, uint32_t *hz, int n)
{
    setup_codeplug();
    start();
    now += 1000;
    radio_sm_poll(&sm, now);
    radio_req_t r = { .type = RREQ_P25_SEARCH, .arg = (int32_t)lo, .arg2 = hi };
    radio_sm_request(&sm, &r, now);
    hw.st.rssi_dbm = -124;
    hw.st.noise = 255;
    int k = 0;
    uint32_t seen = 0;
    for (int i = 0; i < 400 && k < n; i++) {
        p25s_view_t v;
        p25s_snapshot(&v);
        if (hw.rx_hz != seen) {
            sj_at[k] = now;
            hz[k++] = seen = hw.rx_hz;
        }
        now += RADIO_POLL_MS;
        radio_sm_poll(&sm, now);
    }
    radio_req_t s = { .type = RREQ_P25_SEARCH };
    radio_sm_request(&sm, &s, now);
}

static void test_scanjob_p25_step(void)
{
    uint32_t hz[4];
    sj_p25_steps(srange_tag(144000000u, 6u), 146000000u, hz, 4);	/* tag 6: 25 kHz */
    CHECK(hz[0] == 144000000u && hz[1] == 144025000u && hz[2] == 144050000u && hz[3] == 144075000u);
    /* A quiet step: one poll with the faster sweep (CONFIG_SEARCH_FAST), two before */
    CHECK(sj_at[2] - sj_at[1] == (CONFIG_SEARCH_FAST ? 1u : 2u) * RADIO_POLL_MS &&
          sj_at[3] - sj_at[2] == (CONFIG_SEARCH_FAST ? 1u : 2u) * RADIO_POLL_MS);
    sj_p25_steps(srange_tag(430000000u, 9u), 433000000u, hz, 4);	/* tag 9: 1 MHz, round to the start */
    CHECK(hz[0] == 430000000u && hz[1] == 431000000u && hz[2] == 432000000u && hz[3] == 433000000u);
    sj_p25_steps(433000000u, 434000000u, hz, 3);			/* untagged: 12.5 kHz */
    CHECK(hz[0] == 433000000u && hz[1] == 433012500u && hz[2] == 433025000u);
    sj_p25_steps(srange_tag(433000000u, 3u), 434000000u, hz, 3);	/* tag 3: 6.25 kHz */
    CHECK(hz[0] == 433000000u && hz[1] == 433006250u && hz[2] == 433012500u);
    CHECK(!p25s_on());
    printf("Plus search range: the P25 search steps by the request's step (25 kHz, 1 MHz, 12.5 kHz, 6.25 kHz), %u ms a quiet step\n",
           (unsigned)(sj_at[2] - sj_at[1]));
}
#endif

int main(void)
{
    int prior = classic_radio_main();
    int before = failures;
#if CONFIG_P25_SEARCH
    test_scanjob_p25_step();
#endif
    printf("Plus scan job radio core: %d failures\n", failures - before);
    return prior || failures != before ? 1 : 0;
}
