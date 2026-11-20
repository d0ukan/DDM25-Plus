/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus Maidenhead locators (locator.h): pure, integer. In 1e-7 degree a field is 20 x 10 degrees, a square 2 x 1,
 * a subsquare 1/12 x 1/24 degree; the centre adds half of the last unit. Longitudes are counted from 180 W and
 * latitudes from 90 S, in unsigned 32 bits (360e7 does not fit an int32). */
#include "config.h"
#if CONFIG_LOCATOR
#include <stddef.h>
#include "ui/locator.h"

static char upper(char c)
{
	return c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c;
}

bool loc_parse(const char *t, int32_t *lat_e7, int32_t *lon_e7, char *norm)
{
	size_t n = 0;
	while (n <= LOC_LEN_MAX && t[n] != '\0')
		n++;
	if (n != 4u && n != 6u)
		return false;
	char a = upper(t[0]), b = upper(t[1]);
	if (a < 'A' || a > 'R' || b < 'A' || b > 'R' || t[2] < '0' || t[2] > '9' || t[3] < '0' || t[3] > '9')
		return false;
	uint32_t lon = (uint32_t)(a - 'A') * 200000000u + (uint32_t)(t[2] - '0') * 20000000u;
	uint32_t lat = (uint32_t)(b - 'A') * 100000000u + (uint32_t)(t[3] - '0') * 10000000u;
	char c = 0, d = 0;
	if (n == 6u) {
		c = upper(t[4]);
		d = upper(t[5]);
		if (c < 'A' || c > 'X' || d < 'A' || d > 'X')
			return false;
		/* the subsquare's corner plus half of it: (2s + 1) / 24 degree east, (2s + 1) / 48 degree north */
		lon += ((uint32_t)(c - 'A') * 2u + 1u) * 10000000u / 24u;
		lat += ((uint32_t)(d - 'A') * 2u + 1u) * 10000000u / 48u;
	} else {
		lon += 10000000u;	/* half a square: 1 degree east, half a degree north */
		lat += 5000000u;
	}
	*lon_e7 = (int32_t)(lon - 1800000000u);
	*lat_e7 = (int32_t)lat - 900000000;
	if (norm != NULL) {
		norm[0] = a;
		norm[1] = b;
		norm[2] = t[2];
		norm[3] = t[3];
		if (n == 6u) {
			norm[4] = (char)(c - 'A' + 'a');
			norm[5] = (char)(d - 'A' + 'a');
		}
		norm[n] = '\0';
	}
	return true;
}

void loc_encode(int32_t lat_e7, int32_t lon_e7, char *out)
{
	int64_t lo = (int64_t)lon_e7 + 1800000000, la = (int64_t)lat_e7 + 900000000;
	if (lo < 0)
		lo = 0;
	if (lo > 3599999999LL)
		lo = 3599999999LL;
	if (la < 0)
		la = 0;
	if (la > 1799999999LL)
		la = 1799999999LL;
	uint32_t x = (uint32_t)lo, y = (uint32_t)la;
	out[0] = (char)('A' + x / 200000000u);
	out[1] = (char)('A' + y / 100000000u);
	out[2] = (char)('0' + x % 200000000u / 20000000u);
	out[3] = (char)('0' + y % 100000000u / 10000000u);
	out[4] = (char)('a' + x % 20000000u * 12u / 10000000u);	/* 12 a degree: 24 in the 2 degree square */
	out[5] = (char)('a' + y % 10000000u * 24u / 10000000u);
	out[6] = '\0';
}
#endif
