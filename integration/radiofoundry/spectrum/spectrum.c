/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus spectrum sweep engine (spectrum.h). Pure logic: no RTOS, no hardware; the radio state machine tunes. */
#include "config.h"
#if CONFIG_SPECTRUM
#include <string.h>
#include "radio/spectrum.h"

static spectrum_view_t sweep;
static uint8_t at;			/* the point being read */
static uint32_t tuned_ms;
static volatile uint32_t seq;		/* odd while the sweep is written (spectrum_snapshot retries) */
static volatile uint32_t reads;		/* spectrum_snapshot calls: the UI still looks */
static uint32_t reads_seen, read_ms;

uint32_t spectrum_point_hz(const spectrum_view_t *v, uint8_t i)
{
	int32_t off = ((int32_t)i - (int32_t)(v->points / 2u)) * (int32_t)v->step_hz;
	return (uint32_t)((int32_t)v->center_hz + off);
}

static void begin_write(void) { seq++; __asm__ volatile("" ::: "memory"); }
static void end_write(void) { __asm__ volatile("" ::: "memory"); seq++; }

uint32_t spectrum_start(uint32_t center_hz, uint32_t span_hz, uint32_t now_ms)
{
	begin_write();
	memset(&sweep, 0, sizeof sweep);
	sweep.points = SPECTRUM_POINTS_MAX / 2u;	/* 32 points: one sweep in about a second */
	if (span_hz >= 1000000u)
		sweep.points = SPECTRUM_POINTS_MAX;	/* a wide span keeps its resolution with more points */
	sweep.step_hz = span_hz / sweep.points;
	if (sweep.step_hz < 2500u)
		sweep.step_hz = 2500u;
	sweep.center_hz = center_hz;
	sweep.hit = SPECTRUM_POINTS_MAX;
	sweep.on = true;
	end_write();
	at = 0;
	tuned_ms = read_ms = now_ms;
	reads_seen = reads;
	return spectrum_point_hz(&sweep, 0);
}

static int16_t median(const int16_t *v, uint8_t n)
{
	int16_t s[SPECTRUM_POINTS_MAX];
	memcpy(s, v, n * sizeof *v);
	for (uint8_t i = 1; i < n; i++) {
		int16_t x = s[i];
		uint8_t j = i;
		while (j > 0 && s[j - 1] > x) {
			s[j] = s[j - 1];
			j--;
		}
		s[j] = x;
	}
	return s[n / 2u];
}

static void close_call(void)
{
	sweep.floor_dbm = median(sweep.dbm, sweep.points);
	uint8_t best = SPECTRUM_POINTS_MAX;
	for (uint8_t i = 0; i < sweep.points; i++) {
		if (sweep.dbm[i] >= SPECTRUM_HIT_MIN_DBM && sweep.dbm[i] - sweep.floor_dbm >= SPECTRUM_HIT_DB &&
		    (best == SPECTRUM_POINTS_MAX || sweep.dbm[i] > sweep.dbm[best]))
			best = i;
	}
	sweep.hit = best;
	sweep.hit_dbm = best < SPECTRUM_POINTS_MAX ? sweep.dbm[best] : 0;
}

uint32_t spectrum_service(int16_t rssi_dbm, uint32_t now_ms)
{
	if (!sweep.on)
		return 0;
	if (reads != reads_seen) {
		reads_seen = reads;
		read_ms = now_ms;
	} else if (now_ms - read_ms >= SPECTRUM_IDLE_MS) {
		spectrum_stop();
		return 0;
	}
	if (now_ms - tuned_ms < SPECTRUM_SETTLE_MS)
		return 0;
	if (rssi_dbm == 0)
		rssi_dbm = -140;	/* "unknown" reads as no signal, never as the strongest */
	begin_write();
	sweep.dbm[at] = rssi_dbm;
	if (sweep.peak_dbm[at] == 0 || rssi_dbm > sweep.peak_dbm[at])
		sweep.peak_dbm[at] = rssi_dbm;
	if (++at >= sweep.points) {
		at = 0;
		sweep.sweeps++;
		close_call();
	}
	end_write();
	tuned_ms = now_ms;
	return spectrum_point_hz(&sweep, at);
}

void spectrum_stop(void)
{
	begin_write();
	sweep.on = false;
	end_write();
}

bool spectrum_on(void)
{
	return sweep.on;
}

uint32_t spectrum_sweeps(void)
{
	return sweep.sweeps;
}

bool spectrum_snapshot(spectrum_view_t *out)
{
	reads++;
	for (unsigned tries = 0; tries < 8u; tries++) {
		uint32_t a = seq;
		__asm__ volatile("" ::: "memory");
		if (a & 1u)
			continue;
		memcpy(out, &sweep, sizeof *out);
		__asm__ volatile("" ::: "memory");
		if (seq == a)
			return out->on && (out->sweeps != 0 || out->dbm[0] != 0);
	}
	return false;
}

#if CONFIG_PC_LIVE
static void put_le(uint8_t *p, uint32_t x, unsigned bytes)
{
	for (unsigned i = 0; i < bytes; i++)
		p[i] = (uint8_t)(x >> (8u * i));
}

/* DM32 Plus PC live panel (debug link 0x21 [13], docs/plus.md): the sweep as bytes, straight from the engine's state
 * under its sequence word, without counting as a read (reads is the screen's: a PC watching must not keep a sweep
 * alive that nobody looks at on the radio) and without a copy of the view on the host task's stack */
size_t spectrum_export(uint8_t *out, size_t cap)
{
	if (cap < SPECTRUM_EXPORT_HEAD + 2u * SPECTRUM_POINTS_MAX)
		return 0;
	uint32_t a = seq;
	__asm__ volatile("" ::: "memory");
	if ((a & 1u) || !sweep.on || (sweep.sweeps == 0 && sweep.dbm[0] == 0))
		return 0;
	uint8_t n = sweep.points <= SPECTRUM_POINTS_MAX ? sweep.points : SPECTRUM_POINTS_MAX;
	out[0] = SPECTRUM_EXPORT_VERSION;
	out[1] = sweep.on;
	out[2] = n;
	out[3] = sweep.hit < n ? sweep.hit : 0xFFu;
	put_le(out + 4, sweep.center_hz, 4);
	put_le(out + 8, sweep.step_hz, 4);
	put_le(out + 12, sweep.sweeps, 4);
	put_le(out + 16, (uint16_t)sweep.floor_dbm, 2);
	put_le(out + 18, (uint16_t)sweep.hit_dbm, 2);
	for (uint8_t i = 0; i < n; i++)
		put_le(out + SPECTRUM_EXPORT_HEAD + 2u * i, (uint16_t)sweep.dbm[i], 2);
	__asm__ volatile("" ::: "memory");
	return seq == a ? SPECTRUM_EXPORT_HEAD + 2u * n : 0;
}
#endif
#endif
