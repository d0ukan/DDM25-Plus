/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus battery history and time left (battgraph.h). Integer only. */
#include "config.h"
#if CONFIG_BATT_GRAPH
#include <string.h>
#include "ui/battgraph.h"

void battg_reset(battg_t *g)
{
	memset(g, 0, sizeof *g);
}

static uint8_t to_byte(uint32_t mv)
{
	if (mv <= BATTG_FLOOR_MV + BATTG_STEP_MV / 2u)
		return 1;
	uint32_t v = (mv - BATTG_FLOOR_MV + BATTG_STEP_MV / 2u) / BATTG_STEP_MV;
	return v > 255u ? 255u : v < 1u ? 1u : (uint8_t)v;
}

void battg_feed(battg_t *g, uint16_t mv, uint32_t now_ms)
{
	if (mv == 0)
		return;
	if (g->start_ms != 0 && now_ms - g->fed_ms < BATTG_FEED_MS)
		return;
	if (g->start_ms == 0)
		g->start_ms = now_ms ? now_ms : 1u;
	g->fed_ms = now_ms;
	g->sum += mv;
	g->cnt++;
	if (now_ms - g->start_ms < BATTG_PERIOD_MS)
		return;
	g->s[g->head] = to_byte((g->sum + g->cnt / 2u) / g->cnt);
	g->head = (uint8_t)((g->head + 1u) % BATTG_SAMPLES);
	if (g->n < BATTG_SAMPLES)
		g->n++;
	g->sum = 0;
	g->cnt = 0;
	g->start_ms += BATTG_PERIOD_MS;
	if (now_ms - g->start_ms >= BATTG_PERIOD_MS)
		g->start_ms = now_ms ? now_ms : 1u;	/* readings stopped for a while: start the next period now */
}

uint8_t battg_sample(const battg_t *g, uint8_t age)
{
	if (age >= g->n)
		return 0;
	return g->s[(g->head + BATTG_SAMPLES - 1u - age) % BATTG_SAMPLES];
}

/* The fit: minutes left (>= 0) or BATTG_NONE with the reason in *why */
static int32_t estimate(const battg_t *g, uint16_t empty_mv, uint8_t *why)
{
	/* The newest samples back to BATTG_FIT_MAX or to the last rise (a charger) */
	uint8_t m = 0;
	while (m < g->n && m < BATTG_FIT_MAX) {
		if (m > 0 && battg_sample(g, (uint8_t)(m - 1u)) >= battg_sample(g, m) + BATTG_RISE)
			break;		/* sample m - 1 (newer) rose over m: the discharge starts at m - 1 */
		m++;
	}
	if (g->n >= 2 && battg_sample(g, 0) >= battg_sample(g, 1) + BATTG_RISE) {
		*why = 2;	/* rising now: charging */
		return BATTG_NONE;
	}
	if (m < BATTG_FIT_MIN) {
		*why = 1;
		return BATTG_NONE;
	}
	/* Least squares over x = 0 (oldest) .. m - 1 (newest), y in steps */
	int64_t sx = 0, sy = 0, sxx = 0, sxy = 0;
	for (uint8_t k = 0; k < m; k++) {
		int64_t x = k, y = battg_sample(g, (uint8_t)(m - 1u - k));
		sx += x;
		sy += y;
		sxx += x * x;
		sxy += x * y;
	}
	int64_t num = (int64_t)m * sxy - sx * sy, den = (int64_t)m * sxx - sx * sx;
	if (num >= 0) {
		*why = 2;	/* not falling: charging, or on an external supply */
		return BATTG_NONE;
	}
	/* Q8: the slope in steps a sample, the line's value at x */
	int64_t slope_q8 = num * 256 / den;		/* < 0 */
	int64_t mean_q8 = sy * 256 / m;
	int64_t sse = 0;
	for (uint8_t k = 0; k < m; k++) {
		int64_t pred = mean_q8 + slope_q8 * (2 * (int64_t)k - (m - 1)) / 2;
		int64_t err = (int64_t)battg_sample(g, (uint8_t)(m - 1u - k)) * 256 - pred;
		sse += err * err;
	}
	if (sse / m > (int64_t)(BATTG_RMS_MAX * 256) * (BATTG_RMS_MAX * 256)) {
		*why = 3;
		return BATTG_NONE;
	}
	if (slope_q8 == 0) {
		*why = 2;
		return BATTG_NONE;
	}
	*why = 0;
	int64_t now_q8 = mean_q8 + slope_q8 * (m - 1) / 2;
	int64_t empty_q8 = empty_mv > BATTG_FLOOR_MV ? ((int64_t)empty_mv - BATTG_FLOOR_MV) * 256 / BATTG_STEP_MV : 0;
	if (now_q8 <= empty_q8)
		return 0;
	int64_t samples_q8 = (now_q8 - empty_q8) * 256 / -slope_q8;	/* samples to go, Q8 */
	int64_t minutes = samples_q8 * (BATTG_PERIOD_MS / 60000u) / 256;
	return minutes > 0x7FFFFFFF ? 0x7FFFFFFF : (int32_t)minutes;
}

int32_t battg_minutes_left(const battg_t *g, uint16_t empty_mv)
{
	uint8_t why;
	return estimate(g, empty_mv, &why);
}

uint8_t battg_why_none(const battg_t *g, uint16_t empty_mv)
{
	uint8_t why;
	estimate(g, empty_mv, &why);
	return why;
}
#endif
