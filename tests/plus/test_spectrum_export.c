/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus PC live panel's spectrum query (debug link 0x21 [13], spectrum_export): the bytes of a running sweep (the
 * header, the points, the hit, negative dBm as int16 little endian), "unavailable" (0) without a sweep or before its
 * first point, a buffer too small, a write under way; and that it is not a look at the sweep: a PC asking all the time
 * does not keep a sweep alive that the screen left (the idle stop after SPECTRUM_IDLE_MS still comes), while the
 * screen's spectrum_snapshot does. Built with -DCONFIG_SPECTRUM=1 -DCONFIG_PC_LIVE=1. */
#include <stdio.h>
#include <string.h>
#include "radio/spectrum.h"

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static int16_t le16(const uint8_t *p) { return (int16_t)(p[0] | p[1] << 8); }

int main(void)
{
	uint8_t out[1023];
	uint32_t now = 1000;
	CHECK(spectrum_export(out, sizeof out) == 0);		/* no sweep */
	spectrum_start(433500000u, 500000u, now);
	CHECK(spectrum_export(out, sizeof out) == 0);		/* nothing read yet */
	/* One full sweep: -121 dBm everywhere, -70 at point 20; the PC asks between the points (no snapshot) */
	for (unsigned i = 0; i < 32u; i++) {
		now += SPECTRUM_SETTLE_MS;
		spectrum_service(i == 20u ? -70 : -121, now);
		spectrum_export(out, sizeof out);
	}
	size_t len = spectrum_export(out, sizeof out);
	CHECK(len == SPECTRUM_EXPORT_HEAD + 2u * 32u && SPECTRUM_EXPORT_HEAD == 20u);
	CHECK(out[0] == 1 && out[1] == 1 && out[2] == 32 && out[3] == 20);
	CHECK(le32(out + 4) == 433500000u && le32(out + 8) == 15625u && le32(out + 12) == 1);
	CHECK(le16(out + 16) == -121 && le16(out + 18) == -70);
	CHECK(le16(out + 20) == -121 && le16(out + 20 + 2 * 20) == -70 && le16(out + 20 + 2 * 31) == -121);
	/* The same view the screen reads */
	spectrum_view_t v;
	CHECK(spectrum_snapshot(&v) && v.hit == 20 && v.floor_dbm == -121 && v.points == 32);
	/* Too small a buffer: refused */
	CHECK(spectrum_export(out, SPECTRUM_EXPORT_HEAD + 2u * SPECTRUM_POINTS_MAX - 1u) == 0);
	/* No hit: 0xFF */
	for (unsigned i = 0; i < 32u; i++) {
		now += SPECTRUM_SETTLE_MS;
		spectrum_service(-121, now);
		spectrum_snapshot(&v);
	}
	CHECK(spectrum_export(out, sizeof out) > 0 && out[3] == 0xFF);

	/* Not a look: the screen stops reading, the PC goes on asking every 100 ms; the sweep stops by itself after
	 * SPECTRUM_IDLE_MS as it would without the PC */
	uint32_t left = now;
	while (spectrum_on() && now - left < 10000u) {
		now += SPECTRUM_SETTLE_MS;
		spectrum_service(-121, now);
		if ((now / SPECTRUM_SETTLE_MS) % 3u == 0)
			spectrum_export(out, sizeof out);
	}
	CHECK(!spectrum_on() && now - left >= SPECTRUM_IDLE_MS && now - left < SPECTRUM_IDLE_MS + 200u);
	CHECK(spectrum_export(out, sizeof out) == 0);		/* stopped: unavailable */
	/* ... while the screen's own reads keep it on */
	spectrum_start(145000000u, 2500000u, now);
	left = now;
	for (unsigned k = 0; k < 400u; k++) {
		now += SPECTRUM_SETTLE_MS;
		spectrum_service(-118, now);
		spectrum_snapshot(&v);
	}
	CHECK(spectrum_on() && now - left > SPECTRUM_IDLE_MS);
	len = spectrum_export(out, sizeof out);
	CHECK(len == SPECTRUM_EXPORT_HEAD + 2u * 64u && out[2] == 64 && le32(out + 8) == 2500000u / 64u);
	/* The export never starts or retunes a sweep: stopped stays stopped */
	spectrum_stop();
	for (int k = 0; k < 5; k++)
		CHECK(spectrum_export(out, sizeof out) == 0 && !spectrum_on());
	printf("spectrum export: %d failures\n", failures);
	return failures != 0;
}
