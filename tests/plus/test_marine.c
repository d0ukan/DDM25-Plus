/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus Tools > Marine VHF table (integration/radiofoundry/marine): Appendix 18's structure (a duplex channel's
 * coast station 4.6 MHz above its ship station, a simplex one on one frequency, the intership ones ship only, the 25
 * kHz raster of 156.025 .. 157.425 / 160.625 .. 162.025 MHz), the known channels (16, 70, 06, 13, AIS), what listening
 * tunes (the coast frequency; 70 and AIS not at all) and marine_hz, the radio core's RX only rule. */
#include <stdio.h>
#include <string.h>
#include "radio/marine.h"

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static const marine_ch_t *ch(const char *name)
{
	int i = marine_find(name);
	CHECK(i >= 0);
	return i >= 0 ? &marine_table[i] : &marine_table[0];
}

int main(void)
{
	/* 01..28 and 60..88 (57 channels) and AIS 1 / 2 */
	CHECK(marine_count == 59);
	unsigned duplex = 0, simplex = 0, ship_only = 0;
	for (uint8_t i = 0; i < marine_count; i++) {
		const marine_ch_t *c = &marine_table[i];
		uint32_t s = marine_ship_hz(c), k = marine_coast_hz(c), hz;
		CHECK(s % 25000u == 0 || c->use == MAR_AIS);
		CHECK(s >= 156025000u && s <= 162025000u);
		CHECK(marine_find(c->name) == i);
		CHECK(marine_hz(s) && (k == 0 || marine_hz(k)));
		if (k == 0) {
			ship_only++;
			CHECK(c->use == MAR_INTERSHIP || c->use == MAR_SAR);
		} else if (k == s) {
			simplex++;
		} else {
			duplex++;
			CHECK(k - s == 4600000u && c->use == MAR_DUPLEX);
		}
		/* The channel number's frequency: 01..28 at 156.050 + 50 kHz steps (ship), 60..88 at 156.025 + 50 kHz */
		if (c->use != MAR_AIS) {
			int n = (c->name[0] - '0') * 10 + (c->name[1] - '0');
			uint32_t want = n < 60 ? 156000000u + 50000u * (uint32_t)n : 156025000u + 50000u * (uint32_t)(n - 60);
			if (n > 28 && n < 60)
				CHECK(0);
			CHECK(s == want);
		}
		bool data = c->use == MAR_DSC || c->use == MAR_AIS;
		CHECK(marine_listen_hz(c, &hz) == !data);
		if (!data)
			CHECK(hz == (k ? k : s));
	}
	CHECK(duplex == 33 && ship_only == 4 && simplex == 22);	/* simplex: 09-17, 67-71, 73-76, 87, 88, AIS 1 / 2 */

	/* Known ones */
	uint32_t hz = 0;
	CHECK(ch("16")->use == MAR_DISTRESS && marine_ship_hz(ch("16")) == 156800000u && marine_coast_hz(ch("16")) == 156800000u);
	CHECK(marine_listen_hz(ch("16"), &hz) && hz == 156800000u);
	CHECK(ch("70")->use == MAR_DSC && marine_ship_hz(ch("70")) == 156525000u && !marine_listen_hz(ch("70"), &hz));
	CHECK(ch("06")->use == MAR_SAR && marine_coast_hz(ch("06")) == 0 && marine_listen_hz(ch("06"), &hz) && hz == 156300000u);
	CHECK(ch("13")->use == MAR_BRIDGE && marine_ship_hz(ch("13")) == 156650000u);
	CHECK(marine_ship_hz(ch("60")) == 156025000u && marine_coast_hz(ch("60")) == 160625000u);
	CHECK(marine_ship_hz(ch("28")) == 157400000u && marine_coast_hz(ch("28")) == 162000000u);
	CHECK(marine_listen_hz(ch("22"), &hz) && hz == 161700000u);
	CHECK(marine_ship_hz(ch("87B")) == 161975000u && ch("87b") == ch("87B") && !marine_listen_hz(ch("87B"), &hz));
	CHECK(marine_ship_hz(ch("88B")) == 162025000u && ch("88B")->use == MAR_AIS);
	CHECK(ch("87")->use == MAR_SIMPLEX && marine_ship_hz(ch("87")) == 157375000u);
	CHECK(marine_find("29") < 0 && marine_find("") < 0 && marine_find("166") < 0);

	/* marine_hz: within 1.25 kHz of a table frequency; not between channels, not outside the band */
	CHECK(marine_hz(156800000u) && marine_hz(156801249u) && marine_hz(156798751u));
	CHECK(!marine_hz(156801250u) && !marine_hz(156812500u));	/* a 12.5 kHz interstitial is not a table one */
	CHECK(!marine_hz(156000000u) && !marine_hz(155975000u) && !marine_hz(145500000u) && !marine_hz(0));
	CHECK(marine_hz(160625000u) && marine_hz(162025000u) && !marine_hz(162050000u) && !marine_hz(433500000u));
	CHECK(!marine_hz(160900000u));	/* channel 2006: not listed */
	CHECK(!marine_hz(UINT32_MAX) && !marine_hz(UINT32_MAX - 1000u));

	printf("marine VHF: %u channels (%u duplex, %u simplex, %u ship only): %d failures\n", marine_count, duplex,
	       simplex, ship_only, failures);
	return failures != 0;
}
