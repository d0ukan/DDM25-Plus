// SPDX-License-Identifier: GPL-3.0-or-later
// DM32 Plus priority channel (CONFIG_SCAN_PRIO, docs/plus.md "Priority channel and temporary skip"): the setting.
//
// Upstream's own priority (radio_settings_t.scan_priority, the debug link's only) revisits the scan's start channel
// after every 2 others. Plus names the channel: a codeplug channel number, and the zone it was set from as a hint
// for finding it when the scanned zone doesn't hold it. Kept in settings_t.unused_dmr_id (an old own-ID override that
// nothing reads any more, "kept for the layout"): the record's layout stays upstream's. An old ID there is at most 24
// bits, so the tag in bits 24-31 tells a priority channel from it; anything else reads as Off.
//
//   bits 0-10   the channel number, 1..CODEPLUG_CHANNELS_MAX (1024)
//   bits 11-13  not ours: VFO A's DMR Any flags (vfox/vfox.h, the same tag; kept on every write)
//   bits 16-22  the zone it was set from, 0..127
//   bits 24-31  SCANPRIO_TAG
#ifndef RADIO_SCANPRIO_H
#define RADIO_SCANPRIO_H

#include <stdint.h>
#include <stdbool.h>

#define SCANPRIO_TAG		0x50u	// 'P'
#define SCANPRIO_VFOX_BITS	0x3800u	// bits 11-13: VFO A's DMR Any flags (vfox), kept when the channel changes
#define SCANPRIO_EVERY_MS	2000u	// a scan or a watch looks at the priority channel this often (one dwell)

static inline uint32_t scanprio_pack(uint16_t number, uint16_t zone)
{
	if (number == 0 || number > 2047u || zone > 127u) {
		return 0;
	}
	return ((uint32_t)SCANPRIO_TAG << 24) | ((uint32_t)zone << 16) | number;
}

// False (Off) for 0, a word without the tag, or one with stray bits
static inline bool scanprio_unpack(uint32_t word, uint16_t *number, uint16_t *zone)
{
	if ((word >> 24) != SCANPRIO_TAG || (word & 0x0080C000u) != 0 || (word & 0x7FFu) == 0) {
		return false;
	}
	*number = (uint16_t)(word & 0x7FFu);
	*zone = (uint16_t)((word >> 16) & 0x7Fu);
	return true;
}

static inline uint16_t scanprio_number(uint32_t word)
{
	uint16_t n, z;
	return scanprio_unpack(word, &n, &z) ? n : 0;
}

// The radio core's copy of the setting (radio_scanprio.inc): the UI gives it every change (ui_svc_apply), the radio
// core reads it from the settings at start-up. Any task
void radio_scan_prio_set(uint32_t word);

#endif
