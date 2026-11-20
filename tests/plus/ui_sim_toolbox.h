/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus round 13 ideas 2 UI flows (tests/plus/test_ui_toolbox.c): what the test sets in the simulator
 * (tests/plus/ui_sim_toolbox.c wraps upstream's tests/host/sim/ui_sim.c) and what it records. */
#ifndef UI_SIM_TOOLBOX_H
#define UI_SIM_TOOLBOX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "radio/spectrum.h"
#include "radio/p25search.h"
#include "radio/dmrsearch.h"
#include "radio/dmr_csbk.h"
#include "radio/dmrtrunk.h"
#include "radio/p25trunk.h"
#include "radio/radio_msg.h"

typedef struct {
	/* the GPS: a fix (or none) at a position, with speed, course, altitude; the UTC time runs on from utc_hms as the
	   simulator's clock goes (utc_at: when it was utc_hms) */
	bool gps, fix, time;
	int32_t lat_e7, lon_e7;
	uint16_t speed_kmh, course;
	int16_t alt_m;
	uint32_t utc_hms, utc_at;
	p25s_view_t p25;		/* what the P25 search shows */
	dmrs_view_t dmr;		/* and the DMR search */
	p25tk_view_t p25tk;		/* the trunk screens */
	dmrtk_view_t dmrtk;
	uint8_t pager_n;		/* the pager's messages */
	radio_pager_msg_t pager[2];
	uint32_t msg_seq;
	char req[512];			/* radio requests in order */
	int beeps;			/* radio_req_beep */
	uint8_t last_beep;
	/* the heap the waterfall takes (UI_HOST_HEAP): a failure on request, the bytes held, calls */
	bool heap_fail;
	size_t heap_held;
	int heap_allocs, heap_frees, heap_refused;
} tb_sim_t;

extern tb_sim_t tb_sim;
void tb_sim_clear(void);
void tb_sim_dump(void);
void tb_sim_rssi(int16_t dbm);		/* the view's signal strength */
uint32_t tb_sim_freq(void);		/* the view's RX frequency */
void tb_sim_changed(void);		/* the view changed: the screen on top draws again */

#endif
