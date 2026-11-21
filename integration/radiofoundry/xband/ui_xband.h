/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus Tools > Cross-band, the UI task's side (ui_xband.c, radio/xband.h). Installed as core/ui/ui_xband.h. */
#ifndef PLUS_UI_XBAND_H
#define PLUS_UI_XBAND_H

#include <stdbool.h>
#include <stdint.h>
#include "hal_input.h"

/* ui_app_key, first: while a session runs the screen is locked (true: the key was taken: Back stops, others refused) */
bool ui_xband_lock(const input_event_t *ev);
/* Home's bottom line while a session runs ("Record A 0:12  Back"), else bottom as it was */
const char *ui_xband_home(const char *bottom);
/* The UI's tick: true when Home should draw again (the relay moved); a session's end is told once */
bool ui_xband_tick(uint8_t top, uint32_t now);

#endif
