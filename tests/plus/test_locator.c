/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus locator calculator maths (core/ui/locator.c): published centres of known locators, 4 and 6 characters,
 * case, refusals, the encoder against a double precision reference over a grid of the whole world (and the parse of
 * what it writes back to the same subsquare), and the distance / bearing Go-To's nav_way gives between locator
 * centres against a double precision great circle. Built with -DCONFIG_LOCATOR=1 -DCONFIG_NAV=1 -DCONFIG_APRS=1
 * (nav.c, aprs_fmt.c). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui/locator.h"
#include "ui/nav.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* within e7 units of the expected degrees */
static bool near(int32_t e7, double deg, int32_t tol)
{
	return labs((long)e7 - lround(deg * 1e7)) <= tol;
}

static void ref_encode(double lat, double lon, char *out)
{
	double x = lon + 180.0, y = lat + 90.0;
	out[0] = (char)('A' + (int)(x / 20.0));
	out[1] = (char)('A' + (int)(y / 10.0));
	out[2] = (char)('0' + (int)(fmod(x, 20.0) / 2.0));
	out[3] = (char)('0' + (int)fmod(y, 10.0));
	out[4] = (char)('a' + (int)(fmod(x, 2.0) * 12.0));
	out[5] = (char)('a' + (int)(fmod(y, 1.0) * 24.0));
	out[6] = 0;
}

static double gc_m(double la1, double lo1, double la2, double lo2)
{
	const double r = 6371008.8, d = M_PI / 180.0;
	double a = sin((la2 - la1) * d / 2), b = sin((lo2 - lo1) * d / 2);
	double h = a * a + cos(la1 * d) * cos(la2 * d) * b * b;
	return 2 * r * asin(sqrt(h));
}

static double gc_brg(double la1, double lo1, double la2, double lo2)
{
	const double d = M_PI / 180.0;
	double y = sin((lo2 - lo1) * d) * cos(la2 * d);
	double x = cos(la1 * d) * sin(la2 * d) - sin(la1 * d) * cos(la2 * d) * cos((lo2 - lo1) * d);
	double b = atan2(y, x) / d;
	return b < 0 ? b + 360.0 : b;
}

