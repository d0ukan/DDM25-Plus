/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus priority channel and temporary skip UI flows: upstream's UI simulator (tests/host/sim/ui_sim.c) as it is,
 * built in place of it, with its scan services taken over for the test: the scan's phase the view reports once a
 * scan starts (scx_sim_phase: stepping, or stopped on a signal), and the skip and Clear skips requests counted (the
 * skip then handed to the simulator's own, which moves the channel on). */
#define ui_svc_scan sim_base_scan
#define ui_svc_scan_skip sim_base_scan_skip
#include "sim/ui_sim.c"
#undef ui_svc_scan
#undef ui_svc_scan_skip
#include "radio/scan.h"

uint8_t scx_sim_phase;
unsigned scx_sim_skips, scx_sim_unskips;

bool ui_svc_scan(bool on);
bool ui_svc_scan_skip(void);

bool ui_svc_scan(bool on)
{
	bool r = sim_base_scan(on);
	view.scan_phase = on ? scx_sim_phase : SCAN_PH_LISTEN;
	changed();
	return r;
}

bool ui_svc_scan_skip(void)
{
	scx_sim_skips++;
	return sim_base_scan_skip();
}

bool ui_svc_scan_unskip(void)
{
	scx_sim_unskips++;
	return true;
}
