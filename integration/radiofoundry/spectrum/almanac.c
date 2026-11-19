/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus almanac (almanac.h): the sunrise equation in its common almanac form, single precision. */
#include "config.h"
#if CONFIG_ALMANAC
#include <math.h>
#include "ui/almanac.h"

#define DEG	0.017453292f

static float fix360(float x)
{
	while (x < 0.0f)
		x += 360.0f;
	while (x >= 360.0f)
		x -= 360.0f;
	return x;
}

static int day_of_year(int d, int m, int y)
{
	static const int before[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
	bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
	return before[m - 1] + d + (leap && m > 2 ? 1 : 0);
}

/* UTC hours when the sun crosses zenith (degrees from straight up) rising or setting; false when it doesn't */
static bool sun_cross(int n, float lat, float lon, float zenith, bool rising, float *ut)
{
	float lng_hour = lon / 15.0f;
	float t = (float)n + ((rising ? 6.0f : 18.0f) - lng_hour) / 24.0f;
	float m = 0.9856f * t - 3.289f;
	float l = fix360(m + 1.916f * sinf(m * DEG) + 0.020f * sinf(2.0f * m * DEG) + 282.634f);
	float ra = fix360(atanf(0.91764f * tanf(l * DEG)) / DEG);
	ra += floorf(l / 90.0f) * 90.0f - floorf(ra / 90.0f) * 90.0f;	/* the same quadrant as L */
	ra /= 15.0f;
	float sin_dec = 0.39782f * sinf(l * DEG), cos_dec = cosf(asinf(sin_dec));
	float cos_h = (cosf(zenith * DEG) - sin_dec * sinf(lat * DEG)) / (cos_dec * cosf(lat * DEG));
	if (cos_h > 1.0f || cos_h < -1.0f)
		return false;
	float h = (rising ? 360.0f - acosf(cos_h) / DEG : acosf(cos_h) / DEG) / 15.0f;
	float tt = h + ra - 0.06571f * t - 6.622f;
	float u = tt - lng_hour;
	while (u < 0.0f)
		u += 24.0f;
	while (u >= 24.0f)
		u -= 24.0f;
	*ut = u;
	return true;
}

static int16_t local_min(bool ok, float ut, int16_t tz_min)
{
	if (!ok)
		return ALMANAC_NONE;
	int m = (int)(ut * 60.0f + 0.5f) + tz_min;
	while (m < 0)
		m += 1440;
	return (int16_t)(m % 1440);
}

/* Days from 2000-01-01 12:00 UTC to noon UTC of the date */
static int32_t days_j2000(int d, int m, int y)
{
	int32_t days = 0;
	for (int yy = 2000; yy < y; yy++)
		days += ((yy % 4 == 0 && yy % 100 != 0) || yy % 400 == 0) ? 366 : 365;
	return days + day_of_year(d, m, y) - 1;
}

bool almanac_compute(uint32_t ddmmyy, int32_t lat_e7, int32_t lon_e7, int16_t tz_min, almanac_t *out)
{
	int d = (int)(ddmmyy / 10000u), m = (int)(ddmmyy / 100u % 100u), y = 2000 + (int)(ddmmyy % 100u);
	if (d < 1 || d > 31 || m < 1 || m > 12)
		return false;
	float lat = (float)lat_e7 / 1e7f, lon = (float)lon_e7 / 1e7f;
	int n = day_of_year(d, m, y);
	float ut;
	bool ok = sun_cross(n, lat, lon, 90.833f, true, &ut);
	out->rise_min = local_min(ok, ut, tz_min);
	ok = sun_cross(n, lat, lon, 90.833f, false, &ut);
	out->set_min = local_min(ok, ut, tz_min);
	ok = sun_cross(n, lat, lon, 96.0f, true, &ut);
	out->dawn_min = local_min(ok, ut, tz_min);
	ok = sun_cross(n, lat, lon, 96.0f, false, &ut);
	out->dusk_min = local_min(ok, ut, tz_min);

	/* The moon: a new moon on 2000-01-06 18:14 UTC (5.26 days after the J2000 noon), mean synodic month */
	const float synodic = 29.530588f;
	float age = fmodf((float)days_j2000(d, m, y) - 5.26f, synodic);
	if (age < 0.0f)
		age += synodic;
	out->moon_age10 = (uint16_t)(age * 10.0f + 0.5f);
	float f = age / synodic;
	out->moon_lit = (uint8_t)((1.0f - cosf(2.0f * 3.14159265f * f)) * 50.0f + 0.5f);
	out->waxing = f < 0.5f;
	out->moon_phase = (uint8_t)((int)(f * 8.0f + 0.5f) % 8);
	return true;
}
#endif
