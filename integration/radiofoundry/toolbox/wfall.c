/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus spectrum waterfall (wfall.h): the portable part. Installed as core/ui/wfall.c. */
#include "config.h"
#if CONFIG_SPECTRUM_WF
#include <string.h>
#include "ui/wfall.h"

void wf_reset(wf_t *w)
{
	w->head = w->n = w->points = 0;
	w->sweeps = 0;
}

uint8_t wf_level(int16_t dbm, int16_t floor_dbm)
{
	if (dbm == 0)
		return 0;	/* not read yet */
	int d = (int)dbm - (int)floor_dbm;
	if (d < WF_DB_LEVEL)
		return 0;
	d /= WF_DB_LEVEL;
	return (uint8_t)(d > 15 ? 15 : d);
}

#if CONFIG_SPECTRUM_COLOR
/* Slots 1..7 on a dark background (the dark and high contrast themes) and on a light one; 0: the theme's colour (slot
 * 4 its RX green, slot 7 its TX red) */
static const color_t ramp[2][WF_SLOTS] = {
	{ 0, RGB565(16, 40, 168), RGB565(16, 120, 255), RGB565(0, 216, 232), 0, RGB565(248, 232, 0), RGB565(255, 128, 0),
	  0 },
	{ 0, RGB565(168, 204, 255), RGB565(32, 104, 232), RGB565(0, 160, 192), 0, RGB565(200, 168, 0),
	  RGB565(232, 104, 0), 0 },
};

void wf_ramp(color_t slot[WF_SLOTS], color_t bg, color_t rx, color_t tx)
{
	const color_t *r = ramp[((bg >> 5) & 0x3Fu) >= 32u];	/* a light background: green over half */
	for (uint8_t s = 0; s < WF_SLOTS; s++)
		slot[s] = r[s];
	slot[0] = bg;
	slot[4] = rx;
	slot[7] = tx;
}
#endif

uint8_t wf_slot(uint8_t level)
{
	static const uint8_t slot[16] = { 0, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 5, 6, 6, 7 };
	return slot[level & 15u];
}

void wf_push(wf_t *w, const int16_t *dbm, uint8_t points, int16_t floor_dbm, uint32_t sweeps)
{
	if (w->buf == NULL || points == 0)
		return;
	if (points > WF_POINTS)
		points = WF_POINTS;
	if (points != w->points) {
		w->head = w->n = 0;
		w->points = points;
	}
	uint8_t *row = w->buf + (size_t)w->head * WF_ROW_BYTES;
	memset(row, 0, WF_ROW_BYTES);
	for (uint8_t i = 0; i < points; i++) {
		uint8_t l = wf_level(dbm[i], floor_dbm);
		row[i / 2u] |= (uint8_t)(i & 1u ? l << 4 : l);
	}
	w->head = (uint8_t)((w->head + 1u) % WF_ROWS);
	if (w->n < WF_ROWS)
		w->n++;
	w->sweeps = sweeps;
}

uint8_t wf_get(const wf_t *w, uint8_t r, uint8_t i)
{
	if (w->buf == NULL || r >= w->n || i >= w->points)
		return 0;
	uint8_t at = (uint8_t)((w->head + WF_ROWS - 1u - r) % WF_ROWS);
	uint8_t b = w->buf[(size_t)at * WF_ROW_BYTES + i / 2u];
	return (uint8_t)(i & 1u ? b >> 4 : b & 15u);
}
#endif
