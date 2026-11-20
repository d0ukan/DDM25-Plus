/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus locator calculator (CONFIG_LOCATOR, docs/plus.md): Maidenhead locators, 4 characters (a square, 2 x 1
 * degree) or 6 (a subsquare, 5 x 2.5 minutes), to the centre's latitude / longitude and back. Integer only (1e-7
 * degree), the same grid as Tools > Repeaters' locator (rpt_locator) and Tools > Position's. Installed as
 * core/ui/locator.{c,h}; pure (the host tests run it as is). */
#ifndef PLUS_LOCATOR_H
#define PLUS_LOCATOR_H

#include <stdbool.h>
#include <stdint.h>

#define LOC_LEN_MAX	6u

/* text: 4 or 6 characters, any case (spaces around are not allowed): field A-R, square 0-9, subsquare A-X. True with
 * the centre in *lat_e7 / *lon_e7 and the locator written the usual way in norm (at least 7 bytes, "KM69kw"; may be
 * NULL) */
bool loc_parse(const char *text, int32_t *lat_e7, int32_t *lon_e7, char *norm);
/* The 6 character locator of a position (out at least 7 bytes); latitude -90..90, longitude -180..180 (clamped) */
void loc_encode(int32_t lat_e7, int32_t lon_e7, char *out);

#endif
