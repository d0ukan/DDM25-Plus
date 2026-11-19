/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus waypoints and Go-To (docs/plus.md): up to NAV_WAYPOINTS positions saved from the GPS (WP1, WP2, ... the
 * oldest replaced when full), kept in RAM (gone at power off), and the way to a target from here: distance, true
 * bearing, the bearing relative to the course while moving, and the time to get there at the current speed. */
#ifndef PLUS_NAV_H
#define PLUS_NAV_H

#include <stdbool.h>
#include <stdint.h>

#define NAV_WAYPOINTS		8u
#define NAV_NAME_LEN		7u
#define NAV_MOVING_KMH		3u	/* slower than this the GPS course means nothing: north up */

typedef struct {
	char name[NAV_NAME_LEN + 1];
	int32_t lat_e7, lon_e7;
} nav_point_t;

typedef struct {
	uint32_t meters;
	uint16_t bearing;	/* true, 0..359 */
	bool moving;		/* the course is good: rel is the target's bearing from the course */
	int16_t rel;		/* -180 .. 180, + to the right */
	uint32_t eta_s;		/* 0 = unknown (not moving) */
} nav_way_t;

uint8_t nav_count(void);
bool nav_get(uint8_t i, nav_point_t *out);		/* 0 = the newest */
uint8_t nav_save(int32_t lat_e7, int32_t lon_e7);	/* the new point's number (1..255, cycling) */
bool nav_delete(uint8_t i);
void nav_clear(void);
bool nav_rename(uint8_t i, const char *name);		/* up to NAV_NAME_LEN characters (Tools > Map's "START") */

void nav_way(int32_t lat_e7, int32_t lon_e7, int32_t to_lat_e7, int32_t to_lon_e7, uint16_t speed_kmh,
	     uint16_t course, nav_way_t *out);

#endif
