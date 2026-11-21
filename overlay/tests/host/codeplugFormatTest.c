// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Copyright (c) 2026 Open25-DM32 Contributors
// Native channel/contact ownership and optional refusal of retired persisted formats.
#include <stdio.h>
#include <string.h>
#include "codeplug/codeplug.h"
#include "codeplug/codeplug_kv.h"
#include "codeplug/codeplug_tag.h"
#include "p25/p25_codeplug.h"
#include "radio/radio_p25.h"
#include "storage/storage.h"
#include "sim/sim_flash.h"

static unsigned checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

void storage_lock(void) {}
void storage_unlock(void) {}
static unsigned provider_opens;
static uint8_t queued;
static uint32_t air_fit(const air_profile_t *p) { (void)p; return AIR_FIT_RX | AIR_FIT_TX | AIR_FIT_SOFT; }
static bool air_open(const air_profile_t *p, air_event_cb_t cb, void *ctx)
{ (void)p; (void)cb; (void)ctx; provider_opens++; queued = 0; return true; }
static void air_close(void) { queued = 0; }
static void air_restart(void) {}
static bool air_tx_begin(void) { return true; }
static bool air_queue(const air_tx_chunk_t *c) { (void)c; if (queued == 4) return false; queued++; return true; }
static uint8_t air_space(void) { return (uint8_t)(4 - queued); }
static void air_tx_end(void) { queued = 0; }
static uint32_t air_now(void) { return 1000; }
static const hal_air_t air = {
	.fit = air_fit, .open = air_open, .close = air_close, .rx_restart = air_restart,
	.tx_begin = air_tx_begin, .tx_queue = air_queue, .tx_space = air_space, .tx_end = air_tx_end,
	.tx_abort = air_tx_end, .now_us = air_now, .tx_chunk_syms = 144, .tx_queue_depth = 4, .tx_lead_ms = 30,
};

static const hal_flash_map_t map = {
	.settings_addr = 0x11000u, .settings_size = 0x8000u, .journal_addr = 0x19000u,
	.openfw_eeprom = 0, .openfw_flash = 0x20000u, .openfw_end = 0x200000u,
	.calibration_addr = 0x10000u, .calibration_size = 0x200u,
};
static band_t bands[] = { { 136000000u, 174000000u, true }, { 400000000u, 480000000u, true } };
const board_t board = { .name = "test", .bands = bands, .band_count = 2, .power_count = 9, .flash = &sim_flash, .air = &air };

static channel_t channel(const char *name)
{
	channel_t c = {0};
	strncpy(c.name, name, CODEPLUG_NAME_LEN);
	c.rx_hz = c.tx_hz = 446050000u;
	c.mode = RADIO_MODE_FM;
	c.bw = RADIO_BW_12K5;
	c.color_code = c.timeslot = 1;
	c.system = CODEPLUG_SYSTEM_NONE;
	return c;
}

static void fresh(void)
{
	sim_flash_create(0x200000u, &map);
	CHECK(storage_init());
	codeplug_accept(false);
	CHECK(codeplug_init() == CODEPLUG_BUILTIN);
}

static void raw_modes(void)
{
	const uint8_t retired[] = { 2u, 0x0Du, 0x0Eu, 0xA5u, 0xFFu };
	for (unsigned i = 0; i < sizeof retired; i++) {
		fresh();
		channel_t c = channel("Mode test"), back;
		CHECK(codeplug_set_channel(10, &c));
#if CONFIG_CPS_RFKV_ONLY
		uint8_t valid_sector[STORAGE_SECTOR];
		memcpy(valid_sector, sim_flash_data + 0x3000u, sizeof valid_sector);
#endif
		uint8_t r[CODEPLUG_OPENFW_CHANNEL_SIZE];
		memset(r, 0xFF, sizeof r);
		codeplug_openfw_encode(&c, r);
		r[0x18] = retired[i];
#if CONFIG_CPS_RFKV_ONLY
		CHECK(!codeplug_openfw_decode(r, &back));
#else
		CHECK(codeplug_openfw_decode(r, &back) && back.mode == RADIO_MODE_FM);
#endif
		memcpy(sim_flash_data + 0x3790u + 9u * CODEPLUG_OPENFW_CHANNEL_SIZE, r, sizeof r);
		uint32_t before = storage_crc32(0, sim_flash_data, 0x200000u);
		codeplug_reload();
		codeplug_accept(true);
		codeplug_reload();
#if CONFIG_CPS_RFKV_ONLY
		CHECK(codeplug_foreign() && codeplug_source() == CODEPLUG_BUILTIN);
		CHECK(!codeplug_set_channel(10, &c));
		CHECK(storage_crc32(0, sim_flash_data, 0x200000u) == before);
		// The native CPS commit remains available to replace refused programming.
		CHECK(codeplug_kv_commit(0x3000u, valid_sector));
		CHECK(codeplug_written() == CODEPLUG_OPENFW && !codeplug_foreign());
		memcpy(sim_flash_data + 0x3790u + 9u * CODEPLUG_OPENFW_CHANNEL_SIZE, r, sizeof r);
		codeplug_reload();
		CHECK(codeplug_foreign());
		CHECK(codeplug_start_new());
		CHECK(!codeplug_foreign() && codeplug_source() == CODEPLUG_OPENFW);
		CHECK(codeplug_channel(1, &back) && back.mode == RADIO_MODE_FM);
#else
		(void)before;
		CHECK(!codeplug_foreign() && codeplug_channel(10, &back));
#endif
	}
}

