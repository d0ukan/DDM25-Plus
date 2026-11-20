/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus GPS dashboard's trip meter (CONFIG_GPS_DASH, docs/plus.md "GPS dashboard"): the distance travelled from
 * the GPS fixes and the top speed since the last reset. The portable part (no RTOS, no float); installed as
 * core/ui/tripm.{c,h}; the screen and the once-a-second feed are ui_gpsdash.c.
 *
 * The distance grows in steps: a fix TRIP_MIN_M or more from the last counted point adds the straight way there and
 * becomes the new point, so standing still (a fix wandering a few metres) adds nothing, and a slow walk counts once it
 * has gone TRIP_MIN_M. A step faster than TRIP_JUMP_KMH (a fix far off after a bad one, or the first fix after a long
 * outage abroad) is not counted: the point moves there. The top speed is the GPS's own speed, never above
 * TRIP_SPEED_MAX_KMH (a spike). 20 bytes. */
#ifndef PLUS_TRIPM_H
#define PLUS_TRIPM_H

#include <stdbool.h>
#include <stdint.h>

#define TRIP_MIN_M		25u	/* a step is counted once this far from the last counted point */
#define TRIP_JUMP_KMH		400u	/* faster than this from the last point: a jump, not counted */
#define TRIP_SPEED_MAX_KMH	300u	/* a GPS speed above this is a spike: not the top speed */
#define TRIP_MOVING_KMH		3u	/* slower than this the course means nothing */

typedef struct {
	int32_t lat, lon;	/* the last counted point */
	uint32_t at_ms;		/* when it was counted */
	uint32_t meters;	/* since the reset */
	uint16_t max_kmh;
	bool have;		/* a point to count from */
} trip_t;

void trip_reset(trip_t *t);
/* A GPS reading (ui_gpsdash.c: once a second): nothing without a fix. speed_kmh 0xFFFF: unknown */
void trip_feed(trip_t *t, bool fix, int32_t lat_e7, int32_t lon_e7, uint16_t speed_kmh, uint32_t now_ms);
/* Metres between two positions (equirectangular at the first one's latitude: right to a few metres for a step) */
uint32_t trip_dist_m(int32_t lat1_e7, int32_t lon1_e7, int32_t lat2_e7, int32_t lon2_e7);
/* "N", "NNE", ... "NW", "NNW": the nearest of the 16 compass points */
const char *trip_compass16(uint16_t deg);

#endif
