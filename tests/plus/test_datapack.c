/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus user data packs (core/storage/datapack.c) on the simulated 16 MiB data chip: the region guard (nothing
 * outside 0xE70000-0xEFFFFF is ever erased or programmed), the two slots and their sequence, the header written last,
 * the payload CRC read back, broken slots, and a power cut at every flash operation of a pack being written (the pack
 * in force before stays in force, or the new one is whole). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "storage/storage.h"
#include "storage/datapack.h"
#include "sim/sim_flash.h"
#include "board.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

void storage_lock(void) {}
void storage_unlock(void) {}
void storage_quick_lock(void) {}
void storage_quick_unlock(void) {}

#define FLASH_SIZE	0x1000000u
static const hal_flash_map_t map = {
	.settings_addr = 0x711000u, .settings_size = 0x8000u, .journal_addr = 0x719000u,
	.openfw_eeprom = 0x700000u, .openfw_flash = 0x720000u, .openfw_end = 0xF00000u,
	.calibration_addr = 0x710000u, .calibration_size = 0x200u,
};
const board_t board = { .name = "test", .flash = &sim_flash };

static uint32_t rng = 1;
static uint8_t rnd(void)
{
	rng = rng * 1103515245u + 12345u;
	return (uint8_t)(rng >> 16);
}

/* Everything outside the region, hashed (it must never change) */
static uint32_t outside(void)
{
	return storage_crc32(storage_crc32(0, sim_flash_data, DPK_BASE), sim_flash_data + DPK_END, FLASH_SIZE - DPK_END);
}

static dpk_hdr_t header(uint8_t type, const uint8_t *p, uint32_t len, const char *name)
{
	dpk_hdr_t h;
	memset(&h, 0, sizeof h);
	memcpy(h.magic, "DPK1", 4);
	h.type = type;
	h.fmt = 1;
	h.len = len;
	h.crc = storage_crc32(0, p, len);
	memcpy(h.date, "20261120", 8);
	strncpy(h.name, name, sizeof h.name);
	strncpy(h.tag, "test", sizeof h.tag);
	return h;
}

/* A whole pack, written in pieces of step bytes in a scrambled order */
static bool write_pack(uint8_t type, const uint8_t *p, uint32_t len, const char *name, uint32_t step)
{
	dpk_writer_t w;
	dpk_hdr_t h = header(type, p, len, name);
	if (!dpk_begin(&w, &h))
		return false;
	uint32_t pieces = (len + step - 1u) / step;
	for (uint32_t k = 0; k < pieces; k++) {
		uint32_t i = (k * 7u + 3u) % pieces;	/* every piece once, out of order */
		uint32_t off = i * step, n = len - off < step ? len - off : step;
		if (pieces % 7u == 0u)
			i = k, off = i * step, n = len - off < step ? len - off : step;
		if (!dpk_put(&w, off, p + off, n))
			return false;
	}
	return dpk_commit(&w);
}

static bool active_is(uint8_t type, const uint8_t *p, uint32_t len)
{
	const dpk_info_t *h = dpk_active(type);
	if (h == NULL || h->len != len)
		return false;
	uint8_t b[256];
	for (uint32_t off = 0; off < len; off += sizeof b) {
		uint32_t n = len - off < sizeof b ? len - off : (uint32_t)sizeof b;
		if (!dpk_read(type, off, b, n) || memcmp(b, p + off, n) != 0)
			return false;
	}
	return true;
}

static void test_guard(void)
{
	uint8_t b[8] = { 0 };
	CHECK(!dpk_erase(DPK_BASE - DPK_SECTOR) && !dpk_erase(DPK_END) && !dpk_erase(0x710000u) && !dpk_erase(0));
	CHECK(!dpk_erase(DPK_BASE + 1u));	/* not a sector start */
	CHECK(!dpk_program(DPK_BASE - 1u, b, 2) && !dpk_program(DPK_END - 4u, b, 8) && !dpk_program(DPK_END, b, 1));
	CHECK(!dpk_program(0xFFFFFFFCu, b, 8));	/* wraps */
	CHECK(dpk_slot_addr(DPK_RADIOID, 0) == 0xE70000u && dpk_slot_addr(DPK_RADIOID, 1) == 0xEB0000u);
	CHECK(dpk_slot_addr(DPK_AIR, 0) == 0xEF0000u && dpk_slot_addr(DPK_AIR, 1) == 0xEF4000u);
	CHECK(dpk_slot_addr(DPK_AIR, 1) + dpk_slot_size(DPK_AIR) <= DPK_END && dpk_slot_addr(3, 0) == 0u);
	/* a header claiming more than its slot holds is refused before anything is erased */
	dpk_writer_t w;
	dpk_hdr_t h = header(DPK_AIR, b, 8, "x");
	h.len = dpk_slot_size(DPK_AIR);
	CHECK(!dpk_begin(&w, &h));
}

static uint8_t pa[100000], pb[100000], pc[12000], air[6000];

