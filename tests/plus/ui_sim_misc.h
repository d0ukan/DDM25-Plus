/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus round 3 "misc" UI flows: what tests/plus/ui_sim_misc.c adds to upstream's simulator */
#ifndef UI_SIM_MISC_H
#define UI_SIM_MISC_H

#include <stdbool.h>
#include <stdint.h>
#include "radio/radio.h"

typedef struct {
	bool pos;			/* aprs_position_now has a position */
	int32_t lat_e7, lon_e7;
	char req[600];			/* the radio requests in order: "vfo:1 vfo_set:118100000 ..." */
} misc_sim_t;

extern misc_sim_t misc_sim;

void misc_sim_setup(void);			/* after sim_reset: AM receive, the recorder cleared */
void misc_sim_battery_mv(uint16_t mv);		/* the voltage the radio core reads (radio_view_t.battery_mv) */
void misc_sim_state(radio_state_t st);		/* the radio's state (RS_TX: transmitting) */

#endif
