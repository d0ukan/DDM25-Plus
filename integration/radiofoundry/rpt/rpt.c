/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus repeater list math (rpt.h). Pure: reads the const table, writes only the caller's results. */
#include "config.h"
#if CONFIG_REPEATERS
#include <math.h>
#include "ui/rpt.h"
#include "aprs/aprs_fmt.h"

void rpt_here_set(rpt_here_t *h, bool ok, int32_t lat_e7, int32_t lon_e7)
{
	h->ok = ok;
	h->lat_e5 = ok ? lat_e7 / 100 : 0;
	h->lon_e5 = ok ? lon_e7 / 100 : 0;
	float c = cosf((float)lat_e7 * (3.14159265f / 180.0f / 1e7f));
	h->cos_q15 = ok && c > 0.0f ? (uint16_t)(c * 32767.0f + 0.5f) : 0u;
}

/* The order's key of entry i: the squared equirectangular distance (1e-5 degree units); without our position 0 for
 * every entry (the table's order), else the largest for an entry without its own */
static uint64_t key(const rpt_here_t *h, uint16_t i)
{
	const rpt_entry_t *e = &rpt_table[i];
	if (!h->ok)
		return 0;
	if (e->lat_e5 == RPT_NOPOS || e->lon_e5 == RPT_NOPOS)
		return UINT64_MAX;
	int64_t dlat = (int64_t)e->lat_e5 - h->lat_e5;
	int64_t dlon = ((int64_t)e->lon_e5 - h->lon_e5) * h->cos_q15 / 32768;
	return (uint64_t)(dlat * dlat) + (uint64_t)(dlon * dlon);
}

/* (ka, a) before (kb, b) */
static bool before(uint64_t ka, uint16_t a, uint64_t kb, uint16_t b)
{
	return ka < kb || (ka == kb && a < b);
}

static bool in_prov(uint16_t i, uint8_t prov)
{
	return prov == RPT_PROV_ALL || rpt_table[i].prov == prov;
}

uint16_t rpt_next_in(const rpt_here_t *h, uint16_t cur, int dir, uint8_t prov)
{
	uint16_t n = (uint16_t)rpt_info.count, best = RPT_NONE;
	uint64_t kc = cur < n ? key(h, cur) : 0, kb = 0;
	if (cur != RPT_NONE && cur >= n)
		return RPT_NONE;
	for (uint16_t i = 0; i < n; i++) {
		if (!in_prov(i, prov))
			continue;
		uint64_t k = key(h, i);
		if (cur != RPT_NONE && (dir > 0 ? !before(kc, cur, k, i) : !before(k, i, kc, cur)))
			continue;
		if (best == RPT_NONE || (dir > 0 ? before(k, i, kb, best) : before(kb, best, k, i))) {
			best = i;
			kb = k;
		}
	}
	return best;
}

uint16_t rpt_nearest_if(const rpt_here_t *h, bool (*ok)(const rpt_entry_t *e, void *ctx), void *ctx)
{
	uint16_t n = (uint16_t)rpt_info.count, best = RPT_NONE;
	uint64_t kb = 0;
	if (!h->ok)
		return RPT_NONE;
	for (uint16_t i = 0; i < n; i++) {
		const rpt_entry_t *e = &rpt_table[i];
		if (e->lat_e5 == RPT_NOPOS || e->lon_e5 == RPT_NOPOS || !ok(e, ctx))
			continue;
		uint64_t k = key(h, i);
		if (best == RPT_NONE || k < kb) {
			best = i;
			kb = k;
		}
	}
	return best;
}

uint16_t rpt_rank_in(const rpt_here_t *h, uint16_t i, uint8_t prov)
{
	uint16_t n = (uint16_t)rpt_info.count, r = 0;
	if (i >= n)
		return 0;
	uint64_t ki = key(h, i);
	for (uint16_t j = 0; j < n; j++)
		if (in_prov(j, prov) && before(key(h, j), j, ki, i))
			r++;
	return r;
}

uint16_t rpt_count_in(uint8_t prov)
{
	uint16_t n = (uint16_t)rpt_info.count, c = 0;
	for (uint16_t i = 0; i < n; i++)
		c += in_prov(i, prov);
	return c;
}

bool rpt_way(const rpt_entry_t *e, int32_t lat_e7, int32_t lon_e7, uint32_t *meters, uint16_t *bearing)
{
	if (e->lat_e5 == RPT_NOPOS || e->lon_e5 == RPT_NOPOS)
		return false;
	*meters = aprs_distance_m(lat_e7, lon_e7, e->lat_e5 * 100, e->lon_e5 * 100);
	*bearing = aprs_bearing_deg(lat_e7, lon_e7, e->lat_e5 * 100, e->lon_e5 * 100);
	return true;
}

void rpt_locator(const rpt_entry_t *e, char *out)
{
	out[0] = '\0';
	if (e->lat_e5 == RPT_NOPOS || e->lon_e5 == RPT_NOPOS)
		return;
	int32_t lo = e->lon_e5 + 18000000, la = e->lat_e5 + 9000000;	/* 0 .. 360 / 180 degrees, 1e-5 */
	if (lo < 0)
		lo = 0;
	if (lo > 35999999)
		lo = 35999999;
	if (la < 0)
		la = 0;
	if (la > 17999999)
		la = 17999999;
	out[0] = (char)('A' + lo / 2000000);
	out[1] = (char)('A' + la / 1000000);
	out[2] = (char)('0' + lo % 2000000 / 200000);
	out[3] = (char)('0' + la % 1000000 / 100000);
	out[4] = (char)('a' + lo % 200000 * 12 / 100000);
	out[5] = (char)('a' + la % 100000 * 24 / 100000);
	out[6] = '\0';
}

const char *rpt_mode_name(const rpt_entry_t *e)
{
	static const char *const names[RPT_MODES] = { "FM", "DMR", "C4FM", "D-STAR", "P25", "NXDN", "M17" };
	uint8_t m = e->mode & RPT_MODE_MASK;
	return m < RPT_MODES ? names[m] : "?";
}

bool rpt_tune_plan(const rpt_entry_t *e, rpt_tune_t *out)
{
	uint8_t m = e->mode & RPT_MODE_MASK;
	if (m != RPT_FM && m != RPT_DMR)
		return false;
	out->rx_hz = e->rx_hz;
	out->tx_hz = (uint32_t)((int32_t)e->rx_hz + (int32_t)e->off * (int32_t)RPT_STEP_HZ);
	out->dmr = m == RPT_DMR;
	out->narrow = !out->dmr && (e->mode & RPT_NARROW) != 0;
	out->ctcss10 = out->dmr ? 0u : e->ctcss10;
	out->cc = out->dmr ? (uint8_t)(e->dmr & 0x0Fu) : 0u;
	out->ts = out->dmr ? ((e->dmr >> 4) == 2u ? 2u : 1u) : 0u;
	return true;
}
#endif
