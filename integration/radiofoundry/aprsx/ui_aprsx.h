/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus APRS station page (CONFIG_APRS_STATION, docs/plus.md round 14 item 49): Tools > APRS > Stations heard,
 * OK on a station. Installed as core/ui/ui_aprsx.{c,h}. */
#ifndef PLUS_UI_APRSX_H
#define PLUS_UI_APRSX_H

#include <stdbool.h>
#include <stdint.h>
#include "config.h"

#if CONFIG_APRS_STATION
/* The page of the heard station call ("TA1ABC-9"), on top of the screen in use */
void ui_aprsx_open(const char *call);
/* Every UI tick: true when the page is on top and should be drawn again (once a second: the age moves on) */
bool ui_aprsx_tick(uint8_t scr, uint32_t now_ms);
/* A symbol's name ("Car"), translated; NULL for one without a name here (APRS101 appendix 2: the primary table's
 * common symbols, and the same few on the alternate table and with an overlay) */
const char *ui_aprsx_symbol(char table, char code);
/* ui_map.c: Tools > Map centred on the heard station call, picked, its details under the map */
void ui_map_show_station(const char *call);
#if CONFIG_APRS_MSG
/* ui_aprsmsg.c: the addressee of the message we sent last (NULL: none since power on) and its text */
const char *ui_aprsmsg_sent(const char **text);
#endif
#endif

#endif
