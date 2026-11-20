/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus clock tools (CONFIG_CLOCK_TOOLS, docs/plus.md "Clock tools"): a stopwatch with lap times, a countdown
 * and an alarm at a time of day. The portable part (no RTOS: every call gets the UI's millisecond clock); installed
 * as core/ui/clockx.{c,h}; the screen, the feed and the banner are ui_clock.c. RAM only: all off at power on.
 *
 * Time of day: the radio has no clock of its own that firmware reads (the board's RTC has no driver), so it is the
 * GPS's UTC time (a fix with the time), carried on by the millisecond clock once the fix is lost, plus the UTC offset
 * set here (+03:00 at first: Turkey). Without a GPS time since power on there is no clock: the alarm can be set but
 * the screen says it waits for one, and it cannot ring. */
#ifndef PLUS_CLOCKX_H
#define PLUS_CLOCKX_H

#include <stdbool.h>
#include <stdint.h>

#define CLK_LAPS	4u		/* lap marks kept (the screen shows the newest three with their own times) */
#define CLK_CD_MAX_S	(99u * 60u)	/* the longest countdown */
#define CLK_CD_DEFAULT_S	(5u * 60u)
#define CLK_DAY_S	86400u
#define CLK_JUMP_MIN	10u		/* the clock moving more than this at once (a new GPS time): the alarm waits for its
					   next minute rather than ringing for one jumped over */

enum { CLK_EV_COUNTDOWN = 1u, CLK_EV_ALARM = 2u };

typedef struct {
	/* stopwatch */
	uint32_t sw_start_ms;	/* running: when it last started */
	uint32_t sw_acc_ms;	/* counted before that */
	uint32_t lap[CLK_LAPS];	/* the stopwatch's time at each lap mark, newest first */
	uint8_t laps;		/* lap marks since the reset (lap[] keeps the newest CLK_LAPS) */
	bool sw_run;
	/* countdown */
	bool cd_run;
	uint16_t cd_set_s;	/* the time it counts down from */
	uint32_t cd_end_ms;	/* running: when it ends */
	/* alarm */
	bool al_on;
	uint16_t al_min;	/* minute of the day, local time */
	uint16_t seen_min;	/* the local minute at the last check, 0xFFFF none */
	/* time of day */
	bool clk_ok;
	int16_t tz_min;		/* local time - UTC, minutes */
	uint32_t clk_ms;	/* the millisecond clock when ... */
	uint32_t clk_sod;	/* ... the UTC time was this second of the day */
} clockx_t;

void clk_init(clockx_t *c);

/* Stopwatch: OK starts / stops; # takes a lap while it runs and resets it while stopped */
void clk_sw_toggle(clockx_t *c, uint32_t now_ms);
void clk_sw_hash(clockx_t *c, uint32_t now_ms);
uint32_t clk_sw_ms(const clockx_t *c, uint32_t now_ms);
/* Lap j (0 = the newest): its own time (since the mark before; the first: since the start) and its number (1 ..);
 * false when there is no such lap kept */
bool clk_sw_lap(const clockx_t *c, uint8_t j, uint32_t *lap_ms, uint8_t *number);

/* Countdown: OK starts / stops (back to the time set); the time set steps by delta seconds (1 s .. CLK_CD_MAX_S),
 * only while stopped */
void clk_cd_toggle(clockx_t *c, uint32_t now_ms);
void clk_cd_adjust(clockx_t *c, int32_t delta_s);
uint32_t clk_cd_left_s(const clockx_t *c, uint32_t now_ms);	/* rounded up; the time set while stopped */

/* Alarm: on / off; the time steps by delta minutes, round the day */
void clk_al_toggle(clockx_t *c);
void clk_al_adjust(clockx_t *c, int32_t delta_min);

/* The GPS time: hhmmss UTC of a fix (only with a fix: the receiver keeps the last fix's time after it is lost) */
void clk_gps(clockx_t *c, bool fix, uint32_t utc_hms, uint32_t now_ms);
/* The UTC offset steps by delta minutes, -12:00 .. +14:00 round */
void clk_tz_adjust(clockx_t *c, int32_t delta_min);
/* Local second of the day; false without a clock */
bool clk_local_s(const clockx_t *c, uint32_t now_ms, uint32_t *sod);

/* Every UI tick: what is due now, each once (CLK_EV_*): the countdown reaching 0 (then stopped, the time set kept),
 * the alarm's minute begun (then off) */
uint8_t clk_tick(clockx_t *c, uint32_t now_ms);

#endif
