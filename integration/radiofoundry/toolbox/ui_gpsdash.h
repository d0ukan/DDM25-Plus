/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus Tools > GPS, the UI task's side (ui_gpsdash.c, tripm.h). Installed as core/ui/ui_gpsdash.h. */
#ifndef PLUS_UI_GPSDASH_H
#define PLUS_UI_GPSDASH_H

#include <stdbool.h>
#include <stdint.h>
#include "ui/tripm.h"

/* Every UI tick (ui_app.c): the GPS into the trip meter once a second; true: draw again (top: the screen on top) */
bool ui_gpsdash_tick(uint8_t top, uint32_t now_ms);
const trip_t *ui_gpsdash_trip(void);

#endif
