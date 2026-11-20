/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus Tools > Clock, the UI task's side (ui_clock.c, clockx.h). Installed as core/ui/ui_clock.h. */
#ifndef PLUS_UI_CLOCK_H
#define PLUS_UI_CLOCK_H

#include <stdbool.h>
#include <stdint.h>
#include "ui/clockx.h"

/* Every UI tick (ui_app.c): the GPS time once a second; true: draw the screen again (top: the screen on top) */
bool ui_clock_tick(uint8_t top, uint32_t now_ms);
/* Every UI tick, any screen (ui_app.c, the satellite alarm's banner path): the countdown's end or the alarm due now
 * has sounded its tone and returns its text for the banner ("Alarm 07:30", a static buffer); NULL when none */
const char *ui_clock_alarm(uint32_t now_ms);
clockx_t *ui_clock_state(void);

#endif
