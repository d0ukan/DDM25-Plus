/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus fox hunt tone (integration/radiofoundry/spectrum/fox.c): pitch and rate grow with the signal, silence
 * under the floor, the virtual attenuator moves the scale, the tone keeps its rate and stops when unwatched. */
#include <stdio.h>
#include "radio/fox.h"

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

int main(void)
{
	uint16_t period, p2;
	CHECK(fox_tone_hz(-130, 0, &period) == 0 && period == 1000);	/* under the floor: silent */
	CHECK(fox_tone_hz(0, 0, &period) == 0);				/* unknown: silent */
	uint16_t weak = fox_tone_hz(-120, 0, &period), strong = fox_tone_hz(-60, 0, &p2);
	CHECK(weak >= 400 && strong > weak && p2 < period);
	CHECK(fox_tone_hz(-45, 0, &period) == 2400 && period == 120);	/* top of the scale */
	CHECK(fox_tone_hz(-20, 0, &period) == 2400);			/* saturates ... */
	CHECK(fox_tone_hz(-20, 30, &p2) < 2400 && p2 > period);		/* ... unless attenuated */
	CHECK(fox_tone_hz(-100, 30, &period) == 0);			/* attenuated below the floor */

	/* Service: off by default, a tone at once, then at the signal's rate */
	uint32_t now = 1000;
	CHECK(!fox_on() && fox_service(-60, now) == 0);
	fox_set(true, 0, now);
	CHECK(fox_on() && fox_att() == 0);
	fox_seen();
	CHECK(fox_service(-60, now) == strong);
	fox_tone_hz(-60, 0, &period);
	CHECK(fox_service(-60, now + period - 1u) == 0 && fox_service(-60, now + period) == strong);
	fox_set(true, 50, now);
	CHECK(fox_att() == 30);

	/* Unwatched (the screen was left): the tone stops for good */
	now = 5000;
	fox_set(true, 0, now);
	for (uint32_t t = now; t < now + 20000u && fox_on(); t += 50u)
		fox_service(-60, t);
	CHECK(!fox_on());
	fox_set(true, 0, now);
	for (uint32_t t = now; t < now + 20000u; t += 50u) {
		fox_seen();
		fox_service(-60, t);
	}
	CHECK(fox_on());	/* watched: still on */
	fox_set(false, 0, now);
	CHECK(!fox_on() && fox_service(-60, now + 5000u) == 0);
	printf("Plus fox hunt: pitch/rate by strength, floor, attenuator, rate, idle stop: %d failures\n", failures);
	return failures ? 1 : 0;
}
