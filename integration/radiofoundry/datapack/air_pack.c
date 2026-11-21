/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus air band data pack over the built-in airfield table (air_pack.h). Installed as core/ui/air_pack.c. */
#include "config.h"
#if CONFIG_AIRFIELDS
#include <string.h>
#include "ui/air_pack.h"
#if CONFIG_DATAPACK
#include "storage/datapack.h"

/* The last airfield looked up: where its list starts in the pack (no RAM copy of the list) */
static struct {
	uint32_t seq;		/* the pack it was found in (0: none looked up) */
	uint16_t i;
	uint32_t off;		/* its first frequency in the payload, 0: not in the pack */
	uint8_t n;
} last;

/* Airfield i's list in the pack: true and its offset / count when the pack names it */
static bool find(uint16_t i, uint32_t *off, uint8_t *n)
{
	const dpk_info_t *p = dpk_active(DPK_AIR);
	if (p == NULL)
		return false;
	if (last.seq != p->seq || last.i != i) {
		uint8_t h[AIR_PACK_HDR], r[9];
		last.seq = p->seq;
		last.i = i;
		last.off = 0;
		if (!dpk_read(DPK_AIR, 0, h, sizeof h))
			return false;
		uint32_t at = AIR_PACK_HDR;
		for (uint16_t k = 0, count = (uint16_t)(h[0] | h[1] << 8); k < count; k++) {
			if (!dpk_read(DPK_AIR, at, r, sizeof r) || r[8] > AIR_PACK_FREQS || 3u * r[8] > p->len - at - 9u)
				break;	/* a list past the payload's end: the pack is not used from here on */
			if (strncmp((const char *)r, air_fields[i].code, 8) == 0 && air_fields[i].code[0] != '\0') {
				last.off = at + 9u;
				last.n = r[8];
				break;
			}
			at += 9u + 3u * r[8];
		}
	}
	*off = last.off;
	*n = last.n;
	return last.off != 0u;
}
#endif

uint8_t air_count(uint16_t i)
{
	if (i >= air_field_count)
		return 0;
#if CONFIG_DATAPACK
	uint32_t off;
	uint8_t n;
	if (find(i, &off, &n))
		return n;
#endif
	return air_fields[i].count;
}

const air_freq_t *air_freq(uint16_t i, uint8_t k)
{
	if (k >= air_count(i))
		return NULL;
#if CONFIG_DATAPACK
	static air_freq_t f;
	uint32_t off;
	uint8_t n, r[3];
	if (find(i, &off, &n)) {
		if (!dpk_read(DPK_AIR, off + 3u * k, r, sizeof r))
			return NULL;
		f.ch = (uint16_t)(r[0] | r[1] << 8);
		f.type = r[2] < AIR_TYPES ? r[2] : AIR_OTH;
		return &f;
	}
#endif
	return &air_freqs[air_fields[i].first + k];
}
#endif