static void p25_contact_owner(void)
{
	fresh();
	contact_t target = { .name = "Group", .id = 1, .type = CONTACT_GROUP };
	CHECK(codeplug_set_contact(1, &target));
	channel_t c = channel("P25 native"), back;
	c.mode = RADIO_MODE_DMR;
	c.rx_only = true;
	c.kind = CODEPLUG_KIND_P25;
	c.contact = 1;
	c.dmr_id = 101;
	p25_conv_t conv = { .rx_nac = 0x293, .tx_nac = 0x293, .squelch = 0 };
	CHECK(p25_conv_save(10, &c, &conv));
	CHECK(codeplug_channel(10, &back) && back.kind == CODEPLUG_KIND_P25);
	p25_conv_cfg_t cfg;
	CHECK(p25_conv_load(10, &back, &cfg) && cfg.target == 1 && cfg.unit_id == 101);
	target.id = 9;
	CHECK(codeplug_set_contact(1, &target));
	conv.rx_nac = conv.tx_nac = 0x123;
	CHECK(p25_conv_save(10, &back, &conv));
	codeplug_reload();
	CHECK(codeplug_channel(10, &back));
	CHECK(p25_conv_load(10, &back, &cfg) && cfg.target == 9 && cfg.conv.rx_nac == 0x123);
	radio_p25_init(NULL);
	CHECK(radio_p25_begin(&back, 10, 101) && radio_p25_running());

	uint8_t old[9] = {0};
	p25_conv_encode(&conv, old);
	old[5] = 1;
	old[6] = 0x09;
	old[7] = 0x03;	// retired explicit TG 777 conflicts with native contact TG 9
	codeplug_tag_t tag = { .kind = CODEPLUG_KIND_P25, .system = CODEPLUG_SYSTEM_NONE, .flags = CODEPLUG_TAG_TX };
	CHECK(codeplug_tag_write(10, back.name, &tag, old, sizeof old));
	uint32_t before = storage_crc32(0, sim_flash_data, 0x200000u);
#if CONFIG_CPS_RFKV_ONLY
	CHECK(!p25_conv_load(10, &back, &cfg));
	CHECK(!p25_conv_save(10, &back, &conv));
	CHECK(storage_crc32(0, sim_flash_data, 0x200000u) == before);
	radio_p25_apply(&back, 10, 101);
	CHECK(!radio_p25_running() && !radio_p25_tx_prepare());
	unsigned opened = provider_opens;
	CHECK(!radio_p25_begin(&back, 10, 101) && !radio_p25_running());
	CHECK(provider_opens == opened && !radio_p25_tx_prepare());
#else
	(void)before;
	CHECK(p25_conv_load(10, &back, &cfg) && cfg.target == 9);
#endif
	radio_p25_end();
	// A fresh native tag has exactly one talk-target owner again.
	uint8_t native[P25_CP_CONV_LEN];
	p25_conv_encode(&conv, native);
	CHECK(codeplug_tag_write(10, back.name, &tag, native, sizeof native));
	codeplug_reload();
	CHECK(codeplug_channel(10, &back));
	CHECK(p25_conv_load(10, &back, &cfg) && cfg.target == 9 && cfg.unit_id == 101);
	CHECK(radio_p25_begin(&back, 10, 101) && radio_p25_tx_prepare());
	radio_p25_end();
	CHECK(sim_flash_violations() == 0);
}

int main(void)
{
	raw_modes();
	p25_contact_owner();
	sim_flash_free();
	printf("codeplugFormatTest: strict=%d, %u checks, %s\n", CONFIG_CPS_RFKV_ONLY, checks, failures ? "FAILED" : "PASS");
	return failures ? 1 : 0;
}
