/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus round 14 air items' UI flows: what tests/plus/ui_sim_air.c adds to upstream's simulator */
#ifndef UI_SIM_AIR_H
#define UI_SIM_AIR_H

#include <stdbool.h>
#include <stdint.h>
#include "radio/radio.h"

typedef struct {
	bool pos;			/* aprs_position_now has a position */
	int32_t lat_e7, lon_e7;
	/* the air watch's requests (CONFIG_AIR_WATCH): VFO frequencies asked for, ends */
	int vfo_calls, end_calls;
	uint32_t hz[6];
	uint8_t atis;			/* the VFOs asked for as an ATIS (UI_AIR_ATIS) */
} air_sim_t;

extern air_sim_t air_sim;

void air_sim_setup(void);			/* after sim_reset: AM receive */
void air_sim_state(radio_state_t st);		/* the radio's state (RS_TX: transmitting) */
uint32_t air_sim_vfo_hz(uint8_t idx);		/* VFO idx's frequency as the simulated radio keeps it */

#endif
