// SPDX-License-Identifier: GPL-3.0-or-later
// DM32 Plus multi-watch (CONFIG_MULTI_WATCH, docs/plus.md "Multi-watch"): how long the one receiver takes to go round
// the ring once, for Home's "Ring 0.9 s". An estimate from the radio core's own timing, checked against the radio core
// itself in tests/plus/test_mwatch_radio.inc:
// - a quiet channel (FM, DMR, NXDN, dPMR, or P25 on the FM path) gets the scan's dwell; the receiver moves on at the
//   first poll after it, so each stop takes the dwell rounded up to whole polls;
// - a P25 channel with a signal is handed to the native P25 receiver; a signal that brings no voice for us keeps it
//   up to RADIO_MW_P25_HOLD_MS, then the wait after a signal (dual_delay_ms) follows, as after any signal.
// DDM32 Plus faster ring (CONFIG_MWATCH_FAST, docs/plus.md "The multi-watch ring", 3..6 channels only; dual watch's
// pair keeps the above): a stop with nothing heard ends at the first reading after the settle (SCAN_SETTLE_MS), a
// DMR one at the third (its direct mode bursts are on air 30 ms in 60); a P25 signal without voice for us is given back
// after the hold and passed over while it lasts (no wait after it).
// The constants mirror RADIO_SCAN_DWELL_MS, RADIO_POLL_MS (radio_sm.h), SCAN_SETTLE_MS (scan.h) and
// SCAN_P25_NO_VOICE_MS (native_lab.c); both files check that they still agree.
#ifndef RADIO_MWATCH_H
#define RADIO_MWATCH_H

#include <stdbool.h>
#include <stdint.h>
#include "config.h"

#define RADIO_MW_DWELL_MS	150u	// RADIO_SCAN_DWELL_MS
#define RADIO_MW_POLL_MS	25u	// RADIO_POLL_MS
#define RADIO_MW_P25_HOLD_MS	2500u	// SCAN_P25_NO_VOICE_MS
#if CONFIG_MWATCH_FAST
#define RADIO_MW_SETTLE_MS	50u	// SCAN_SETTLE_MS
#define RADIO_MW_DMR_READS	3u	// readings of a quiet DMR stop, a poll apart from the settle on
#endif

// One quiet stop (dual watch; the ring without CONFIG_MWATCH_FAST)
static inline uint32_t radio_mw_stop_ms(void)
{
	return (RADIO_MW_DWELL_MS + RADIO_MW_POLL_MS - 1u) / RADIO_MW_POLL_MS * RADIO_MW_POLL_MS;
}

// One quiet stop on a ring of n channels (dmr: a DMR channel)
static inline uint32_t radio_mw_quiet_ms(uint8_t n, bool dmr)
{
#if CONFIG_MWATCH_FAST
	if (n > 2) {
		uint32_t q = RADIO_MW_SETTLE_MS + (dmr ? (RADIO_MW_DMR_READS - 1u) * RADIO_MW_POLL_MS : 0u);
		q = (q + RADIO_MW_POLL_MS - 1u) / RADIO_MW_POLL_MS * RADIO_MW_POLL_MS;
		return q < radio_mw_stop_ms() ? q : radio_mw_stop_ms();
	}
#endif
	(void)n;
	(void)dmr;
	return radio_mw_stop_ms();
}

// Once round n quiet channels, n_dmr of them DMR
static inline uint32_t radio_mw_ring_ms(uint8_t n, uint8_t n_dmr)
{
	return (uint32_t)(n - n_dmr) * radio_mw_quiet_ms(n, false) + (uint32_t)n_dmr * radio_mw_quiet_ms(n, true);
}

// What a P25 channel with a signal but no voice for us adds to one round of n, the wait after it delay_ms
static inline uint32_t radio_mw_p25_extra_ms(uint8_t n, uint32_t delay_ms)
{
#if CONFIG_MWATCH_FAST
	if (n > 2)
		return RADIO_MW_P25_HOLD_MS;	// given back after the hold and passed over while it lasts: no wait
#endif
	(void)n;
	return RADIO_MW_P25_HOLD_MS + delay_ms;
}

#endif