static void test_slots(void)
{
	dpk_scan();
	CHECK(dpk_active(DPK_RADIOID) == NULL && dpk_state(DPK_RADIOID) == DPK_NONE && dpk_state(DPK_AIR) == DPK_NONE);
	for (size_t i = 0; i < sizeof pa; i++)
		pa[i] = rnd(), pb[i] = rnd();
	for (size_t i = 0; i < sizeof air; i++)
		air[i] = rnd();
	CHECK(write_pack(DPK_RADIOID, pa, sizeof pa, "RadioID TR", 192));
	CHECK(active_is(DPK_RADIOID, pa, sizeof pa) && dpk_active(DPK_RADIOID)->seq == 1u);
	CHECK(write_pack(DPK_RADIOID, pb, sizeof pb, "RadioID TR2", 500));
	CHECK(active_is(DPK_RADIOID, pb, sizeof pb) && dpk_active(DPK_RADIOID)->seq == 2u);
	CHECK(strncmp(dpk_active(DPK_RADIOID)->name, "RadioID TR2", 16) == 0);
	CHECK(memcmp(dpk_active(DPK_RADIOID)->date, "20261120", 8) == 0);
	CHECK(write_pack(DPK_AIR, air, sizeof air, "TR aerodromes", 128) && active_is(DPK_AIR, air, sizeof air));
	CHECK(active_is(DPK_RADIOID, pb, sizeof pb));	/* the types keep to their own slots */
	/* after a restart the same */
	dpk_scan();
	CHECK(active_is(DPK_RADIOID, pb, sizeof pb) && active_is(DPK_AIR, air, sizeof air));
	/* a wrong payload never commits: the old pack stays */
	dpk_writer_t w;
	dpk_hdr_t h = header(DPK_RADIOID, pa, 4096, "bad");
	CHECK(dpk_begin(&w, &h));
	pa[5] ^= 1u;
	CHECK(dpk_put(&w, 0, pa, 4096));
	pa[5] ^= 1u;
	CHECK(!dpk_commit(&w) && active_is(DPK_RADIOID, pb, sizeof pb));
	CHECK(!dpk_put(&w, 0, pa, 1));		/* the writer is closed */
	/* a byte outside the payload or written twice: refused (dpk_put's range) */
	CHECK(dpk_begin(&w, &h) && !dpk_put(&w, 4090, pa, 10));
}

static void test_broken(void)
{
	/* the pack in force spoilt on the chip (a flipped bit): the older one takes over; both spoilt: "broken" */
	uint32_t a1 = dpk_slot_addr(DPK_RADIOID, 1), a0 = dpk_slot_addr(DPK_RADIOID, 0);
	/* slot 0 holds pa? rewrite it so it does */
	CHECK(write_pack(DPK_RADIOID, pa, sizeof pa, "RadioID TR", 1000));	/* goes into the slot not in force */
	uint32_t in_force = dpk_active(DPK_RADIOID)->seq;
	uint32_t a = in_force % 2u == 1u ? a0 : a1;	/* sequence 1, 3, ... landed in slot 0 */
	(void)a;
	uint32_t live = active_is(DPK_RADIOID, pa, sizeof pa) ? 1u : 0u;
	CHECK(live == 1u);
	/* flip a payload bit of whichever slot is in force */
	uint32_t slot_addr = memcmp(sim_flash_data + a0 + DPK_HDR, pa, 64) == 0 ? a0 : a1;
	sim_flash_data[slot_addr + DPK_HDR + 100] ^= 0x10u;
	dpk_scan();
	CHECK(active_is(DPK_RADIOID, pb, sizeof pb) && dpk_state(DPK_RADIOID) == DPK_OK);
	uint32_t other = slot_addr == a0 ? a1 : a0;
	sim_flash_data[other + 20] ^= 0x01u;		/* its header's length: the header CRC fails */
	dpk_scan();
	CHECK(dpk_active(DPK_RADIOID) == NULL && dpk_state(DPK_RADIOID) == DPK_BAD);
	CHECK(active_is(DPK_AIR, air, sizeof air));
	/* a fresh pack after that */
	CHECK(write_pack(DPK_RADIOID, pb, sizeof pb, "again", 4096) && active_is(DPK_RADIOID, pb, sizeof pb));
}

/* A power cut at every flash operation while a pack is written: after power on the old pack, or the whole new one */
static void test_cuts(void)
{
	for (size_t i = 0; i < sizeof pc; i++)
		pc[i] = rnd();
	uint8_t *before = malloc(DPK_END - DPK_BASE);
	memcpy(before, sim_flash_data + DPK_BASE, DPK_END - DPK_BASE);
	uint32_t old_seq = dpk_active(DPK_RADIOID)->seq;
	/* how many flash operations a whole write takes */
	uint32_t u0 = sim_flash_units();
	CHECK(write_pack(DPK_RADIOID, pc, sizeof pc, "cut", 300));
	uint32_t units = sim_flash_units() - u0;
	int olds = 0, news = 0;
	for (uint32_t cut = 1; cut <= units + 1u; cut++) {
		memcpy(sim_flash_data + DPK_BASE, before, DPK_END - DPK_BASE);
		dpk_scan();
		sim_flash_cut_after(cut);
		(void)write_pack(DPK_RADIOID, pc, sizeof pc, "cut", 300);
		sim_flash_power_on();
		dpk_scan();
		bool is_new = active_is(DPK_RADIOID, pc, sizeof pc);
		bool is_old = active_is(DPK_RADIOID, pb, sizeof pb) && dpk_active(DPK_RADIOID)->seq == old_seq;
		CHECK(is_new || is_old);
		olds += is_old;
		news += is_new;
	}
	sim_flash_cut_after(0);
	CHECK(olds > 0 && news > 0);
	printf("  power cuts at each of %u flash operations: %d left the old pack, %d the whole new one\n", units + 1u,
	       olds, news);
	free(before);
}

int main(void)
{
	sim_flash_create(FLASH_SIZE, &map);
	for (uint32_t i = 0; i < FLASH_SIZE; i++)
		sim_flash_data[i] = i < DPK_BASE || i >= DPK_END ? (uint8_t)(i * 31u + 7u) : 0xFFu;
	uint32_t h0 = outside();
	test_guard();
	test_slots();
	test_broken();
	test_cuts();
	CHECK(outside() == h0);		/* not one byte outside the region changed */
	CHECK(sim_flash_violations() == 0);
	printf("datapack store: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
