/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM25 Plus "find my friend" (follow.h): pure. */
#include "config.h"
#if CONFIG_NAV_FOLLOW
#include <string.h>
#include "ui/follow.h"

void follow_set(follow_t *f, const char *call)
{
	memset(f, 0, sizeof *f);
	size_t n = strnlen(call, FOLLOW_CALL_LEN - 1u);
	memcpy(f->call, call, n);
}

void follow_clear(follow_t *f)
{
	memset(f, 0, sizeof *f);
}

bool follow_on(const follow_t *f)
{
	return f->call[0] != '\0';
}

bool follow_update(follow_t *f, const char *call, bool has_pos, int32_t lat, int32_t lon, uint32_t heard_ms)
{
	if (!follow_on(f) || strncmp(f->call, call, FOLLOW_CALL_LEN) != 0)
		return false;
	bool changed = false;
	if (heard_ms != f->heard_ms) {
		f->heard_ms = heard_ms;
		changed = true;
	}
	if (has_pos && (!f->has_pos || lat != f->lat || lon != f->lon)) {
		f->has_pos = true;
		f->lat = lat;
		f->lon = lon;
		changed = true;
	}
	return changed;
}

uint32_t follow_age_ms(const follow_t *f, uint32_t now_ms)
{
	if (!follow_on(f) || f->heard_ms == 0u)
		return 0;
	return now_ms - f->heard_ms;
}

bool follow_stale(const follow_t *f, uint32_t now_ms)
{
	return follow_on(f) && f->heard_ms != 0u && follow_age_ms(f, now_ms) >= FOLLOW_STALE_MS;
}
#endif
