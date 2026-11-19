/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus battery history and time left (CONFIG_BATT_GRAPH, docs/plus.md "Battery graph"): one sample every 2 minutes,
 * the mean of the voltage readings in those 2 minutes as one byte (10 mV steps above 6.00 V: 6.01 .. 8.55 V), 96 of
 * them (3.2 hours) in a ring; and the time left from the slope of the last 30-60 minutes while discharging. Integer
 * and fixed point only; RAM only (gone at power off): 112 B. Portable (no RTOS): the host tests feed it synthetic
 * curves.
 *
 * Time left: a least squares line through the newest samples back to 60 minutes, or back to where the voltage rose (a
 * charger: the samples before it are another discharge), at least 30 minutes of them; the line's level now down to
 * the empty level (DM-32: 6.40 V, where its charge level reads 0 %) at its slope. None ("--") while that is not
 * known: under 30 minutes of samples since the last rise, the newest rise (charging), a slope that is not falling, or
 * readings scattered more than 30 mV RMS around the line (a load that keeps changing). Linear: a Li-ion pack's knee
 * near empty makes the last part go faster than shown. */
#ifndef PLUS_BATTGRAPH_H
#define PLUS_BATTGRAPH_H

#include <stdbool.h>
#include <stdint.h>

#define BATTG_SAMPLES		96u
#define BATTG_PERIOD_MS		120000u	/* a sample every 2 minutes */
#define BATTG_FEED_MS		1000u	/* readings averaged into a sample: at most one a second */
#define BATTG_FLOOR_MV		6000u	/* a sample byte is (mV - floor) / step, 1 .. 255 (0: no sample) */
#define BATTG_STEP_MV		10u
#define BATTG_EMPTY_MV		6400u	/* the DM-32's empty level (its 0 %, boards/dm32/power_dm32.c BAT_EMPTY_MV) */
#define BATTG_FIT_MIN		15u	/* samples the estimate needs: 30 minutes */
#define BATTG_FIT_MAX		30u	/* and uses at most: 60 minutes */
#define BATTG_RISE		3u	/* a rise of this many steps between two samples: a charger (30 mV) */
#define BATTG_RMS_MAX		3u	/* steps RMS around the line at most (30 mV) */
#define BATTG_NONE		(-1)	/* battg_minutes_left: not known */

typedef struct {
	uint8_t s[BATTG_SAMPLES];	/* the ring: s[head] is the next to write */
	uint8_t head, n;		/* n: samples in the ring */
	uint16_t cnt;			/* readings in the sample being made */
	uint32_t sum;			/* their sum, mV */
	uint32_t start_ms;		/* when the sample being made started (0: not started) */
	uint32_t fed_ms;		/* the last reading taken */
} battg_t;

void battg_reset(battg_t *g);
/* A reading (mV, 0 = none) at now_ms: averaged, and a sample stored every BATTG_PERIOD_MS */
void battg_feed(battg_t *g, uint16_t mv, uint32_t now_ms);
/* Sample age 0 = the newest .. n - 1; 0 = none. mV of a sample value */
uint8_t battg_sample(const battg_t *g, uint8_t age);
static inline uint16_t battg_mv(uint8_t v) { return v ? (uint16_t)(BATTG_FLOOR_MV + v * BATTG_STEP_MV) : 0u; }
/* Minutes to empty_mv at the recent slope, or BATTG_NONE (see above) */
int32_t battg_minutes_left(const battg_t *g, uint16_t empty_mv);
/* Why there is no estimate: 0 there is one, 1 too few samples, 2 charging (the newest rise or not falling),
 * 3 not steady */
uint8_t battg_why_none(const battg_t *g, uint16_t empty_mv);

#endif
