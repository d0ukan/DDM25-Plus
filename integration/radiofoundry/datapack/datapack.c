/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus user data packs: the two-slot store (datapack.h). Installed as core/storage/datapack.c. */
#include "config.h"
#if CONFIG_DATAPACK
#include <string.h>
#include "board.h"
#include "storage/storage.h"
#include "storage/datapack.h"

/* RadioID: two 256 KB slots; air band: two 16 KB slots after them; the rest of the region spare */
#define ID_SLOT		0x40000u
#define AIR_SLOT	0x4000u
#define AIR_BASE	(DPK_BASE + 2u * ID_SLOT)

static struct {
	dpk_info_t h;
	uint8_t slot;		/* 0xFF: none in force */
	uint8_t state;		/* DPK_OK / DPK_NONE / DPK_BAD */
} cur[DPK_TYPES];

uint32_t dpk_slot_size(uint8_t type)
{
	return type == DPK_RADIOID ? ID_SLOT : type == DPK_AIR ? AIR_SLOT : 0u;
}

uint32_t dpk_slot_addr(uint8_t type, uint8_t slot)
{
	uint32_t base = type == DPK_RADIOID ? DPK_BASE : AIR_BASE;
	return dpk_slot_size(type) != 0u && slot < 2u ? base + slot * dpk_slot_size(type) : 0u;
}

static bool inside(uint32_t addr, uint32_t len)
{
	return addr >= DPK_BASE && len <= DPK_END - addr;
}

static bool rd(uint32_t addr, void *buf, uint32_t len)
{
	return board.flash != NULL && board.flash->read != NULL && board.flash->size >= DPK_END &&
	       board.flash->read(addr, buf, len);
}

bool dpk_erase(uint32_t addr)
{
	if (!inside(addr, DPK_SECTOR) || addr % DPK_SECTOR != 0u || board.flash == NULL || board.flash->size < DPK_END)
		return false;
	storage_lock();
	bool ok = board.flash->erase(addr);
	storage_unlock();
	return ok;
}

bool dpk_program(uint32_t addr, const void *data, uint32_t len)
{
	if (!inside(addr, len) || board.flash == NULL || board.flash->size < DPK_END)
		return false;
	storage_lock();
	bool ok = board.flash->write(addr, data, len);
	storage_unlock();
	return ok;
}

void dpk_seal(dpk_hdr_t *h)
{
	h->hcrc = storage_crc32(0, h, offsetof(dpk_hdr_t, hcrc));
}

bool dpk_hdr_ok(const dpk_hdr_t *h, uint8_t type)
{
	return memcmp(h->magic, "DPK1", 4) == 0 && h->type == type && h->fmt == 1u &&
	       h->len <= dpk_slot_size(type) - DPK_HDR && h->hcrc == storage_crc32(0, h, offsetof(dpk_hdr_t, hcrc));
}

/* The payload's CRC-32 as the flash holds it now */
static bool payload_crc(uint32_t addr, uint32_t len, uint32_t *crc)
{
	uint8_t b[64];
	uint32_t c = 0;
	for (uint32_t off = 0; off < len; off += sizeof b) {
		uint32_t n = len - off < sizeof b ? len - off : (uint32_t)sizeof b;
		if (!rd(addr + off, b, n))
			return false;
		c = storage_crc32(c, b, n);
	}
	*crc = c;
	return true;
}

/* A slot's header if the slot holds a whole, checked pack; *bad: something is there that does not check */
static bool slot_ok(uint8_t type, uint8_t slot, dpk_hdr_t *h, bool *bad)
{
	uint32_t a = dpk_slot_addr(type, slot), crc;
	if (!rd(a, h, sizeof *h))
		return false;
	static const uint8_t erased[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
	if (memcmp(h->magic, erased, 4) == 0)
		return false;		/* empty (or being written: the header comes last) */
	if (dpk_hdr_ok(h, type) && payload_crc(a + DPK_HDR, h->len, &crc) && crc == h->crc)
		return true;
	*bad = true;
	return false;
}

static void scan_type(uint8_t type)
{
	dpk_hdr_t h;
	bool bad = false;
	uint8_t i = (uint8_t)(type - 1u);
	cur[i].slot = 0xFF;
	for (uint8_t s = 0; s < 2u; s++) {
		if (slot_ok(type, s, &h, &bad) && (cur[i].slot == 0xFF || h.seq > cur[i].h.seq)) {
			cur[i].h.seq = h.seq;
			cur[i].h.len = h.len;
			cur[i].h.crc = h.crc;
			memcpy(cur[i].h.date, h.date, sizeof h.date);
			memcpy(cur[i].h.name, h.name, sizeof h.name);
			cur[i].slot = s;
		}
	}
	cur[i].state = cur[i].slot != 0xFF ? DPK_OK : bad ? DPK_BAD : DPK_NONE;
}

void dpk_scan(void)
{
	for (uint8_t t = 1; t <= DPK_TYPES; t++)
		scan_type(t);
}

const dpk_info_t *dpk_active(uint8_t type)
{
	return type >= 1u && type <= DPK_TYPES && cur[type - 1u].state == DPK_OK ? &cur[type - 1u].h : NULL;
}

uint8_t dpk_state(uint8_t type)
{
	return type >= 1u && type <= DPK_TYPES ? cur[type - 1u].state : DPK_NONE;
}

bool dpk_read(uint8_t type, uint32_t off, void *buf, uint32_t len)
{
	const dpk_info_t *h = dpk_active(type);
	return h != NULL && off <= h->len && len <= h->len - off &&
	       rd(dpk_slot_addr(type, cur[type - 1u].slot) + DPK_HDR + off, buf, len);
}

bool dpk_begin(dpk_writer_t *w, const dpk_hdr_t *h)
{
	w->addr = 0;
	if (h->type < 1u || h->type > DPK_TYPES || h->len > dpk_slot_size(h->type) - DPK_HDR)
		return false;
	uint8_t i = (uint8_t)(h->type - 1u);
	uint8_t slot = cur[i].state == DPK_OK ? (uint8_t)(1u - cur[i].slot) : 0u;
	uint32_t a = dpk_slot_addr(h->type, slot);
	for (uint32_t s = 0; s < DPK_HDR + h->len; s += DPK_SECTOR)
		if (!dpk_erase(a + s))
			return false;
	w->h = *h;
	w->h.seq = cur[i].state == DPK_OK ? cur[i].h.seq + 1u : 1u;
	w->addr = a;
	return true;
}

bool dpk_put(dpk_writer_t *w, uint32_t off, const void *data, uint32_t len)
{
	return w->addr != 0u && off <= w->h.len && len <= w->h.len - off && dpk_program(w->addr + DPK_HDR + off, data, len);
}

bool dpk_commit(dpk_writer_t *w)
{
	uint32_t crc, a = w->addr;
	w->addr = 0;		/* closed, whatever happens now */
	if (a == 0u || !payload_crc(a + DPK_HDR, w->h.len, &crc) || crc != w->h.crc)
		return false;
	memcpy(w->h.magic, "DPK1", 4);
	dpk_seal(&w->h);
	bool ok = dpk_program(a, &w->h, sizeof w->h);
	scan_type(w->h.type);
	return ok && dpk_active(w->h.type) != NULL && dpk_active(w->h.type)->seq == w->h.seq;
}
#endif
