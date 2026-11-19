/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus waypoints and Go-To math (nav.h); the great circle comes from the APRS engine (aprs_fmt.h). */
#include "config.h"
#if CONFIG_NAV
#include <string.h>
#include "ui/nav.h"
#include "aprs/aprs_fmt.h"

static nav_point_t points[NAV_WAYPOINTS];	/* points[0] the newest */
static uint8_t count, serial;

uint8_t nav_count(void)
{
	return count;
}

bool nav_get(uint8_t i, nav_point_t *out)
{
	if (i >= count)
		return false;
	*out = points[i];
	return true;
}

uint8_t nav_save(int32_t lat_e7, int32_t lon_e7)
{
	if (count < NAV_WAYPOINTS)
		count++;
	memmove(&points[1], &points[0], (size_t)(count - 1u) * sizeof points[0]);
	serial = (uint8_t)(serial == 255u ? 1u : serial + 1u);
	nav_point_t *p = &points[0];
	memset(p, 0, sizeof *p);
	p->name[0] = 'W';
	p->name[1] = 'P';
	uint8_t n = serial, at = 2;
	char digits[3];
	uint8_t k = 0;
	do {
		digits[k++] = (char)('0' + n % 10u);
		n /= 10u;
	} while (n != 0 && k < sizeof digits);
	while (k > 0)
		p->name[at++] = digits[--k];
	p->lat_e7 = lat_e7;
	p->lon_e7 = lon_e7;
	return serial;
}

bool nav_delete(uint8_t i)
{
	if (i >= count)
		return false;
	memmove(&points[i], &points[i + 1u], (size_t)(count - i - 1u) * sizeof points[0]);
	count--;
	return true;
}

void nav_clear(void)
{
	count = 0;
	serial = 0;
}

bool nav_rename(uint8_t i, const char *name)
{
	if (i >= count)
		return false;
	memset(points[i].name, 0, sizeof points[i].name);
	memcpy(points[i].name, name, strnlen(name, NAV_NAME_LEN));
	return true;
}

void nav_way(int32_t lat_e7, int32_t lon_e7, int32_t to_lat_e7, int32_t to_lon_e7, uint16_t speed_kmh,
	     uint16_t course, nav_way_t *out)
{
	out->meters = aprs_distance_m(lat_e7, lon_e7, to_lat_e7, to_lon_e7);
	out->bearing = aprs_bearing_deg(lat_e7, lon_e7, to_lat_e7, to_lon_e7);
	out->moving = speed_kmh != 0xFFFFu && course != 0xFFFFu && speed_kmh >= NAV_MOVING_KMH;
	int rel = (int)out->bearing - (out->moving ? (int)(course % 360u) : 0);
	while (rel > 180)
		rel -= 360;
	while (rel < -180)
		rel += 360;
	out->rel = (int16_t)rel;
	out->eta_s = out->moving ? (uint32_t)((uint64_t)out->meters * 36u / ((uint32_t)speed_kmh * 10u)) : 0u;
}
#endif
