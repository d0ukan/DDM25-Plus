/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus APRS messages and SOS (aprs_plus.h): the Mic-E encoder, the canned texts, the composer's check. */
#include "config.h"
#if CONFIG_APRS_MSG
#include <string.h>
#include "aprs/aprs_plus.h"
#include "aprs/ax25.h"

const char *const aprs_canned[APRS_CANNED_N] = { "QSL", "73", "OK", "QRV?", "On my way", "Call me", "Wait",
						 "Thanks" };

/* Hundredths of a minute of arc in |v| (1e-7 degree), rounded */
static uint32_t hmin(int32_t v)
{
	uint64_t m = (uint64_t)(v < 0 ? -(int64_t)v : (int64_t)v);
	return (uint32_t)((m * 6000u + (uint64_t)APRS_DEG / 2u) / (uint64_t)APRS_DEG);
}

/* A destination character: the digit, '0'-'9' for a 0 bit / south / no offset / east, 'P'-'Y' for a 1 */
static char mice_dst(uint32_t digit, bool bit)
{
	return (char)((bit ? 'P' : '0') + (char)digit);
}

size_t aprs_mice_build(const aprs_pos_t *pos, uint8_t msg, const char *comment, char *dst, char *out, size_t max)
{
	/* Latitude DDMM.HH as the six destination digits */
	uint32_t h = hmin(pos->lat);
	if (h > 90u * 6000u)
		h = 90u * 6000u;
	uint32_t deg = h / 6000u, min = h % 6000u / 100u, hun = h % 100u;
	uint8_t d[6] = { (uint8_t)(deg / 10u), (uint8_t)(deg % 10u), (uint8_t)(min / 10u), (uint8_t)(min % 10u),
			 (uint8_t)(hun / 10u), (uint8_t)(hun % 10u) };
	/* Message bits A B C: M0 (Off duty) = 111 .. M6 (Priority) = 001, Emergency = 000 */
	uint8_t bits = msg >= APRS_MICE_EMERGENCY ? 0u : (uint8_t)(7u - msg);
	/* Longitude */
	h = hmin(pos->lon);
	if (h >= 180u * 6000u)
		h = 180u * 6000u - 1u;	/* 180 itself has no Mic-E code: 179 59.99 */
	uint32_t ldeg = h / 6000u, lmin = h % 6000u / 100u, lhun = h % 100u;
	bool offset = ldeg < 10u || ldeg >= 100u;
	dst[0] = mice_dst(d[0], (bits & 4u) != 0);
	dst[1] = mice_dst(d[1], (bits & 2u) != 0);
	dst[2] = mice_dst(d[2], (bits & 1u) != 0);
	dst[3] = mice_dst(d[3], pos->lat >= 0);		/* north */
	dst[4] = mice_dst(d[4], offset);		/* longitude offset +100 */
	dst[5] = mice_dst(d[5], pos->lon < 0);		/* west */
	dst[APRS_MICE_DST_LEN] = '\0';

	size_t clen = comment != NULL ? strlen(comment) : 0;
	bool alt = pos->alt_m != APRS_ALT_NONE;
	size_t n = 9u + (alt ? 4u : 0u) + clen;
	if (n > max)
		return 0;
	out[0] = '`';	/* current GPS data */
	/* Degrees: 0-9 -> 118-127 (offset), 10-99 -> 38-127, 100-109 -> 108-117 (offset), 110-179 -> 38-107 (offset) */
	out[1] = (char)(ldeg < 10u ? ldeg + 118u : ldeg < 100u ? ldeg + 28u : ldeg < 110u ? ldeg + 8u : ldeg - 72u);
	out[2] = (char)(lmin < 10u ? lmin + 88u : lmin + 28u);	/* minutes 0-9 as 60-69 */
	out[3] = (char)(lhun + 28u);
	/* Speed (knots) and course: SP = speed / 10, DC = speed % 10 x 10 + course / 100, SE = course % 100, each + 28.
	 * SP and DC below 4 would be control characters: SP + 80 (speed + 800) and DC + 4 (course + 400), which the
	 * receiver takes back off (APRS101's own example does this) */
	uint32_t kn = pos->speed_kn == APRS_SPEED_NONE ? 0u : pos->speed_kn > 799u ? 799u : pos->speed_kn;
	uint32_t crs = pos->course == APRS_COURSE_NONE || pos->course > 360u ? 0u : pos->course;
	uint32_t sp = kn / 10u, dc = kn % 10u * 10u + crs / 100u, se = crs % 100u;
	out[4] = (char)(sp < 4u ? sp + 108u : sp + 28u);
	out[5] = (char)(dc < 4u ? dc + 32u : dc + 28u);
	out[6] = (char)(se + 28u);
	out[7] = pos->sym_code;
	out[8] = pos->sym_table;
	size_t k = 9;
	if (alt) {
		int32_t a = pos->alt_m + 10000;	/* meters above -10 km, base 91 */
		aprs_base91((uint32_t)(a < 0 ? 0 : a > 91 * 91 * 91 - 1 ? 91 * 91 * 91 - 1 : a), 3, out + k);
		out[k + 3u] = '}';
		k += 4u;
	}
	if (clen != 0)
		memcpy(out + k, comment, clen);
	return n;
}

bool aprs_addressee_fix(char *to)
{
	size_t k = 0;
	for (size_t i = 0; to[i] != '\0'; i++) {
		char c = to[i];
		if (c == ' ')
			continue;
		to[k++] = c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c;
	}
	to[k] = '\0';
	ax25_addr_t x;
	return k != 0 && k <= APRS_ADDRESSEE_LEN && ax25_addr_parse(to, k, &x);
}

uint8_t aprs_compose_send(char *to, char *text)
{
	if (!aprs_addressee_fix(to))
		return APRS_SEND_BAD_TO;
	size_t n = strlen(text);
	while (n > 0 && text[n - 1u] == ' ')
		text[--n] = '\0';
	if (n == 0)
		return APRS_SEND_NO_TEXT;
	for (size_t i = 0; i < n; i++) {
		if (text[i] == '|' || text[i] == '~' || text[i] == '{')
			text[i] = '-';	/* APRS101 ch. 14: not in a message's text */
	}
	return aprs_send_message(to, text) ? APRS_SEND_OK : APRS_SEND_BUSY;
}
#endif
