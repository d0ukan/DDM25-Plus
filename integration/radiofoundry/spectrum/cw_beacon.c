/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus CW beacon messages (cw_beacon.h). The keying itself is in radio_sm.c (the CW ID keyer). */
#include "config.h"
#if CONFIG_CW_BEACON
#include <stddef.h>
#include "radio/cw_beacon.h"

static void put(char *out, uint8_t cap, uint8_t *n, const char *s)
{
	while (*s != '\0' && *n + 1u < cap)
		out[(*n)++] = *s++;
}

uint8_t cw_beacon_text(uint8_t kind, const char *call, char *out, uint8_t cap)
{
	uint8_t n = 0;
	if (cap == 0)
		return 0;
	out[0] = '\0';
	if (call == NULL || call[0] == '\0' || kind >= CW_BEACON_KINDS)
		return 0;
	switch (kind) {
	case CW_BEACON_CQ:
		put(out, cap, &n, "CQ CQ DE ");
		put(out, cap, &n, call);
		put(out, cap, &n, " ");
		put(out, cap, &n, call);
		put(out, cap, &n, " K");
		break;
	case CW_BEACON_SOS:
		put(out, cap, &n, "SOS SOS SOS DE ");
		put(out, cap, &n, call);
		break;
	default:
		put(out, cap, &n, "DE ");
		put(out, cap, &n, call);
		put(out, cap, &n, " K");
		break;
	}
	out[n] = '\0';
	return n;
}

uint32_t cw_beacon_interval_ms(uint8_t code)
{
	static const uint32_t ms[CW_BEACON_INTERVALS] = { 0u, 30000u, 60000u, 180000u };
	return code < CW_BEACON_INTERVALS ? ms[code] : 0u;
}
#endif
