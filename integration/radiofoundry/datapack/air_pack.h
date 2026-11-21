/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus air band data pack over the built-in airfield table (CONFIG_DATAPACK, docs/r14.md 7.3): the airfield
 * screens ask here for an airfield's frequencies. When the user's air band pack (type DPK_AIR) lists the airfield's
 * code, its list replaces the built-in one (an empty list too); otherwise the built-in table answers. Airfields the
 * pack names that the table does not hold are not shown (format 1: frequencies only). Installed as
 * core/ui/air_pack.{c,h}.
 *
 * Air band pack payload, format 1: a 4-byte header (airfield count 2 bytes LE, 2 reserved), then per airfield its
 * code (8 bytes, NUL padded), its frequency count (1 byte, at most AIR_PACK_FREQS) and each frequency as the built-in
 * table's air_freq_t (channel 2 bytes LE: (kHz - 108000) / 5, type 1 byte: air_type_t). */
#ifndef PLUS_AIR_PACK_H
#define PLUS_AIR_PACK_H

#include <stdint.h>
#include "ui/airfield.h"

#define AIR_PACK_HDR	4u
#define AIR_PACK_FREQS	64u

/* Airfield i's frequency count and its frequency k (NULL past the end), from the pack when it names the airfield */
uint8_t air_count(uint16_t i);
const air_freq_t *air_freq(uint16_t i, uint8_t k);

#endif