int main(void)
{
	int32_t la, lo;
	char norm[8], out[8];

	/* Published centres: JN58td (Munich) 48.1458 N 11.6250 E, FN31pr (Newington, ARRL) 41.7292 N 72.7083 W,
	 * IO91wm (London) 51.5208 N 0.1250 W, KM69kw 39.9375 N 32.8750 E (Ankara), and 4-character squares */
	CHECK(loc_parse("JN58td", &la, &lo, norm) && near(la, 48.1458333, 2) && near(lo, 11.625, 2) && !strcmp(norm, "JN58td"));
	CHECK(loc_parse("FN31pr", &la, &lo, norm) && near(la, 41.7291667, 2) && near(lo, -72.7083333, 2));
	CHECK(loc_parse("IO91wm", &la, &lo, norm) && near(la, 51.5208333, 2) && near(lo, -0.125, 2));
	CHECK(loc_parse("KM69kw", &la, &lo, norm) && near(la, 39.9375, 2) && near(lo, 32.875, 2));
	CHECK(loc_parse("KM69", &la, &lo, norm) && near(la, 39.5, 0) && near(lo, 33.0, 0) && !strcmp(norm, "KM69"));
	CHECK(loc_parse("JN58", &la, &lo, NULL) && near(la, 48.5, 0) && near(lo, 11.0, 0));
	/* the corners of the world */
	CHECK(loc_parse("AA00aa", &la, &lo, NULL) && near(la, -90 + 1.0 / 48, 2) && near(lo, -180 + 1.0 / 24, 2));
	CHECK(loc_parse("RR99xx", &la, &lo, NULL) && near(la, 90 - 1.0 / 48, 2) && near(lo, 180 - 1.0 / 24, 2));
	CHECK(loc_parse("RR99", &la, &lo, NULL) && near(la, 89.5, 0) && near(lo, 179.0, 0));
	/* any case, written the usual way */
	CHECK(loc_parse("km69KW", &la, &lo, norm) && !strcmp(norm, "KM69kw") && near(lo, 32.875, 2));
	CHECK(loc_parse("jn58", &la, &lo, norm) && !strcmp(norm, "JN58"));
	/* refused: length, letters past R / X, digits where letters go and the other way, spaces */
	const char *bad[] = { "", "K", "KM6", "KM69k", "KM69kw1", "KM69kwxx", "SM69", "KS69", "ZZ00", "KM6A", "K169",
			      "KM69yw", "KM69ky", "KM6 kw", " KM69", "KM69 ", "KMA9kw", "KM69k9", "1M69kw" };
	for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
		la = lo = 12345;
		CHECK(!loc_parse(bad[i], &la, &lo, norm) && la == 12345 && lo == 12345);
	}

	/* The encoder against the reference over the world (points off the grid lines), and back into the subsquare */
	int mism = 0, outside = 0;
	for (double lat = -89.9; lat < 90.0; lat += 0.731) {
		for (double lon = -179.9; lon < 180.0; lon += 1.137) {
			char ref[8];
			int32_t e7la = (int32_t)lround(lat * 1e7), e7lo = (int32_t)lround(lon * 1e7);
			loc_encode(e7la, e7lo, out);
			ref_encode(e7la / 1e7, e7lo / 1e7, ref);
			mism += strcmp(out, ref) != 0;
			if (!loc_parse(out, &la, &lo, NULL) || labs((long)la - e7la) > 10000000L / 48 + 1 ||
			    labs((long)lo - e7lo) > 10000000L / 24 + 1)
				outside++;
		}
	}
	CHECK(mism == 0);
	CHECK(outside == 0);
	/* the edges clamp into the grid */
	loc_encode(900000000, 1800000000, out);
	CHECK(!strcmp(out, "RR99xx"));
	loc_encode(-900000000, -1800000000, out);
	CHECK(!strcmp(out, "AA00aa"));
	loc_encode(399375000, 328750000, out);
	CHECK(!strcmp(out, "KM69kw"));

	/* Distance and bearing from a locator's centre (Go-To's nav_way, the APRS engine's great circle), against a
	 * double precision one: within 0.5 % and 1 degree */
	static const char *const pairs[][2] = { { "KM69kw", "KN41lb" }, { "KM69kw", "JN58td" }, { "JN58td", "FN31pr" },
						{ "IO91wm", "KM69kw" }, { "KM69kw", "KM69kv" } };
	for (size_t i = 0; i < sizeof pairs / sizeof pairs[0]; i++) {
		int32_t a_la = 0, a_lo = 0, b_la = 0, b_lo = 0;
		CHECK(loc_parse(pairs[i][0], &a_la, &a_lo, NULL) && loc_parse(pairs[i][1], &b_la, &b_lo, NULL));
		nav_way_t w;
		nav_way(a_la, a_lo, b_la, b_lo, 0xFFFFu, 0xFFFFu, &w);
		double m = gc_m(a_la / 1e7, a_lo / 1e7, b_la / 1e7, b_lo / 1e7);
		double brg = gc_brg(a_la / 1e7, a_lo / 1e7, b_la / 1e7, b_lo / 1e7);
		double db = fabs(w.bearing - brg);
		if (db > 180)
			db = 360 - db;
		CHECK(fabs(w.meters - m) <= m * 0.005 + 50);
		CHECK(db <= 1.0);
		CHECK(!w.moving && w.eta_s == 0);
		printf("  %s -> %s: %u m (%.0f), %u deg (%.1f)\n", pairs[i][0], pairs[i][1], (unsigned)w.meters, m,
		       (unsigned)w.bearing, brg);
	}

	printf("locator: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
