/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus clock tools (clockx.h): the portable part. Installed as core/ui/clockx.c. */
#include "config.h"
#if CONFIG_CLOCK_TOOLS
#include <string.h>
#include "ui/clockx.h"

void clk_init(clockx_t *c)
{
	memset(c, 0, sizeof *c);
	c->cd_set_s = CLK_CD_DEFAULT_S;
	c->al_min = 7u * 60u;
	c->seen_min = 0xFFFFu;
	c->tz_min = 180;
}

/* --- Stopwatch --- */

uint32_t clk_sw_ms(const clockx_t *c, uint32_t now_ms)
{
	return c->sw_acc_ms + (c->sw_run ? now_ms - c->sw_start_ms : 0u);
}

void clk_sw_toggle(clockx_t *c, uint32_t now_ms)
{
	if (c->sw_run)
		c->sw_acc_ms += now_ms - c->sw_start_ms;
	else
		c->sw_start_ms = now_ms;
	c->sw_run = !c->sw_run;
}

void clk_sw_hash(clockx_t *c, uint32_t now_ms)
{
	if (!c->sw_run) {
		c->sw_acc_ms = 0;	/* stopped: reset */
		c->laps = 0;
		memset(c->lap, 0, sizeof c->lap);
		return;
	}
	memmove(&c->lap[1], &c->lap[0], (CLK_LAPS - 1u) * sizeof c->lap[0]);
	c->lap[0] = clk_sw_ms(c, now_ms);
	if (c->laps < 255u)
		c->laps++;
}

bool clk_sw_lap(const clockx_t *c, uint8_t j, uint32_t *lap_ms, uint8_t *number)
{
	/* lap j's own time needs the mark before it: kept for all but the oldest of CLK_LAPS (unless that is lap 1) */
	if (j >= c->laps || j >= CLK_LAPS || (j == CLK_LAPS - 1u && c->laps > CLK_LAPS))
		return false;
	uint32_t before = j + 1u < c->laps && j + 1u < CLK_LAPS ? c->lap[j + 1u] : 0u;
	*lap_ms = c->lap[j] - before;
	*number = (uint8_t)(c->laps - j);
	return true;
}

/* --- Countdown --- */

void clk_cd_toggle(clockx_t *c, uint32_t now_ms)
{
	c->cd_run = !c->cd_run;
	if (c->cd_run)
		c->cd_end_ms = now_ms + (uint32_t)c->cd_set_s * 1000u;
}

void clk_cd_adjust(clockx_t *c, int32_t delta_s)
{
	if (c->cd_run)
		return;
	int32_t s = (int32_t)c->cd_set_s + delta_s;
	c->cd_set_s = (uint16_t)(s < 1 ? 1 : s > (int32_t)CLK_CD_MAX_S ? (int32_t)CLK_CD_MAX_S : s);
}

uint32_t clk_cd_left_s(const clockx_t *c, uint32_t now_ms)
{
	if (!c->cd_run)
		return c->cd_set_s;
	int32_t left = (int32_t)(c->cd_end_ms - now_ms);
	return left <= 0 ? 0u : ((uint32_t)left + 999u) / 1000u;
}

/* --- Alarm --- */

void clk_al_toggle(clockx_t *c)
{
	c->al_on = !c->al_on;
	c->seen_min = 0xFFFFu;	/* the minute it is now does not ring: only one begun from here on */
}

static uint16_t wrap_min(int32_t m)
{
	m %= 1440;
	return (uint16_t)(m < 0 ? m + 1440 : m);
}

void clk_al_adjust(clockx_t *c, int32_t delta_min)
{
	c->al_min = wrap_min((int32_t)c->al_min + delta_min);
	c->seen_min = 0xFFFFu;
}

/* --- Time of day --- */

void clk_gps(clockx_t *c, bool fix, uint32_t utc_hms, uint32_t now_ms)
{
	uint32_t h = utc_hms / 10000u, m = utc_hms / 100u % 100u, s = utc_hms % 100u;
	if (!fix || utc_hms == 0xFFFFFFFFu || h > 23u || m > 59u || s > 59u)
		return;
	c->clk_ok = true;
	c->clk_ms = now_ms;
	c->clk_sod = h * 3600u + m * 60u + s;
}

void clk_tz_adjust(clockx_t *c, int32_t delta_min)
{
	int32_t z = c->tz_min + delta_min;
	if (z > 14 * 60)
		z = -12 * 60;
	if (z < -12 * 60)
		z = 14 * 60;
	c->tz_min = (int16_t)z;
	c->seen_min = 0xFFFFu;
}

bool clk_local_s(const clockx_t *c, uint32_t now_ms, uint32_t *sod)
{
	if (!c->clk_ok)
		return false;
	/* 32 bits (no 64-bit division every tick): the millisecond clock wraps after 49 days, 4.3 M seconds */
	int32_t s = (int32_t)c->clk_sod + (int32_t)((now_ms - c->clk_ms) / 1000u) + (int32_t)c->tz_min * 60;
	s %= (int32_t)CLK_DAY_S;
	*sod = (uint32_t)(s < 0 ? s + (int32_t)CLK_DAY_S : s);
	return true;
}

uint8_t clk_tick(clockx_t *c, uint32_t now_ms)
{
	uint8_t ev = 0;
	if (c->cd_run && (int32_t)(now_ms - c->cd_end_ms) >= 0) {
		c->cd_run = false;
		ev |= CLK_EV_COUNTDOWN;
	}
	uint32_t sod;
	if (!clk_local_s(c, now_ms, &sod)) {
		c->seen_min = 0xFFFFu;
		return ev;
	}
	uint16_t now_min = (uint16_t)(sod / 60u);
	if (c->al_on && c->seen_min != 0xFFFFu && now_min != c->seen_min) {
		/* the alarm's minute begun since the last check (a few minutes jumped over by a new GPS time count; a big
		 * jump does not: rather late than for a time it never was) */
		uint16_t step = wrap_min((int32_t)now_min - c->seen_min);
		uint16_t into = wrap_min((int32_t)c->al_min - c->seen_min);
		if (step <= CLK_JUMP_MIN && into != 0 && into <= step) {
			c->al_on = false;
			ev |= CLK_EV_ALARM;
		}
	}
	c->seen_min = now_min;
	return ev;
}
#endif
