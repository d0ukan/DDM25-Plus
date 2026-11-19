/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus battery history and time left (integration/radiofoundry/battery/battgraph.c) on synthetic curves: the
 * 2 minute samples (the mean of one reading a second, readings faster than that ignored, a gap in the readings), the
 * byte scale and its limits, the ring after 3.2 hours; the estimate on a straight discharge (several slopes, against
 * the exact answer), with noise inside the limit, too few samples, charging (a rise, a flat or rising line), a
 * discharge after a charge (only the part after the rise counts), a load that keeps changing, already empty. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui/battgraph.h"

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static battg_t g;
static uint32_t now;
static double level;	/* mV */

/* minutes of readings, one a second: from from_mv (< 0: where the last run ended) at rate mV an hour, +-noise mV */
static void run(uint32_t minutes, double from_mv, double rate_mv_h, int noise)
{
	if (from_mv >= 0)
		level = from_mv;
	for (uint32_t t = 0; t < minutes * 60u; t++) {
		now += 1000;
		int v = (int)(level + 0.5);
		if (noise)
			v += (rand() % (2 * noise + 1)) - noise;
		battg_feed(&g, (uint16_t)v, now);
		level += rate_mv_h / 3600.0;
	}
}

static void start(void)
{
	battg_reset(&g);
	now = 5000;
	level = 0;
	srand(1);
}

static void put(uint8_t v)
{
	g.s[g.head] = v;
	g.head = (uint8_t)((g.head + 1u) % BATTG_SAMPLES);
	g.n++;
}

/* minutes from level_mv down to 6.40 V at rate mV an hour */
static int32_t exact(double level_mv, double rate)
{
	return (int32_t)((level_mv - BATTG_EMPTY_MV) / -rate * 60.0);
}

static void near(int32_t got, int32_t want, int32_t tol, const char *what)
{
	printf("%s: %d min left (exact %d)\n", what, (int)got, (int)want);
	CHECK(got >= want - tol && got <= want + tol);
}

