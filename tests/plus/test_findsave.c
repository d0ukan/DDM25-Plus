/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM25 Plus search finds as channels (core/ui/findsave.c): names, the P25 and DMR channel rules (the TX rule above
 * all: RX only unless a talkgroup was heard, our own unit ID is known, the frequency is amateur and in the TX bands),
 * the duplicate rule and the zone name. Built with -DCONFIG_FIND_SAVE=1 -DCONFIG_DMR_ANY=1 (and once without
 * CONFIG_DMR_ANY: plain colour code 1 / slot 1). */
#include <stdio.h>
#include <string.h>
#include "ui/findsave.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static findsave_item_t p25(uint32_t hz, uint16_t nac, uint16_t tg)
{
	findsave_item_t f;
	memset(&f, 0, sizeof f);
	f.kind = FINDSAVE_P25;
	f.hz = hz;
	f.nac = nac;
	f.tg = tg;
	f.cc = FINDSAVE_CC_NONE;
	return f;
}

static findsave_item_t dmr(uint32_t hz, uint8_t cc, uint8_t ts)
{
	findsave_item_t f;
	memset(&f, 0, sizeof f);
	f.kind = FINDSAVE_DMR;
	f.hz = hz;
	f.cc = cc;
	f.ts_mask = ts;
	return f;
}

