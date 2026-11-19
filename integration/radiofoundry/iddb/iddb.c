/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus DMR ID database lookup (iddb.h). Pure: reads the const table, writes only the caller's result. */
#include "config.h"
#if CONFIG_ID_DB
#include "radio/iddb.h"

static const char alphabet[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

void iddb_call(uint32_t packed, char *out)
{
	for (int i = IDDB_CALL_MAX - 1; i >= 0; i--) {
		out[i] = alphabet[packed % 37u];
		packed /= 37u;
	}
	int n = IDDB_CALL_MAX;
	while (n > 0 && out[n - 1] == ' ')
		n--;
	out[n] = '\0';
}

static uint32_t le(const uint8_t *p, unsigned n)
{
	uint32_t v = 0;
	while (n--)
		v = (v << 8) | p[n];
	return v;
}

bool iddb_lookup(uint32_t id, iddb_entry_t *out)
{
	uint32_t blocks = iddb_info.blocks;
	if (id == 0 || id > 0xFFFFFFu || iddb_info.count == 0 || blocks == 0 || id < iddb_index[0].first_id)
		return false;
	// the last block that starts at or below id
	uint32_t lo = 0, hi = blocks;
	while (hi - lo > 1) {
		uint32_t mid = lo + (hi - lo) / 2u;
		if (iddb_index[mid].first_id <= id)
			lo = mid;
		else
			hi = mid;
	}
	const uint8_t *p = iddb_records + iddb_index[lo].offset;
	const uint8_t *end = iddb_records + (lo + 1u < blocks ? iddb_index[lo + 1u].offset : iddb_info.record_bytes);
	uint32_t cur = iddb_index[lo].first_id;
	while (p < end) {
		uint32_t delta = 0;
		for (unsigned shift = 0; p < end && shift < 28u; shift += 7u) {
			uint8_t b = *p++;
			delta |= (uint32_t)(b & 0x7Fu) << shift;
			if (!(b & 0x80u))
				break;
		}
		if (end - p < 6)
			return false;	// a cut record: a table this code did not make
		cur += delta;
		if (cur == id) {
			iddb_call(le(p, 4), out->call);
			out->name = iddb_names + iddb_name_off[le(p + 4, 2)];
			return true;
		}
		if (cur > id)
			return false;
		p += 6;
	}
	return false;
}

bool iddb_text(uint32_t id, char *out, size_t len)
{
	iddb_entry_t e;
	if (len == 0)
		return false;
	out[0] = '\0';
	if (!iddb_lookup(id, &e))
		return false;
	size_t n = 0;
	for (const char *s = e.call; *s && n + 1 < len; s++)
		out[n++] = *s;
	if (e.name[0] != '\0' && n + 2 < len) {
		out[n++] = ' ';
		for (const char *s = e.name; *s && n + 1 < len; s++)
			out[n++] = *s;
	}
	out[n] = '\0';
	return true;
}
#endif
