/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus spectrum sweep engine (integration/radiofoundry/spectrum): point layout, settle, peak hold, close call
 * hit, unknown readings, the idle stop and snapshots. */
#include <stdio.h>
#include <string.h>
#include "radio/spectrum.h"

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* One full sweep: every point reads `floor` except `hit_at`, which reads `hit` */
static uint32_t sweep(uint32_t *now, int16_t floor, int hit_at, int16_t hit, bool look)
{
	spectrum_view_t v;
	uint32_t last = 0;
	for (unsigned i = 0; i < 64u && spectrum_on(); i++) {
		if (look)
			spectrum_snapshot(&v);
		*now += SPECTRUM_SETTLE_MS;
		uint32_t hz = spectrum_service((int)i == hit_at ? hit : floor, *now);
		if (hz)
			last = hz;
		if (look && spectrum_sweeps() && i + 1u >= 32u)
			break;
	}
	return last;
}

int main(void)
{
	uint32_t now = 1000;
	spectrum_view_t v;
	CHECK(!spectrum_on() && !spectrum_snapshot(&v));

	/* 500 kHz span: 32 points, 15.625 kHz apart, centered */
	uint32_t first = spectrum_start(433500000u, 500000u, now);
	CHECK(spectrum_on() && first == 433500000u - 16u * 15625u);
	CHECK(spectrum_service(-120, now + SPECTRUM_SETTLE_MS - 1u) == 0);	/* still settling */
	uint32_t next = spectrum_service(-120, now + SPECTRUM_SETTLE_MS);
	CHECK(next == first + 15625u);
	now += SPECTRUM_SETTLE_MS;

	/* A strong signal at point 20 (counting the point already read): the close call hit after the sweep */
	sweep(&now, -121, 19, -70, true);
	CHECK(spectrum_snapshot(&v) && v.points == 32 && v.sweeps >= 1);
	CHECK(v.hit == 20 && v.hit_dbm == -70 && v.floor_dbm <= -120 && v.floor_dbm >= -121);
	CHECK(spectrum_point_hz(&v, v.hit) == 433500000u + 4u * 15625u);
	CHECK(v.peak_dbm[20] == -70);

	/* The signal goes: no hit, but the peak hold keeps it */
	sweep(&now, -121, -1, 0, true);
	CHECK(spectrum_snapshot(&v) && v.hit == SPECTRUM_POINTS_MAX && v.peak_dbm[20] == -70 && v.dbm[20] == -121);

	/* Weak bumps are no hit: 12 dB over the floor but under -105 dBm */
	sweep(&now, -125, 5, -110, true);
	CHECK(spectrum_snapshot(&v) && v.hit == SPECTRUM_POINTS_MAX);

	/* "Unknown" (0) reads as no signal */
	sweep(&now, 0, -1, 0, true);
	CHECK(spectrum_snapshot(&v) && v.dbm[3] == -140 && v.hit == SPECTRUM_POINTS_MAX);

	/* A wide span uses 64 points; a narrow one keeps a 2.5 kHz step at least */
	spectrum_start(145000000u, 2500000u, now);
	CHECK(spectrum_snapshot(&v) == false);
	memset(&v, 0, sizeof v);
	spectrum_service(-120, now += SPECTRUM_SETTLE_MS);
	CHECK(spectrum_snapshot(&v) && v.points == 64 && v.step_hz == 2500000u / 64u);
	spectrum_start(145000000u, 10000u, now);
	spectrum_service(-120, now += SPECTRUM_SETTLE_MS);
	CHECK(spectrum_snapshot(&v) && v.step_hz == 2500u);

	/* Nobody looks (the UI left the screen): it stops by itself */
	uint32_t t0 = now;
	while (spectrum_on() && now - t0 < 10000u)
		spectrum_service(-120, now += 10u);
	CHECK(!spectrum_on() && now - t0 >= SPECTRUM_IDLE_MS && now - t0 < SPECTRUM_IDLE_MS + 50u);
	CHECK(spectrum_service(-120, now + 100u) == 0);

	/* Stop on request */
	spectrum_start(433500000u, 250000u, now);
	CHECK(spectrum_on());
	spectrum_stop();
	CHECK(!spectrum_on() && !spectrum_snapshot(&v));

	printf("Plus spectrum: layout, settle, peak hold, close call hit, unknown readings, idle stop: %d failures\n",
	       failures);
	return failures ? 1 : 0;
}
