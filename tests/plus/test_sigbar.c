// SPDX-License-Identifier: GPL-3.0-or-later
// DDM32 Plus: the status strip's signal bars (tools/plus_sigbar.py), the functions cut from the generated ui_screens.c
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "radio/radio.h"

#include SIGBAR_INC

int main(void)
{
	radio_view_t v;
	memset(&v, 0, sizeof v);
	// Nothing heard: the noise floor never lights a bar, whatever it reads
	for (int dbm = -130; dbm <= -60; dbm++) {
		v.rssi_dbm = (int16_t)dbm;
		v.state = RS_IDLE;
		assert(strip_bars(&v) == 0);
		v.state = RS_TX;
		assert(strip_bars(&v) == 0);
	}
	// Squelch open (FM, AM, P25 call): the level as upstream showed it
	v.state = RS_RX;
	v.rssi_dbm = -80; assert(strip_bars(&v) == 4);
	v.rssi_dbm = -95; assert(strip_bars(&v) == 3);
	v.rssi_dbm = -105; assert(strip_bars(&v) == 2);
	v.rssi_dbm = -115; assert(strip_bars(&v) == 1);
	v.rssi_dbm = -125; assert(strip_bars(&v) == 0);
	// DMR call: the call's level first, else the receiver's
	v.state = RS_IDLE;
	v.rssi_dbm = -125;
	v.dmr.state = RDMR_RX;
	v.dmr.rssi_dbm = -85; assert(strip_bars(&v) == 4);
	v.dmr.state = RDMR_RX_OTHER;
	v.dmr.rssi_dbm = 0; v.rssi_dbm = -100; assert(strip_bars(&v) == 2);
	v.dmr.state = RDMR_IDLE;
	v.dmr.rssi_dbm = -85; assert(strip_bars(&v) == 0);	// a finished call keeps its level: no bars
	puts("sigbar ok");
	return 0;
}
