/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus Tools > DOTA Transfer (ui_dota.c, docs/dota.md 12). Installed as core/ui/ui_dota.h. */
#ifndef UI_DOTA_H
#define UI_DOTA_H

#include <stdbool.h>
#include <stdint.h>

/* Every UI tick (ui_app.c): the receiver's storage jobs posted; true when the transfer screen should draw again */
bool ui_dota_tick(uint8_t top, uint32_t now);

#endif
