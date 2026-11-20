/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus DOTA Transfer: the codeplug's side of an import (docs/dota.md 10). Included at the end of
 * core/codeplug/codeplug.c (tools/plus_dota.py), so it reaches the in-use bitmaps and the zone slots.
 *
 * Staging: a received channel goes into a free channel slot, a received zone into a free zone slot, both WITHOUT their
 * in-use bits. A record whose bit is clear is no channel (zone) at all: nothing shows, a power cut leaves nothing,
 * and the next write to that slot blanks it first (codeplug_set_channel). The "rank-th free slot" names the slot of
 * the transfer's rank-th channel (zone): staging never changes the bitmaps, so it is the same slot for the whole
 * transfer, in any order. The commit sets the staged channels' bits, one rewrite per bank. */

bool codeplug_stage_channel(uint16_t number, const channel_t *ch)
{
	storage_lock();
	bool ok = source == CODEPLUG_OPENFW && number >= 1 && number <= CH_MAX && !is_used(number) &&
		  set_channel_as(number, ch, false);
	storage_unlock();
	return ok;
}

uint16_t codeplug_free_nth(uint16_t rank)
{
	uint16_t found = 0, seen = 0;
	storage_lock();
	for (uint16_t n = 1; source == CODEPLUG_OPENFW && n <= CH_MAX; n++) {
		if (!is_used(n) && seen++ == rank) {
			found = n;
			break;
		}
	}
	storage_unlock();
	return found;
}

uint16_t codeplug_free_channels(void)
{
	uint16_t free_n = 0;
	storage_lock();
	for (uint16_t n = 1; source == CODEPLUG_OPENFW && n <= CH_MAX; n++) {
		free_n += is_used(n) ? 0u : 1u;
	}
	storage_unlock();
	return free_n;
}

bool codeplug_commit_staged(const uint8_t *ranks, uint16_t count)
{
	uint8_t banks = 0;
	uint16_t rank = 0;
	storage_lock();
	bool ok = source == CODEPLUG_OPENFW;
	for (uint16_t n = 1; ok && n <= CH_MAX && rank < count; n++) {
		if (is_used(n)) {
			continue;
		}
		if ((ranks[rank >> 3] >> (rank & 7u)) & 1u) {
			set_used(n, true);
			banks |= (uint8_t)(1u << ((n - 1u) / CH_PER_BANK));
		}
		rank++;
	}
	for (unsigned b = 0; ok && b < CH_BANKS; b++) {
		if (banks & (1u << b)) {
			ok = write_bitmap((uint16_t)(b * CH_PER_BANK + 1u));
		}
	}
	if (!ok) {
		index_channels();	/* back to what the flash holds */
	}
	find_max();
	serial++;
	storage_unlock();
	return ok;
}

#if CONFIG_DMR
/* The index (1-based) of the first contact with this ID and type, 0 when there is none. One record at a time under
 * one lock: no buffer (the caller runs on the start-up task's small stack) */
uint16_t codeplug_contact_index(uint32_t id, uint8_t type)
{
	uint8_t r[CONTACT_SIZE];
	uint16_t found = 0;
	storage_lock();
	for (uint16_t i = 1; source == CODEPLUG_OPENFW && i <= CODEPLUG_CONTACTS_MAX && found == 0; i++) {
		if (!flash_read(CONTACT_BASE + (i - 1u) * CONTACT_SIZE, r, sizeof r) || r[0] == 0xFF || r[0x14] != type) {
			continue;
		}
		uint32_t have = type == CONTACT_ALL_CALL ? 0xFFFFFFu : bcd_id(&r[0x10]);
		if (have == id && have != 0) {
			found = i;
		}
	}
	storage_unlock();
	return found;
}
#endif

/* The rank-th free zone slot of a stored 80-channel zone table, -1 when there is none */
static int zone_free_slot(uint8_t rank)
{
	bool taken[ZONE_SLOTS] = { false };
	if (source != CODEPLUG_OPENFW || zone_count == 0 || zone_channels != 80) {
		return -1;
	}
	for (uint8_t i = 0; i < zone_count; i++) {
		taken[zone_slot[i]] = true;
	}
	for (unsigned slot = 0; slot < ZONE_SLOTS; slot++) {
		if (!taken[slot] && rank-- == 0) {
			return (int)slot;
		}
	}
	return -1;
}

uint8_t codeplug_free_zone_slots(void)
{
	storage_lock();
	uint8_t n = 0;
	while (zone_free_slot(n) >= 0) {
		n++;
	}
	storage_unlock();
	return n;
}

/* A staged zone's record: the name, then 80 channel numbers (0xFFFF: none there) */
bool codeplug_stage_zone(uint8_t rank, const zone_t *z)
{
	uint8_t r[ZONE_RECORD_80];
	storage_lock();
	int slot = zone_free_slot(rank);
	bool ok = slot >= 0;
	if (ok) {
		memset(r, 0xFF, sizeof r);
		put_name(r, z->name, ZONE_NAME_LEN);
		for (uint16_t i = 0; i < z->count && i < 80u; i++) {
			put16(&r[ZONE_NAME_LEN + 2u * i], z->channels[i]);
		}
		ok = flash_write(ZONE_DATA + (uint32_t)slot * ZONE_RECORD_80, r, ZONE_RECORD_80);
	}
	storage_unlock();
	return ok;
}

bool codeplug_staged_zone(uint8_t rank, zone_t *z)
{
	uint8_t r[ZONE_RECORD_80];
	storage_lock();
	int slot = zone_free_slot(rank);
	bool ok = slot >= 0 && flash_read(ZONE_DATA + (uint32_t)slot * ZONE_RECORD_80, r, ZONE_RECORD_80);
	storage_unlock();
	if (ok) {
		memset(z, 0, sizeof *z);
		copy_name(z->name, r, ZONE_NAME_LEN);
		z->count = 80;
		for (unsigned i = 0; i < 80u; i++) {
			z->channels[i] = le16(&r[ZONE_NAME_LEN + 2u * i]);
		}
	}
	return ok;
}
