// SPDX-License-Identifier: GPL-3.0-or-later
// DM32 Plus multi-watch (CONFIG_MULTI_WATCH, docs/plus.md "Multi-watch"): how long the one receiver takes to go round
// the ring once, for Home's "Ring 0.9 s". An estimate from the radio core's own timing, checked against the radio core
// itself in tests/plus/test_mwatch_radio.inc:
// - a quiet channel (FM, DMR, NXDN, dPMR, or P25 on the FM path) gets the scan's dwell; the receiver moves on at the
//   first poll after it, so each stop takes the dwell rounded up to whole polls;
// - a P25 channel with a signal is handed to the native P25 receiver; a signal that brings no voice for us keeps it
//   up to RADIO_MW_P25_HOLD_MS, then the wait after a signal (dual_delay_ms) follows, as after any signal.
// The constants mirror RADIO_SCAN_DWELL_MS, RADIO_POLL_MS (radio_sm.h) and SCAN_P25_NO_VOICE_MS (native_lab.c); both
// files check that they still agree.
#ifndef RADIO_MWATCH_H
#define RADIO_MWATCH_H

#include <stdint.h>

#define RADIO_MW_DWELL_MS	150u	// RADIO_SCAN_DWELL_MS
#define RADIO_MW_POLL_MS	25u	// RADIO_POLL_MS
#define RADIO_MW_P25_HOLD_MS	2500u	// SCAN_P25_NO_VOICE_MS

// One quiet stop
static inline uint32_t radio_mw_stop_ms(void)
{
	return (RADIO_MW_DWELL_MS + RADIO_MW_POLL_MS - 1u) / RADIO_MW_POLL_MS * RADIO_MW_POLL_MS;
}

// Once round n quiet channels
static inline uint32_t radio_mw_ring_ms(uint8_t n)
{
	return (uint32_t)n * radio_mw_stop_ms();
}

// What a P25 channel with a signal but no voice for us adds to one round, the wait after it delay_ms
static inline uint32_t radio_mw_p25_extra_ms(uint32_t delay_ms)
{
	return RADIO_MW_P25_HOLD_MS + delay_ms;
}

#endif
