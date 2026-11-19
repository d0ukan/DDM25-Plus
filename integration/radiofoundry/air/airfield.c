/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus airfield list math (airfield.h). Pure: reads the const table, writes only the caller's results. */
#include "config.h"
#if CONFIG_AIRFIELDS
#include "ui/airfield.h"
#include "aprs/aprs_fmt.h"

void air_here_set(air_here_t *h, bool ok, int32_t lat_e7, int32_t lon_e7)
{
	h->ok = ok;
	h->lat_e5 = ok ? lat_e7 / 100 : 0;
	h->lon_e5 = ok ? lon_e7 / 100 : 0;
}

/* cos(degrees) in Q15 for 0..90 (table) with linear interpolation: lat_e5 in 1e-5 degree */
static int32_t cos_q15(int32_t lat_e5)
{
	static const uint16_t c[19] = { 32767, 32643, 32270, 31650, 30792, 29697, 28378, 26842, 25102, 23170, 21063,
					18795, 16384, 13848, 11207, 8481, 5690, 2856, 0 };	/* every 5 degrees */
	if (lat_e5 < 0)
		lat_e5 = -lat_e5;
	if (lat_e5 >= 9000000)
		return 0;
	int32_t k = lat_e5 / 500000, f = lat_e5 % 500000;
	return c[k] + (int32_t)(((int64_t)c[k + 1] - c[k]) * f / 500000);
}

/* The order's key of airfield i: the squared equirectangular distance at the pair's middle latitude (1e-5 degree
 * units; ordering within 0.3 % of the great circle up to a few hundred km, 1 % across the country); 0 for every
 * airfield without our position (the table's order) */
static uint64_t key(const air_here_t *h, uint16_t i)
{
	const air_field_t *a = &air_fields[i];
	if (!h->ok)
		return 0;
	int64_t dlat = (int64_t)a->lat_e5 - h->lat_e5;
	int64_t dlon = ((int64_t)a->lon_e5 - h->lon_e5) * cos_q15((a->lat_e5 + h->lat_e5) / 2) / 32768;
	return (uint64_t)(dlat * dlat) + (uint64_t)(dlon * dlon);
}

/* (ka, a) before (kb, b) */
static bool before(uint64_t ka, uint16_t a, uint64_t kb, uint16_t b)
{
	return ka < kb || (ka == kb && a < b);
}

uint16_t air_next(const air_here_t *h, uint16_t cur, int dir)
{
	uint16_t n = air_field_count, best = AIR_NONE;
	if (cur != AIR_NONE && cur >= n)
		return AIR_NONE;
	uint64_t kc = cur < n ? key(h, cur) : 0, kb = 0;
	for (uint16_t i = 0; i < n; i++) {
		uint64_t k = key(h, i);
		if (cur != AIR_NONE && (dir > 0 ? !before(kc, cur, k, i) : !before(k, i, kc, cur)))
			continue;
		if (best == AIR_NONE || (dir > 0 ? before(k, i, kb, best) : before(kb, best, k, i))) {
			best = i;
			kb = k;
		}
	}
	return best;
}

uint16_t air_rank(const air_here_t *h, uint16_t i)
{
	uint16_t n = air_field_count, r = 0;
	if (i >= n)
		return 0;
	uint64_t ki = key(h, i);
	for (uint16_t j = 0; j < n; j++)
		if (before(key(h, j), j, ki, i))
			r++;
	return r;
}

void air_way(uint16_t i, int32_t lat_e7, int32_t lon_e7, uint32_t *meters, uint16_t *bearing)
{
	const air_field_t *a = &air_fields[i];
	*meters = aprs_distance_m(lat_e7, lon_e7, a->lat_e5 * 100, a->lon_e5 * 100);
	*bearing = aprs_bearing_deg(lat_e7, lon_e7, a->lat_e5 * 100, a->lon_e5 * 100);
}

uint32_t air_freq_khz(const air_freq_t *f)
{
	return AIR_CH_BASE_KHZ + 5u * f->ch;
}

bool air_freq_833(const air_freq_t *f)
{
	return air_freq_khz(f) % 25u != 0;
}

uint32_t air_freq_hz(const air_freq_t *f)
{
	uint32_t khz = air_freq_khz(f), r = khz % 25u;
	if (r == 0)
		return khz * 1000u;
	/* Grid point n is n x 25000 / 3 Hz (radio_am_step): x.005, x.010, x.015 are the block's points 0, 1, 2 */
	uint64_t n = (uint64_t)(khz - r) * 3u / 25u + (r / 5u - 1u);
	return (uint32_t)((n * 25000u + 1u) / 3u);
}

const char *air_type_name(uint8_t type)
{
	static const char *const names[AIR_TYPES] = { "ATIS", "DEL", "GND", "RMP", "TWR", "APP", "DEP", "AFIS", "INFO",
						      "CTAF", "PAR", "SAR", "EMR", "OPS", "OTH" };
	return type < AIR_TYPES ? names[type] : "?";
}
#endif
