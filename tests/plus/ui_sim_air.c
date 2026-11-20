/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus round 14 air items (tests/plus/test_ui_air.c): upstream's UI simulator (tests/host/sim/ui_sim.c) as it is,
 * built in place of it, with what Tools > Airfields needs from the radio: one that receives AM (the screen is shown and
 * tunes) and the position aprs_position_now gives. */
#include "sim/ui_sim.c"
#include "ui_sim_air.h"

air_sim_t air_sim;

static bool air_am_rx(uint32_t hz, radio_bw_t bw)
{
	(void)hz;
	(void)bw;
	return true;
}
static hal_radio_t air_radio = { .rx_am = air_am_rx };

void air_sim_setup(void)
{
	memset(&air_sim, 0, sizeof air_sim);
	board.caps |= CAP_AM_RX;	/* after sim_reset (it sets the caps of the key map) */
	board.radio = &air_radio;
}

void air_sim_state(radio_state_t st)
{
	view.state = st;
	changed();
}

/* The position APRS uses (Tools > Airfields' order) */
bool aprs_position_now(int32_t *lat, int32_t *lon, uint32_t *err_m, bool *dmr_gps)
{
	*err_m = 0;
	*dmr_gps = false;
	if (!air_sim.pos)
		return false;
	*lat = air_sim.lat_e7;
	*lon = air_sim.lon_e7;
	return true;
}
