/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus search ranges (docs/plus.md "Search ranges and speed", CONFIG_SEARCH_RANGE): the step the P25 and DMR
 * searches sweep with. Installed as core/radio/srange.h; the UI (ui_srange.c) and the radio core both include it.
 *
 * The step travels in the search request itself (radio_req_p25_search / radio_req_dmr_search keep their two Hz
 * arguments): the start frequency's last decimal digit names it. Every frequency the radio tunes is a multiple of
 * 10 Hz (the FD6818 takes 10 Hz units), so that digit is free; 0 is 12.5 kHz, the step the searches always had, so a
 * request without a tag sweeps as before. */
#ifndef PLUS_SRANGE_H
#define PLUS_SRANGE_H

#include <stdint.h>

#define SRANGE_STEPS	10u

/* Tag digit -> step in Hz: 12.5 kHz first (an untagged start), then the others smallest first */
static inline uint32_t srange_step_hz(uint8_t tag)
{
	static const uint32_t hz[SRANGE_STEPS] = { 12500u, 2500u, 5000u, 6250u, 10000u, 20000u, 25000u, 50000u,
						   100000u, 1000000u };
	return hz[tag < SRANGE_STEPS ? tag : 0u];
}

/* The start frequency with the step's tag, and back */
static inline uint32_t srange_tag(uint32_t lo_hz, uint8_t tag)
{
	return lo_hz / 10u * 10u + (tag < SRANGE_STEPS ? tag : 0u);
}

static inline uint8_t srange_tag_of(uint32_t tagged)
{
	return (uint8_t)(tagged % 10u);
}

static inline uint32_t srange_lo_of(uint32_t tagged)
{
	return tagged - tagged % 10u;
}

#endif
