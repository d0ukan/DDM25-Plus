/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus almanac (almanac.c) against published times: Istanbul and Ankara sunrise/sunset (UTC+3), a polar day,
 * known full and new moons. Tolerance: 3 minutes (the sunrise equation's own accuracy is about one). */
#include <stdio.h>
#include <stdlib.h>
#include "ui/almanac.h"

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static bool near(int16_t got, int h, int m)
{
	int want = h * 60 + m;
	if (abs(got - want) > 3) {
		printf("  got %02d:%02d want %02d:%02d\n", got / 60, got % 60, h, m);
		return false;
	}
	return true;
}

int main(void)
{
	almanac_t a;
	/* Istanbul (41.0082 N, 28.9784 E), 21 June 2026: rise 05:32, set 20:39 (UTC+3) */
	CHECK(almanac_compute(210626u, 410082000, 289784000, 180, &a));
	CHECK(near(a.rise_min, 5, 32) && near(a.set_min, 20, 39));
	CHECK(a.dawn_min < a.rise_min && a.dusk_min > a.set_min && a.rise_min - a.dawn_min > 25 && a.rise_min - a.dawn_min < 40);
	/* Ankara (39.9334 N, 32.8597 E), 21 December 2026: rise 08:07, set 17:27 (UTC+3; solar noon 12:47) */
	CHECK(almanac_compute(211226u, 399334000, 328597000, 180, &a));
	CHECK(near(a.rise_min, 8, 7) && near(a.set_min, 17, 27));
	/* Tromso in June: the sun never sets */
	CHECK(almanac_compute(210626u, 696492000, 189553000, 120, &a));
	CHECK(a.rise_min == ALMANAC_NONE && a.set_min == ALMANAC_NONE);
	/* Moon: full 23 April 2024 23:49 UTC, new 8 April 2024 18:21 UTC */
	CHECK(almanac_compute(240424u, 410082000, 289784000, 180, &a));
	CHECK(a.moon_lit >= 95 && a.moon_phase == 4);
	CHECK(almanac_compute(80424u, 410082000, 289784000, 180, &a));
	CHECK(a.moon_lit <= 5 && (a.moon_phase == 0) && (a.moon_age10 < 15 || a.moon_age10 > 280));
	CHECK(almanac_compute(150424u, 410082000, 289784000, 180, &a));
	CHECK(a.waxing && a.moon_phase == 2 && a.moon_lit > 35 && a.moon_lit < 65);	/* first quarter 15 April */
	CHECK(!almanac_compute(320126u, 0, 0, 0, &a) && !almanac_compute(11326u, 0, 0, 0, &a));
	printf("Plus almanac: Istanbul/Ankara sun times, polar day, moon phases, bad dates: %d failures\n", failures);
	return failures ? 1 : 0;
}
