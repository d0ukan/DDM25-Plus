/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus fox hunt tone logic (fox.h). */
#include "config.h"
#if CONFIG_FOX
#include "radio/fox.h"

static bool on;
static uint8_t att;
static uint32_t next_ms, seen_ms;
static volatile uint32_t seen, seen_last;

uint16_t fox_tone_hz(int16_t rssi_dbm, uint8_t att_db, uint16_t *period_ms)
{
	int eff = (rssi_dbm == 0 ? -140 : rssi_dbm) - att_db;
	if (eff < FOX_FLOOR_DBM) {
		*period_ms = 1000u;	/* look again in a second, silently */
		return 0;
	}
	if (eff > FOX_TOP_DBM)
		eff = FOX_TOP_DBM;
	int span = FOX_TOP_DBM - FOX_FLOOR_DBM, x = eff - FOX_FLOOR_DBM;	/* 0 .. span */
	*period_ms = (uint16_t)(700 - 580 * x / span);			/* 700 ms weak .. 120 ms strong */
	return (uint16_t)(400 + 2000 * x / span);			/* 400 Hz weak .. 2400 Hz strong */
}

void fox_set(bool want, uint8_t att_db, uint32_t now_ms)
{
	on = want;
	att = att_db > 30u ? 30u : att_db;
	next_ms = seen_ms = now_ms;
	seen_last = seen;
}

bool fox_on(void)
{
	return on;
}

uint8_t fox_att(void)
{
	return att;
}

void fox_seen(void)
{
	seen++;
}

uint16_t fox_service(int16_t rssi_dbm, uint32_t now_ms)
{
	if (!on)
		return 0;
	if (seen != seen_last) {
		seen_last = seen;
		seen_ms = now_ms;
	} else if (now_ms - seen_ms >= FOX_IDLE_MS) {
		on = false;	/* the screen was left: no tone forever */
		return 0;
	}
	if ((int32_t)(now_ms - next_ms) < 0)
		return 0;
	uint16_t period;
	uint16_t hz = fox_tone_hz(rssi_dbm, att, &period);
	next_ms = now_ms + period;
	return hz;
}
#endif
