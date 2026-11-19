/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus Tools > Marine VHF (CONFIG_MARINE, docs/plus.md "Marine VHF"): the international maritime mobile VHF
 * channels of ITU Radio Regulations Appendix 18 (the table ITU-R M.1084 Annex 4 refers to), each with its ship
 * station and coast station transmit frequency and its use, and the receive-only rule for them in the VFO.
 *
 * Listening: the VFO on the channel's coast station frequency (what a ship hears; the ship frequency for the
 * intership-only channels), FM, 25 kHz, no tones, simplex. Never transmitting: an amateur licence does not cover the
 * maritime mobile service. The radio core refuses TX (TXDENY_RX_ONLY, the RX only refusal a channel's RX only flag
 * gives) while the VFO's RX or TX frequency is one of this table's frequencies (marine_hz): so a VFO tuned from this
 * screen stays receive only after a later TX split, a mode change or a power cycle (the VFO's frequency is kept in
 * the settings, the rule is the frequency's). Codeplug channels are not affected. Channel 70 (DSC) and AIS 1 / 2
 * carry data only: they are listed, not tuned.
 *
 * Portable (no RTOS): the radio core's TX check and the host tests use it as is. */
#ifndef PLUS_MARINE_H
#define PLUS_MARINE_H

#include <stdbool.h>
#include <stdint.h>

#define MARINE_BASE_HZ	156000000u	/* marine_ch_t frequencies: (Hz - this) / MARINE_STEP_HZ */
#define MARINE_STEP_HZ	12500u
#define MARINE_NONE	0xFFFFu		/* this side does not transmit on the channel */
#define MARINE_TOL_HZ	1250u		/* marine_hz: this close to a table frequency */

/* The channel's use (Appendix 18's columns and notes, shortened) */
typedef enum {
	MAR_DUPLEX,		/* two frequencies: public correspondence, port operations */
	MAR_SIMPLEX,		/* one frequency: port operations and ship movement, intership */
	MAR_INTERSHIP,		/* ships only */
	MAR_DISTRESS,		/* 16: distress, safety and calling */
	MAR_DSC,		/* 70: digital selective calling, data only */
	MAR_SAR,		/* 06: intership, search and rescue */
	MAR_BRIDGE,		/* 13: bridge to bridge, navigation safety */
	MAR_ONBOARD,		/* 15, 17: on-board communications, 1 W */
	MAR_NAV,		/* 75, 76: navigation related, 1 W (guard channels of 16) */
	MAR_AIS,		/* AIS 1 / 2: data only */
	MAR_USES
} marine_use_t;

typedef struct {
	char name[4];		/* "16", "87B" */
	uint16_t ship;		/* the ship station's TX frequency, MARINE_BASE_HZ + ship x 12.5 kHz */
	uint16_t coast;		/* the coast station's, MARINE_NONE: none */
	uint8_t use;		/* marine_use_t */
} marine_ch_t;

extern const marine_ch_t marine_table[];
extern const uint8_t marine_count;

uint32_t marine_ship_hz(const marine_ch_t *c);
uint32_t marine_coast_hz(const marine_ch_t *c);		/* 0: none */
/* What listening to it tunes (the coast frequency, else the ship one); false: data only, not tuned */
bool marine_listen_hz(const marine_ch_t *c, uint32_t *hz);
/* hz is (within MARINE_TOL_HZ) a ship or coast frequency of the table: the VFO there must not transmit */
bool marine_hz(uint32_t hz);
/* The table index of channel name ("16", "87b"), or -1 */
int marine_find(const char *name);

#endif
