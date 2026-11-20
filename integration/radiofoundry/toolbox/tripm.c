/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus GPS dashboard's trip meter (tripm.h): the portable part. Installed as core/ui/tripm.c. */
#include "config.h"
#if CONFIG_GPS_DASH
#include <string.h>
#include "ui/tripm.h"

#define M_PER_DEG	111195	/* metres per degree on the great circle (6,371 km) */

void trip_reset(trip_t *t)
{
	memset(t, 0, sizeof *t);
}

/* cos(latitude) Q15 (Bhaskara I's approximation, within 0.2 %; as the map's mapnav_cos_q15) */
static uint32_t cos_q15(int32_t lat_e7)
{
	int64_t c = (lat_e7 < 0 ? -(int64_t)lat_e7 : lat_e7) / 100000;	/* centidegrees */
	if (c > 9000)
		c = 9000;
	int64_t num = 324000000 - 4 * c * c, den = 324000000 + c * c;
	int64_t q = (num * 32768 + den / 2) / den;
	return (uint32_t)(q > 32767 ? 32767 : q);
}

static uint32_t isqrt64(uint64_t v)
{
	uint64_t r = 0, bit = (uint64_t)1 << 62;
	while (bit > v)
		bit >>= 2;
	while (bit != 0) {
		if (v >= r + bit) {
			v -= r + bit;
			r = (r >> 1) + bit;
		} else {
			r >>= 1;
		}
		bit >>= 2;
	}
	return (uint32_t)r;
}

uint32_t trip_dist_m(int32_t lat1_e7, int32_t lon1_e7, int32_t lat2_e7, int32_t lon2_e7)
{
	int64_t dy = ((int64_t)lat2_e7 - lat1_e7) * M_PER_DEG / 10000000;
	int64_t dx = ((int64_t)lon2_e7 - lon1_e7) * M_PER_DEG / 10000000 * cos_q15(lat1_e7) / 32768;
	return isqrt64((uint64_t)(dx * dx) + (uint64_t)(dy * dy));
}

void trip_feed(trip_t *t, bool fix, int32_t lat_e7, int32_t lon_e7, uint16_t speed_kmh, uint32_t now_ms)
{
	if (!fix)
		return;
	if (speed_kmh != 0xFFFFu && speed_kmh <= TRIP_SPEED_MAX_KMH && speed_kmh > t->max_kmh)
		t->max_kmh = speed_kmh;
	if (!t->have) {
		t->have = true;
		t->lat = lat_e7;
		t->lon = lon_e7;
		t->at_ms = now_ms;
		return;
	}
	uint32_t d = trip_dist_m(t->lat, t->lon, lat_e7, lon_e7);
	if (d < TRIP_MIN_M)
		return;
	uint32_t dt = now_ms - t->at_ms;
	/* d m in dt ms faster than TRIP_JUMP_KMH: d / dt > TRIP_JUMP_KMH / 3600 (m a ms) */
	if ((uint64_t)d * 3600u > (uint64_t)TRIP_JUMP_KMH * (dt > 0u ? dt : 1u)) {
		t->lat = lat_e7;	/* a jump: count from here on */
		t->lon = lon_e7;
		t->at_ms = now_ms;
		return;
	}
	t->meters += d;
	t->lat = lat_e7;
	t->lon = lon_e7;
	t->at_ms = now_ms;
}

const char *trip_compass16(uint16_t deg)
{
	static const char *const pts[16] = { "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
					     "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW" };
	return pts[((uint32_t)(deg % 360u) * 2u + 22u) / 45u % 16u];
}
#endif