int main(void)
{
	/* Samples: the mean of the readings of each 2 minutes (120 readings), 10 mV a step above 6.00 V */
	start();
	run(1, 7800, 0, 0);
	CHECK(g.n == 0 && battg_sample(&g, 0) == 0);
	run(1, -1, 0, 0);
	CHECK(g.n == 0);
	battg_feed(&g, 7800, now += 1000);	/* the 121st reading: 120 s after the first */
	CHECK(g.n == 1 && battg_sample(&g, 0) == 180 && battg_mv(180) == 7800);
	/* Readings faster than one a second count once a second */
	for (int k = 0; k < 10; k++)
		battg_feed(&g, 9000, now + (uint32_t)k * 50u + 1u);
	run(2, 7800, 0, 0);
	CHECK(g.n == 2 && battg_sample(&g, 0) == 180);
	/* The mean: half of the period at 7.80 V, half at 7.70 V */
	run(1, 7800, 0, 0);
	run(1, 7700, 0, 0);
	CHECK(g.n == 3 && battg_sample(&g, 0) == 175);
	/* No reading (0) is skipped */
	uint8_t n = g.n;
	battg_feed(&g, 0, now + 300000u);
	CHECK(g.n == n);
	/* Limits: under the floor 1, over the top 255 */
	start();
	run(2, 5000, 0, 0);
	run(2, 9000, 0, 0);
	run(1, 9000, 0, 0);
	CHECK(g.n == 2 && battg_sample(&g, 1) == 1 && battg_sample(&g, 0) == 255);
	/* A gap of 10 minutes in the readings: one sample, then the next period starts with the readings again */
	start();
	run(3, 7800, 0, 0);
	CHECK(g.n == 1);
	now += 600000;
	run(1, -1, 0, 0);
	CHECK(g.n == 2);	/* the period that was open when the readings stopped */
	run(2, -1, 0, 0);
	CHECK(g.n == 3);
	/* The ring keeps the last 96 (3.2 h): the oldest goes */
	start();
	run(200, 8200, -100, 0);
	CHECK(g.n == BATTG_SAMPLES);
	CHECK(battg_sample(&g, 0) < battg_sample(&g, BATTG_SAMPLES - 1));
	CHECK(battg_sample(&g, BATTG_SAMPLES) == 0);

	/* A straight discharge, 100 mV an hour from 8.00 V: after 29 minutes too few samples, after 61 an estimate */
	start();
	run(29, 8000, -100, 0);
	CHECK(battg_minutes_left(&g, BATTG_EMPTY_MV) == BATTG_NONE && battg_why_none(&g, BATTG_EMPTY_MV) == 1);
	run(32, -1, -100, 0);
	CHECK(battg_why_none(&g, BATTG_EMPTY_MV) == 0);
	near(battg_minutes_left(&g, BATTG_EMPTY_MV), exact(level, -100), 45, "100 mV/h from 8.00 V, after 1 h");
	/* Faster: 300 mV an hour from 7.20 V */
	start();
	run(40, 7200, -300, 0);
	near(battg_minutes_left(&g, BATTG_EMPTY_MV), exact(level, -300), 8, "300 mV/h from 7.20 V, after 40 min");
	/* With +-20 mV reading noise */
	start();
	run(60, 7800, -200, 20);
	near(battg_minutes_left(&g, BATTG_EMPTY_MV), exact(level, -200), 20, "200 mV/h from 7.80 V, noise 20 mV, 1 h");
	/* Slow: 20 mV an hour from 8.10 V: many hours (only 2 steps in an hour: a rough figure) */
	start();
	run(60, 8100, -20, 0);
	int32_t m = battg_minutes_left(&g, BATTG_EMPTY_MV);
	printf("20 mV/h from 8.10 V, 1 h: %d min left (exact %d)\n", (int)m, (int)exact(level, -20));
	CHECK(m > 40 * 60);
	/* Already at empty: 0 */
	start();
	run(60, 6450, -100, 0);
	CHECK(battg_minutes_left(&g, BATTG_EMPTY_MV) == 0);

	/* Charging: rising voltage, then flat */
	start();
	run(60, 7400, 400, 0);
	CHECK(battg_minutes_left(&g, BATTG_EMPTY_MV) == BATTG_NONE && battg_why_none(&g, BATTG_EMPTY_MV) == 2);
	start();
	run(60, 7800, 0, 0);
	CHECK(battg_minutes_left(&g, BATTG_EMPTY_MV) == BATTG_NONE && battg_why_none(&g, BATTG_EMPTY_MV) == 2);
	/* A discharge, a charger for 10 minutes, then off it again: only the part after the rise counts, and it needs its
	 * own 30 minutes */
	start();
	run(60, 7600, -200, 0);
	run(10, -1, 1800, 0);
	CHECK(battg_why_none(&g, BATTG_EMPTY_MV) == 2);
	run(20, -1, -150, 0);
	CHECK(battg_why_none(&g, BATTG_EMPTY_MV) == 1);
	run(22, -1, -150, 0);
	near(battg_minutes_left(&g, BATTG_EMPTY_MV), exact(level, -150), 40, "after a charge, 150 mV/h");
	/* A load that keeps changing: 300 mV up and down every sample (each rise looks like a charger): none */
	start();
	for (int k = 0; k < 16; k++)
		run(2, (k & 1) ? 7500 : 7800, -100, 0);
	CHECK(battg_minutes_left(&g, BATTG_EMPTY_MV) == BATTG_NONE);
	/* Falling, but scattered over 30 mV RMS around the line without any rise of a charger (a sawtooth: up 20 mV a
	 * sample five times, then down 100 mV): not steady */
	start();
	for (int k = 0; k < 30; k++)
		put((uint8_t)(200 - k / 2 + (k % 6) * 2 - 5));
	CHECK(battg_minutes_left(&g, BATTG_EMPTY_MV) == BATTG_NONE && battg_why_none(&g, BATTG_EMPTY_MV) == 3);
	/* The same falling line without the sawtooth: half a step a sample = 25 mV an hour... from 7.86 V */
	start();
	for (int k = 0; k < 30; k++)
		put((uint8_t)(200 - k / 2));
	m = battg_minutes_left(&g, BATTG_EMPTY_MV);
	CHECK(battg_why_none(&g, BATTG_EMPTY_MV) == 0 && m > 0);
	printf("battery graph: %d failures\n", failures);
	return failures != 0;
}