int main(void)
{
	char name[CODEPLUG_NAME_LEN + 1];
	channel_t ch;

	/* Names: 3 decimals, 4 when the 100 Hz digit needs it; they fit the codeplug's 16 characters */
	findsave_item_t f = p25(433500000, 0x1C8, 655);
	findsave_name(&f, name);
	CHECK(strcmp(name, "P25 433.500") == 0);
	f.hz = 433512500;
	findsave_name(&f, name);
	CHECK(strcmp(name, "P25 433.5125") == 0);
	f = dmr(438700000, 1, 2);
	findsave_name(&f, name);
	CHECK(strcmp(name, "DMR 438.700") == 0);
	f = dmr(1299987500, 1, 2);
	findsave_name(&f, name);
	CHECK(strcmp(name, "DMR 1299.9875") == 0 && strlen(name) <= CODEPLUG_NAME_LEN);
	f = dmr(144012500, 1, 2);
	findsave_name(&f, name);
	CHECK(strcmp(name, "DMR 144.0125") == 0);

	/* P25: the NAC found, Normal squelch (every talkgroup: "TG Any"), TX NAC Same as RX, simplex, 12.5 kHz */
	f = p25(433500000, 0x659, 101);
	findsave_channel(&f, 3101234, true, &ch);
	CHECK(ch.mode == RADIO_MODE_P25 && ch.kind == CODEPLUG_KIND_P25 && ch.p25_nac == 0x659 && ch.p25_squelch == 0);
	CHECK(ch.p25_tx_nac_override == 0 && ch.rx_hz == 433500000 && ch.tx_hz == 433500000 && ch.bw == RADIO_BW_12K5);
	CHECK(ch.power == CODEPLUG_POWER_DEFAULT && strcmp(ch.name, "P25 433.500") == 0);
	/* ... it transmits: a talkgroup heard, our ID known, 70 cm, in the TX band; codeplug_p25_valid agrees */
	CHECK(!ch.rx_only && ch.p25_tg == 101 && ch.p25_id == 3101234 && codeplug_p25_valid(&ch));
	/* no talkgroup heard: RX only (the TG stays 0), still valid */
	f.tg = 0;
	findsave_channel(&f, 3101234, true, &ch);
	CHECK(ch.rx_only && ch.p25_tg == 0 && ch.p25_id == 0 && codeplug_p25_valid(&ch));
	/* our ID unknown: RX only, the talkgroup kept for later */
	f.tg = 101;
	findsave_channel(&f, 0, true, &ch);
	CHECK(ch.rx_only && ch.p25_tg == 101 && codeplug_p25_valid(&ch));
	/* not a TX band of the radio */
	findsave_channel(&f, 3101234, false, &ch);
	CHECK(ch.rx_only);
	/* outside 2 m / 70 cm (the editor's outside-band rule), even in the radio's TX band */
	f.hz = 460025000;
	findsave_channel(&f, 3101234, true, &ch);
	CHECK(ch.rx_only && !findsave_p25_tx(&f, 3101234, true));
	f.hz = 145600000;
	CHECK(findsave_p25_tx(&f, 3101234, true));
	f.hz = 148012500;
	CHECK(!findsave_p25_tx(&f, 3101234, true));
	f.hz = 420000000;
	CHECK(findsave_p25_tx(&f, 3101234, true));
	f.hz = 450000000;
	CHECK(findsave_p25_tx(&f, 3101234, true));
	f.hz = 450012500;
	CHECK(!findsave_p25_tx(&f, 3101234, true));
	/* the NACs that never transmit, an ID past the top */
	f.hz = 433500000;
	f.nac = 0xF7E;
	CHECK(!findsave_p25_tx(&f, 3101234, true));
	f.nac = 0xF7F;
	CHECK(!findsave_p25_tx(&f, 3101234, true));
	f.nac = 0x293;
	CHECK(!findsave_p25_tx(&f, 0xFFFFFF, true) && findsave_p25_tx(&f, 0xFFFFFE, true));
	/* a DMR find never transmits by the P25 rule */
	f = dmr(433500000, 1, 1);
	CHECK(!findsave_p25_tx(&f, 3101234, true));

	/* DMR: colour code and the one slot seen, RX group Any, no contact, RX only */
	f = dmr(438700000, 3, 2);
	findsave_channel(&f, 3101234, true, &ch);
	CHECK(ch.mode == RADIO_MODE_DMR && ch.color_code == 3 && ch.timeslot == 2 && ch.contact == 0 && ch.rx_only);
	CHECK(ch.rx_hz == 438700000 && ch.tx_hz == 438700000 && ch.bw == RADIO_BW_12K5 && strcmp(ch.name, "DMR 438.700") == 0);
#if CONFIG_DMR_ANY
	CHECK(ch.listen_list == CODEPLUG_LIST_ANY);
	f = dmr(438700000, FINDSAVE_CC_NONE, 3);	/* no colour code named, both slots */
	findsave_channel(&f, 0, true, &ch);
	CHECK(ch.color_code == CODEPLUG_CC_ANY && ch.timeslot == CODEPLUG_TS_ANY && ch.rx_only);
	f = dmr(438700000, 7, 0);	/* no slot seen */
	findsave_channel(&f, 0, true, &ch);
	CHECK(ch.color_code == 7 && ch.timeslot == CODEPLUG_TS_ANY);
#else
	f = dmr(438700000, FINDSAVE_CC_NONE, 3);
	findsave_channel(&f, 0, true, &ch);
	CHECK(ch.color_code == 1 && ch.timeslot == 1 && ch.listen_list == 0);
#endif

	/* Duplicates: same mode, frequency and NAC / colour code */
	f = p25(433500000, 0x1C8, 655);
	findsave_channel(&f, 0, true, &ch);
	CHECK(findsave_same(&ch, &f));
	findsave_item_t g = f;
	g.tg = 101;	/* another talkgroup on the same NAC: the same channel (TG Any) */
	CHECK(findsave_same(&ch, &g));
	g.nac = 0x1C9;
	CHECK(!findsave_same(&ch, &g));
	g = f;
	g.hz += 12500;
	CHECK(!findsave_same(&ch, &g));
	g = dmr(433500000, 1, 1);	/* DMR on the P25 channel's frequency: not a duplicate */
	CHECK(!findsave_same(&ch, &g));
	f = dmr(438700000, 1, 1);
	findsave_channel(&f, 0, true, &ch);
	g = f;
	g.ts_mask = 2;		/* the other slot: still the same channel (same CC) */
	CHECK(findsave_same(&ch, &g));
	g.cc = 2;
	CHECK(!findsave_same(&ch, &g));
	g = p25(438700000, 1, 0);
	CHECK(!findsave_same(&ch, &g));
#if CONFIG_DMR_ANY
	f = dmr(438700000, FINDSAVE_CC_NONE, 0);
	findsave_channel(&f, 0, true, &ch);
	CHECK(findsave_same(&ch, &f));
#endif

	/* Zone names */
	findsave_zone_name(1, name);
	CHECK(strcmp(name, "Bulunanlar 1") == 0);
	findsave_zone_name(999, name);
	CHECK(strcmp(name, "Bulunanlar 999") == 0 && strlen(name) <= CODEPLUG_NAME_LEN);
	CHECK(findsave_zone_number("Bulunanlar 1") == 1 && findsave_zone_number("Bulunanlar 42") == 42);
	CHECK(findsave_zone_number("Bulunanlar 999") == 999 && findsave_zone_number("Bulunanlar 1000") == 0);
	CHECK(findsave_zone_number("Bulunanlar") == 0 && findsave_zone_number("Bulunanlar ") == 0);
	CHECK(findsave_zone_number("Bulunanlar 0") == 0 && findsave_zone_number("Bulunanlar 01") == 0);
	CHECK(findsave_zone_number("Bulunanlar 3x") == 0 && findsave_zone_number("bulunanlar 3") == 0);
	CHECK(findsave_zone_number("Bulunanlar2") == 0 && findsave_zone_number("Simplex") == 0);

	printf("findsave: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
