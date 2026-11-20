/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM25 Plus find my friend (core/ui/follow.c): one station followed by callsign; a newer position taken, another
 * station's ignored, the last one kept when the station is not heard; its age and the 10 minute stale mark. Built with
 * -DCONFIG_NAV_FOLLOW=1. */
#include <stdio.h>
#include <string.h>
#include "ui/follow.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void)
{
	follow_t f;
	follow_clear(&f);
	CHECK(!follow_on(&f) && follow_age_ms(&f, 5000) == 0 && !follow_stale(&f, 99999999u));
	CHECK(!follow_update(&f, "TA1ABC-9", true, 1, 2, 100));	/* nothing followed: nothing taken */

	follow_set(&f, "TA1ABC-9");
	CHECK(follow_on(&f) && !f.has_pos && strcmp(f.call, "TA1ABC-9") == 0);
	CHECK(follow_age_ms(&f, 5000) == 0 && !follow_stale(&f, 5000 + FOLLOW_STALE_MS));	/* never heard: no age */

	/* Its first report without a position: the time, no position */
	CHECK(follow_update(&f, "TA1ABC-9", false, 0, 0, 1000));
	CHECK(!f.has_pos && f.heard_ms == 1000);
	/* Then a position */
	CHECK(follow_update(&f, "TA1ABC-9", true, 399200000, 328500000, 2000));
	CHECK(f.has_pos && f.lat == 399200000 && f.lon == 328500000 && f.heard_ms == 2000);
	/* The same again: no change (no redraw) */
	CHECK(!follow_update(&f, "TA1ABC-9", true, 399200000, 328500000, 2000));
	/* Another station, even one whose call starts the same: ignored */
	CHECK(!follow_update(&f, "TA1ABC-1", true, 1, 1, 9000));
	CHECK(!follow_update(&f, "TA1ABC", true, 1, 1, 9000));
	CHECK(!follow_update(&f, "TA1ABC-99", true, 1, 1, 9000));
	CHECK(f.lat == 399200000 && f.heard_ms == 2000);
	/* Moved: the new position, live */
	CHECK(follow_update(&f, "TA1ABC-9", true, 399300000, 328600000, 32000));
	CHECK(f.lat == 399300000 && f.lon == 328600000 && f.heard_ms == 32000);
	/* A status without a position (the engine keeps the last position): the time moves, the position stays */
	CHECK(follow_update(&f, "TA1ABC-9", false, 0, 0, 40000));
	CHECK(f.has_pos && f.lat == 399300000 && f.heard_ms == 40000);

	/* Age and stale: under 10 minutes fresh, from 10 minutes stale; nothing heard, the position kept */
	CHECK(follow_age_ms(&f, 40000) == 0 && !follow_stale(&f, 40000));
	CHECK(follow_age_ms(&f, 40000 + 180000) == 180000 && !follow_stale(&f, 40000 + 180000));
	CHECK(!follow_stale(&f, 40000 + FOLLOW_STALE_MS - 1));
	CHECK(follow_stale(&f, 40000 + FOLLOW_STALE_MS));
	CHECK(follow_stale(&f, 40000 + 3600000u) && f.has_pos && f.lat == 399300000);
	/* Heard again: fresh */
	CHECK(follow_update(&f, "TA1ABC-9", true, 399300000, 328600000, 40000 + 700000));
	CHECK(!follow_stale(&f, 40000 + 700000 + 1000));
	/* The millisecond counter wraps: the age is still right */
	follow_set(&f, "TA1ABC-9");
	CHECK(follow_update(&f, "TA1ABC-9", true, 1, 2, 0xFFFFF000u));
	CHECK(follow_age_ms(&f, 0x00001000u) == 0x2000u && !follow_stale(&f, 0x00001000u));

	/* A long callsign is cut to fit; a new target replaces the old one entirely */
	follow_set(&f, "TA1ABCDEFGHIJ-15");
	CHECK(strlen(f.call) == FOLLOW_CALL_LEN - 1u && !f.has_pos);
	follow_set(&f, "TA2XYZ");
	CHECK(strcmp(f.call, "TA2XYZ") == 0 && !f.has_pos && f.heard_ms == 0);
	follow_clear(&f);
	CHECK(!follow_on(&f) && !follow_update(&f, "TA2XYZ", true, 1, 1, 5));

	printf("follow: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
