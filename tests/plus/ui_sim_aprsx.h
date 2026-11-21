/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus round 14 APRS job UI flows (tests/plus/test_ui_aprsx.c): what the test sets in the simulator
 * (tests/plus/ui_sim_aprsx.c wraps upstream's tests/host/sim/ui_sim.c) and what it records. */
#ifndef UI_SIM_APRSX_H
#define UI_SIM_APRSX_H

#include <stdbool.h>
#include <stdint.h>
#include "aprs/aprs.h"
#include "aprs/aprs_plus.h"

#define AX_MSGS		APRS_MESSAGES

typedef struct {
	bool fix;			/* our GPS position (lat_e7, lon_e7) */
	int32_t lat_e7, lon_e7;
	uint8_t nst;			/* the APRS stations heard, 0 = most recent */
	aprs_station_t st[APRS_STATIONS];
	uint8_t nmsg;			/* the inbox, 0 = newest */
	aprs_msg_t msg[AX_MSGS];
	aprs_plus_t aprs;		/* aprs_plus_state's answer */
	char sent_to[16], sent_text[72];	/* the last message handed to the engine */
	int aprs_on, aprs_off, emerg_on, emerg_off;
} ax_sim_t;

extern ax_sim_t ax_sim;
void ax_sim_clear(void);
void ax_sim_dump(void);

#endif
