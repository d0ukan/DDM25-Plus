/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus spectrum waterfall (CONFIG_SPECTRUM_WF, docs/plus.md "Spectrum waterfall"): the last WF_ROWS sweeps of
 * Tools > Spectrum, a row each, 4 bits a point (a level above the sweep's floor). The portable part: the ring, the
 * levels and their colour slots; installed as core/ui/wfall.{c,h}. The buffer (WF_BYTES) is the screen's: taken from
 * the RTOS heap when the screen opens with the waterfall on and given back when it closes (ui_spectrum.c); without it
 * the screen draws the bars alone. The frame buffer is no place for the history: the screen is cleared at every
 * redraw and toasts and banners are drawn over it. */
#ifndef PLUS_WFALL_H
#define PLUS_WFALL_H

#include <stdbool.h>
#include <stdint.h>
#include "ui/gfx.h"

#define WF_POINTS	64u			/* SPECTRUM_POINTS_MAX */
#define WF_ROWS		40u
#define WF_ROW_BYTES	(WF_POINTS / 2u)
#define WF_BYTES	(WF_ROWS * WF_ROW_BYTES)	/* 1,280 */
#define WF_DB_LEVEL	3			/* dB a level: 15 levels span 45 dB above the floor */
#define WF_SLOTS	8u			/* colour slots, darkest first (wf_slot) */

typedef struct {
	uint8_t *buf;		/* WF_BYTES, NULL: no waterfall */
	uint8_t head;		/* the row the next sweep goes into */
	uint8_t n;		/* rows kept */
	uint8_t points;		/* points a row (the sweep's) */
	uint32_t sweeps;	/* the sweep count of the newest row */
} wf_t;

/* No rows (the buffer stays) */
void wf_reset(wf_t *w);
#if CONFIG_SPECTRUM_COLOR
/* DDM32 Plus round 16 (CONFIG_SPECTRUM_COLOR): the slots' colours on a colour panel, a signal strength ramp: slot 0
 * the background, then dark blue, blue, cyan, green, yellow, orange, red. On a light background (the light theme) the
 * weakest is a pale blue and the others are darker shades that read on white. Green and red are the theme's RX and TX
 * colours (palette entries the screen has anyway), so the ramp adds five entries to the 16 colour palette of a bpp 4
 * panel. The waterfall and the 3D view draw with it; a mono panel keeps the theme's slots (ui_spectrum.c) */
void wf_ramp(color_t slot[WF_SLOTS], color_t bg, color_t rx, color_t tx);
#endif
/* A reading's level 0..15: 0 not read or within WF_DB_LEVEL of the floor, then a level every WF_DB_LEVEL dB */
uint8_t wf_level(int16_t dbm, int16_t floor_dbm);
/* A level's colour slot 0..WF_SLOTS - 1 (the screen's colours: background, line, idle, dim, RX, warning, TX) */
uint8_t wf_slot(uint8_t level);
/* A finished sweep as the newest row (the oldest goes when full); a sweep with another point count starts over.
 * Nothing without a buffer */
void wf_push(wf_t *w, const int16_t *dbm, uint8_t points, int16_t floor_dbm, uint32_t sweeps);
/* The level of point i in row r (0 = the newest) */
uint8_t wf_get(const wf_t *w, uint8_t r, uint8_t i);

/* Tools > Spectrum's side (ui_spectrum.c). Every UI tick (ui_app.c), top the screen on top: the buffer goes back to
 * the heap once another screen is on top. held: the screen has its buffer now */
void ui_spectrum_wf_tick(uint8_t top);
bool ui_spectrum_wf_held(void);

#endif
