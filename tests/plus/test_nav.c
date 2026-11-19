/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus waypoints and Go-To math (nav.c): the store (newest first, names, full ring, delete) and the way:
 * Istanbul to Ankara against a double precision great circle, relative bearing while moving, ETA, standing. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui/nav.h"

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static double haversine_m(double la1, double lo1, double la2, double lo2, double *brg)
{
	double r = 3.14159265358979 / 180.0, p1 = la1 * r, p2 = la2 * r, dl = (lo2 - lo1) * r;
	double a = sin((p2 - p1) / 2) * sin((p2 - p1) / 2) + cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
	*brg = fmod(atan2(sin(dl) * cos(p2), cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl)) / r + 360.0, 360.0);
	return 6371000.0 * 2 * atan2(sqrt(a), sqrt(1 - a));
}

int main(void)
{
	nav_point_t p;
	CHECK(nav_count() == 0 && !nav_get(0, &p));
	CHECK(nav_save(410082000, 289784000) == 1 && nav_count() == 1 && nav_get(0, &p) && strcmp(p.name, "WP1") == 0);
	nav_save(399334000, 328597000);
	CHECK(nav_get(0, &p) && strcmp(p.name, "WP2") == 0 && p.lat_e7 == 399334000);
	for (int i = 0; i < 10; i++)
		nav_save(i, i);
	CHECK(nav_count() == NAV_WAYPOINTS && nav_get(0, &p) && strcmp(p.name, "WP12") == 0);
	CHECK(nav_get(NAV_WAYPOINTS - 1u, &p) && strcmp(p.name, "WP5") == 0);	/* the oldest kept */
	CHECK(nav_delete(0) && nav_count() == NAV_WAYPOINTS - 1u && nav_get(0, &p) && strcmp(p.name, "WP11") == 0);
	CHECK(!nav_delete(50));
	/* a name of its own (Tools > Map's START), cut to NAV_NAME_LEN */
	CHECK(nav_rename(0, "START") && nav_get(0, &p) && strcmp(p.name, "START") == 0 && p.lat_e7 == 8);
	CHECK(nav_rename(1, "LONGNAME99") && nav_get(1, &p) && strcmp(p.name, "LONGNAM") == 0);
	CHECK(!nav_rename(NAV_WAYPOINTS, "X"));
	nav_clear();
	CHECK(nav_count() == 0);

	/* Istanbul -> Ankara, standing */
	nav_way_t w;
	double brg, want = haversine_m(41.0082, 28.9784, 39.9334, 32.8597, &brg);
	nav_way(410082000, 289784000, 399334000, 328597000, 0, 0, &w);
	CHECK(fabs((double)w.meters - want) < want * 0.003 && abs((int)w.bearing - (int)lrint(brg)) <= 1);
	CHECK(!w.moving && w.eta_s == 0 && w.rel == w.bearing - (w.bearing > 180 ? 360 : 0));
	/* driving east-north-east at 90 km/h, course 80: the target is a bit to the right; ETA = distance / speed */
	nav_way(410082000, 289784000, 399334000, 328597000, 90, 80, &w);
	CHECK(w.moving && w.rel == (int16_t)(w.bearing - 80) && w.eta_s > 13500 && w.eta_s < 14500);
	/* heading west-north-west (300): the target (110) is behind, slightly right (+170) */
	nav_way(410082000, 289784000, 399334000, 328597000, 50, 300, &w);
	CHECK(w.moving && w.rel >= 165 && w.rel <= 175);
	/* unknown course or speed: standing */
	nav_way(410082000, 289784000, 399334000, 328597000, 0xFFFF, 80, &w);
	CHECK(!w.moving);
	printf("Plus waypoints and Go-To: store, Istanbul-Ankara great circle, relative bearing, ETA: %d failures\n",
	       failures);
	return failures ? 1 : 0;
}
