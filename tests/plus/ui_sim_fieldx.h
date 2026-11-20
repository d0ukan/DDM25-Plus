/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus round 13 ideas 1 UI flows (tests/plus/test_ui_fieldx.c): what the test sets in the simulator
 * (tests/plus/ui_sim_fieldx.c wraps upstream's tests/host/sim/ui_sim.c) and what it records. */
#ifndef UI_SIM_FIELDX_H
#define UI_SIM_FIELDX_H

#include <stdbool.h>
#include <stdint.h>
#include "aprs/aprs.h"
#include "aprs/aprs_plus.h"
#include "radio/p25search.h"
#include "radio/dmrsearch.h"

typedef struct {
	bool gps, fix;			/* the GPS position below instead of the simulator's own */
	int32_t lat_e7, lon_e7;
	bool stations;			/* the APRS stations below instead of the simulator's two */
	uint8_t nst;
	aprs_station_t st[APRS_STATIONS];
	p25s_view_t p25;		/* what the P25 search shows */
	dmrs_view_t dmr;		/* and the DMR search */
	uint16_t zone_cap;		/* channels a zone may hold (0: the simulator's) */
	char req[512];			/* radio requests in order */
	/* the SOS actions asked for */
	int emerg_on, emerg_off, aprs_on, aprs_off;
	uint8_t aprs_why;		/* what aprs_sos_set answers */
	aprs_plus_t aprs;
} fx_sim_t;

extern fx_sim_t fx_sim;
void fx_sim_clear(void);
void fx_sim_dump(void);

#endif
