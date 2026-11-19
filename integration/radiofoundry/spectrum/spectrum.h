/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus spectrum sweep (docs/plus.md): the channel receiver steps across a span, reads the signal strength at
 * each point and the radio task publishes the sweep for the UI. While it runs the receiver belongs to the sweep
 * (speaker muted, no squelch, scan or call handling); stopping it tunes the channel again. Close call: the strongest
 * point well above the floor is the "hit" the UI offers to tune to. */
#ifndef PLUS_SPECTRUM_H
#define PLUS_SPECTRUM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SPECTRUM_POINTS_MAX	64u
#define SPECTRUM_SETTLE_MS	30u	/* a reading this long after a retune is the new point's */
#define SPECTRUM_HIT_DB		12	/* close call: this far above the sweep's floor ... */
#define SPECTRUM_HIT_MIN_DBM	(-105)	/* ... and at least this strong */
#define SPECTRUM_IDLE_MS	3000u	/* nobody looked at the sweep this long (the UI left its screen): it stops */

typedef struct {
	bool on;
	uint8_t points;			/* readings across the span */
	uint32_t center_hz, step_hz;	/* point i is at center + (i - points / 2) * step */
	uint32_t sweeps;		/* full sweeps since the start */
	int16_t dbm[SPECTRUM_POINTS_MAX];	/* the last reading of each point, 0 = not read yet */
	int16_t peak_dbm[SPECTRUM_POINTS_MAX];	/* the strongest reading of each point since the start */
	int16_t floor_dbm;		/* the median of the last full sweep */
	uint8_t hit;			/* close call: the strongest point above the floor, SPECTRUM_POINTS_MAX = none */
	int16_t hit_dbm;
} spectrum_view_t;

/* The sweep engine (radio task). spectrum_start returns the first frequency to tune; spectrum_service is called on
 * every poll with the reading and returns the next frequency to tune, 0 = stay */
uint32_t spectrum_start(uint32_t center_hz, uint32_t span_hz, uint32_t now_ms);
uint32_t spectrum_service(int16_t rssi_dbm, uint32_t now_ms);
void spectrum_stop(void);
bool spectrum_on(void);
uint32_t spectrum_sweeps(void);
uint32_t spectrum_point_hz(const spectrum_view_t *v, uint8_t i);

/* A consistent copy for another task (the UI): false while nothing has been swept */
bool spectrum_snapshot(spectrum_view_t *out);

/* DM32 Plus PC live panel (CONFIG_PC_LIVE, debug link 0x21 [13]): the sweep as the bytes docs/plus.md gives (version,
 * on, points, hit, center, step, sweeps, floor, hit dBm, dBm of each point; little endian). Never counts as a read
 * (spectrum_snapshot's reads keep the sweep alive), never starts or retunes anything. 0: no sweep running or nothing
 * swept yet, or the radio task was writing (try again), or cap too small */
#define SPECTRUM_EXPORT_VERSION	1u
#define SPECTRUM_EXPORT_HEAD	20u
size_t spectrum_export(uint8_t *out, size_t cap);

/* Requests (radio task queue): center 0 = stop */
void radio_req_spectrum(uint32_t center_hz, uint32_t span_hz);

#endif
