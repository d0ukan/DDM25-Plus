/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus r15 "channels" (docs/plus.md "Channels, zones and DMR rows"): the zone and codeplug limits on the real
 * codeplug, its RFKV mode records and zone_edit.c over a simulated NOR flash (upstream's zoneEditTest fixture, its main
 * renamed). Checks:
 *   - a zone grows one channel at a time from 26 to 80 (FM, DMR and P25 channels mixed, so the 27th on is every kind),
 *     each read back whole and readable, the 81st refused as full; the same after a power cycle;
 *   - the limits the UI reports (codeplug_zone_capacity 80, CODEPLUG_ZONES_MAX 68, CODEPLUG_CHANNELS_MAX 1024). */
#define main upstream_zone_main
#include "zoneEditTest.c"
#undef main
#include "codeplug/codeplug_kv.h"
#include "codeplug/codeplug_tag.h"

static channel_t kind_of(uint16_t n)
{
	char name[CODEPLUG_NAME_LEN + 1];
	snprintf(name, sizeof name, "Ch %u", (unsigned)n);
	channel_t c = make(name, 430000000u + 25000u * n);
	if (n % 3u == 1u) {
		c.mode = RADIO_MODE_DMR;
		c.contact = 1;
	} else if (n % 3u == 2u) {
		c.mode = RADIO_MODE_P25;
		c.p25_nac = 0x293;
		c.p25_tg = 1;
		c.p25_id = 1;
		c.dmr_id = 1;
		c.p25_squelch = 1;
	}
	return c;
}

static void test_zone_grows(void)
{
	fresh();
	for (uint16_t n = 1; n <= 100; n++) {
		channel_t c = kind_of(n);
		CHECK(codeplug_set_channel(n, &c));
	}
	uint16_t first[26];
	for (uint16_t i = 0; i < 26; i++)
		first[i] = (uint16_t)(i + 1u);
	zone_t z = zone_of("Big", first, 26);
	uint16_t at;
	/* The first zone made keeps "All channels" (stored first, its first 80 channels) as zone 0: this one is 1 */
	CHECK(zone_edit_create(&z, &at) == ZONE_EDIT_OK && at == 1 && codeplug_zone_count() == 2);
	CHECK(codeplug_zone_capacity() == CODEPLUG_ZONE_MAX && CODEPLUG_ZONE_MAX == 80);
	CHECK(CODEPLUG_ZONES_MAX == 68 && CODEPLUG_CHANNELS_MAX == 1024);
	for (uint16_t k = 27; k <= 80; k++) {
		CHECK(zone_edit_insert(at, (uint16_t)(k - 1u), k) == ZONE_EDIT_OK);
		zone_t r;
		CHECK(codeplug_zone(at, &r) && r.count == k && r.channels[k - 1u] == k);
	}
	CHECK(zone_edit_insert(at, 80, 81) == ZONE_EDIT_FULL);
	boot_again();
	zone_t r;
	CHECK(codeplug_zone(at, &r) && r.count == 80);
	for (uint16_t i = 0; i < r.count; i++) {
		channel_t c;
		CHECK(r.channels[i] == i + 1u && codeplug_channel(r.channels[i], &c));
		CHECK(c.mode == kind_of(r.channels[i]).mode);
	}
}

int main(void)
{
	if (upstream_zone_main() != 0)
		return 1;
	failures = 0;
	test_zone_grows();
	printf("Plus zone capacity: %s\n", failures ? "FAILED" : "all passed");
	return failures != 0;
}
