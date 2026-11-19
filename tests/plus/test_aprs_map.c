// SPDX-License-Identifier: GPL-3.0-or-later
// DM32 Plus APRS stations on the map (docs/map.md "APRS stations"): the upstream radio-core APRS suite
// (tests/host/aprsRadioTest.c: the engine with its station list) run again with CONFIG_MAP built in (the station
// entry gains the altitude in its tail padding: the struct must not grow), then frames heard through the engine's own
// receive path (aprs_rx_frame): the altitude kept from "/A=" (feet to metres, negative, clamped), kept over a status
// packet, dropped by a position without one, none for a DMR caller's position; and the map's pick (mapnav.c) over the
// engine's own list (aprs_station as the getter): nearest the centre first, the walk, a station that only sent a status
// left out, the newest heard moving to the top of the list without changing the pick.
// Built like aprsRadioTest (tools/test_classic.py) with -DCONFIG_MAP=1 and mapnav.c, map.c and its data.
#define main upstream_aprs_radio_main
#include "aprsRadioTest.c"
#undef main
#include <stdlib.h>
#include "ui/mapnav.h"

_Static_assert(sizeof(aprs_station_t) == 64, "the altitude sits in the station entry's tail padding (no RAM)");

static void test_station_altitude(void)
{
	aprs_station_t s;
	reset();
	aprs_set_test_call("N0TST");
	configure(0, false);
	rx_line("TA1ABC-9>APRS,WIDE1-1:!3955.80N/03252.80E>Hello /A=001234 mobile");
	CHECK(aprs_station_count() == 1);
	CHECK(aprs_station(0, &s) && s.has_pos && s.alt_m == 376 && strcmp(s.call, "TA1ABC-9") == 0);
	CHECK(s.sym_table == '/' && s.sym_code == '>');
	char c[sizeof s.text];
	mapnav_comment(s.text, c, sizeof c);
	CHECK(strcmp(c, "Hello mobile") == 0);
	// a status from it: the position and the altitude stay
	rx_line("TA1ABC-9>APRS:>on the road");
	CHECK(aprs_station(0, &s) && s.has_pos && s.alt_m == 376);
	// a position without an altitude: none
	rx_line("TA1ABC-9>APRS:!3955.80N/03252.80E>no altitude");
	CHECK(aprs_station(0, &s) && s.has_pos && s.alt_m == MAPNAV_ALT_NONE);
	// below sea level, and beyond int16 (clamped)
	rx_line("TA1ABC-9>APRS:!3955.80N/03252.80E>/A=-00100 Dead Sea");
	CHECK(aprs_station(0, &s) && s.alt_m == -30);
	rx_line("TA1ABC-9>APRS:!3955.80N/03252.80E>/A=999999 balloon");
	CHECK(aprs_station(0, &s) && s.alt_m == 32767);
	// a DMR caller's position (GPS Info): no altitude
	aprs_dmr_heard(2861234, "TA3DMR Ali", 399000000, 329000000, now);
	CHECK(aprs_station(0, &s) && strcmp(s.call, "TA3DMR") == 0 && s.has_pos && s.alt_m == MAPNAV_ALT_NONE);
}

// The map's pick over the engine's own list
static void test_station_pick(void)
{
	reset();
	aprs_set_test_call("N0TST");
	configure(0, false);
	rx_line("TA2XYZ-7>APRS:!4012.00N/03251.60E>north 30 km");	// 40.20, 32.86
	rx_line("TA7B-15>APRS:!3948.00N/03236.00E>south-west 26 km");	// 39.80, 32.60
	rx_line("TB3A>APRS:>status only");
	rx_line("TA1ABC-9>APRS:!3957.00N/03252.80E>2.7 km");	// 39.95, 32.88
	CHECK(aprs_station_count() == 4);
	int32_t la = 399300000, lo = 328600000;	// Ankara
	uint16_t h = mapnav_station_next(aprs_station, aprs_station_count(), la, lo, 0);
	aprs_station_t s;
	CHECK(mapnav_station_find(aprs_station, aprs_station_count(), h, &s) == 0 && strcmp(s.call, "TA1ABC-9") == 0);
	h = mapnav_station_next(aprs_station, aprs_station_count(), la, lo, h);
	CHECK(mapnav_station_find(aprs_station, aprs_station_count(), h, &s) >= 0 && strcmp(s.call, "TA7B-15") == 0);
	// TA2XYZ-7 heard again: it moves to the top of the list, the pick (TA7B-15) stays where it was in the order
	rx_line("TA2XYZ-7>APRS:!4012.00N/03251.60E>again");
	CHECK(aprs_station(0, &s) && strcmp(s.call, "TA2XYZ-7") == 0);
	CHECK(mapnav_station_find(aprs_station, aprs_station_count(), h, &s) >= 0 && strcmp(s.call, "TA7B-15") == 0);
	h = mapnav_station_next(aprs_station, aprs_station_count(), la, lo, h);
	CHECK(mapnav_station_find(aprs_station, aprs_station_count(), h, &s) >= 0 && strcmp(s.call, "TA2XYZ-7") == 0);
	// after the last: the nearest again; the status-only station is never picked
	h = mapnav_station_next(aprs_station, aprs_station_count(), la, lo, h);
	CHECK(mapnav_station_find(aprs_station, aprs_station_count(), h, &s) >= 0 && strcmp(s.call, "TA1ABC-9") == 0);
	CHECK(mapnav_station_find(aprs_station, aprs_station_count(), mapnav_call_hash("TB3A"), &s) < 0);
	// the list full of newer stations: the picked one falls off, the walk starts over at the nearest
	h = mapnav_call_hash("TA7B-15");
	char line[80];
	for (int i = 0; i < APRS_STATIONS; i++) {
		snprintf(line, sizeof line, "TA%dQ>APRS:!40%02d.00N/03300.00E>filler", i, i);
		rx_line(line);
	}
	CHECK(aprs_station_count() == APRS_STATIONS);
	CHECK(mapnav_station_find(aprs_station, aprs_station_count(), h, &s) < 0);
	h = mapnav_station_next(aprs_station, aprs_station_count(), 400000000, 330000000, h);
	CHECK(mapnav_station_find(aprs_station, aprs_station_count(), h, &s) >= 0 && strcmp(s.call, "TA0Q") == 0);
}

int main(void)
{
	int up = upstream_aprs_radio_main();
	CHECK(up == 0);
	test_station_altitude();
	test_station_pick();
	printf("# test_aprs_map: %d checks, %s\n", checks, failures ? "FAILED" : "PASSED");
	return failures ? 1 : 0;
}
