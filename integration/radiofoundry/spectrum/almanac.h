/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus sun and moon almanac (docs/plus.md): sunrise, sunset and civil twilight for the GPS position and date
 * (the sunrise equation, about a minute off), the moon's age and lit fraction (mean synodic month). Pure math. */
#ifndef PLUS_ALMANAC_H
#define PLUS_ALMANAC_H

#include <stdbool.h>
#include <stdint.h>

#define ALMANAC_NONE	(-1)	/* the sun doesn't cross that altitude that day (polar day or night) */

typedef struct {
	int16_t rise_min, set_min;	/* minutes after local midnight, ALMANAC_NONE */
	int16_t dawn_min, dusk_min;	/* civil twilight (sun 6 degrees down) */
	uint16_t moon_age10;		/* days since the new moon, x10 (0 .. 295) */
	uint8_t moon_lit;		/* lit fraction, percent */
	uint8_t moon_phase;		/* 0 new, 1 waxing crescent, 2 first quarter, 3 waxing gibbous, 4 full, 5 waning
					 * gibbous, 6 last quarter, 7 waning crescent */
	bool waxing;
} almanac_t;

/* ddmmyy as GPS RMC gives it (UTC date), the position in degrees x 1e7, the local offset in minutes: false for an
 * impossible date */
bool almanac_compute(uint32_t ddmmyy, int32_t lat_e7, int32_t lon_e7, int16_t tz_min, almanac_t *out);

#endif
