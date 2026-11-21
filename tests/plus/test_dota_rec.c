/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus DOTA Transfer: the records, format version 1 (core/radio/dota_rec.c, docs/dota.md 9). Every mode and
 * field through encoder and decoder; the identities the format leaves out (per-channel DMR ID, P25 unit ID) come back
 * as 0; contacts, RX group lists and zone parts; every record fits a FM frame's 128 B; truncated and random records are
 * refused whole and never read past their end. Built by tools/test_dota.py. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "radio/dota_rec.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define POWERS 3	/* the DM-32's levels */

static channel_t base(const char *name, uint32_t rx, uint32_t tx, uint8_t mode)
{
	channel_t c;
	memset(&c, 0, sizeof c);
	strncpy(c.name, name, CODEPLUG_NAME_LEN);
	c.rx_hz = rx;
	c.tx_hz = tx;
	c.mode = mode;
	c.bw = RADIO_BW_12K5;
	c.power = CODEPLUG_POWER_DEFAULT;
	c.color_code = 1;
	c.timeslot = 1;
	return c;
}

static bool same(const channel_t *a, const channel_t *b)
{
	return strcmp(a->name, b->name) == 0 && a->rx_hz == b->rx_hz && a->tx_hz == b->tx_hz && a->mode == b->mode &&
	       a->bw == b->bw && a->rx_tone.type == b->rx_tone.type && a->rx_tone.value == b->rx_tone.value &&
	       a->tx_tone.type == b->tx_tone.type && a->tx_tone.value == b->tx_tone.value && a->power == b->power &&
	       a->rx_only == b->rx_only && a->color_code == b->color_code && a->timeslot == b->timeslot &&
	       a->p25_tg == b->p25_tg && a->p25_nac == b->p25_nac && a->p25_squelch == b->p25_squelch &&
	       a->p25_tx_nac_override == b->p25_tx_nac_override && a->dmo == b->dmo && a->tot_s == b->tot_s &&
	       a->alias_tx == b->alias_tx && a->scan_skip == b->scan_skip && a->vox == b->vox && a->no_beep == b->no_beep &&
	       a->no_eco == b->no_eco && a->sfr_flag1 == b->sfr_flag1 && a->sfr_a == b->sfr_a && a->sfr_b == b->sfr_b;
}

static int longest;
static void round_trip(const dota_ch_t *in, const channel_t *want)
{
	uint8_t buf[DOTA_CH_MAX + 8];
	memset(buf, 0xA5, sizeof buf);
	size_t n = dota_rec_put_channel(in, POWERS, buf, DOTA_CH_MAX);
	CHECK(n > 0 && n <= DOTA_CH_MAX && buf[n] == 0xA5);
	if ((int)n > longest)
		longest = (int)n;
	dota_ch_t out;
	CHECK(dota_rec_get_channel(buf, n, POWERS, &out) == n);
	CHECK(same(&out.ch, want));
	CHECK(out.ct_type == in->ct_type && out.ct_id == (in->ct_type ? in->ct_id : 0u) && out.list == in->list);
	CHECK(out.ch.dmr_id == 0 && out.ch.p25_id == 0);	/* identities never travel */
	/* every shorter piece is refused, and a decoder never reads past what it is given (ASan: a heap copy) */
	for (size_t k = 0; k < n; k++) {
		uint8_t *cut = malloc(k ? k : 1);
		memcpy(cut, buf, k);
		CHECK(dota_rec_get_channel(cut, k, POWERS, &out) == 0);
		free(cut);
	}
	/* two in a row: the second starts where the first ended */
	uint8_t two[2 * DOTA_CH_MAX];
	size_t a = dota_rec_put_channel(in, POWERS, two, sizeof two);
	size_t b = dota_rec_put_channel(in, POWERS, two + a, sizeof two - a);
	CHECK(dota_rec_get_channel(two, a + b, POWERS, &out) == a && dota_rec_get_channel(two + a, b, POWERS, &out) == b);
	CHECK(dota_rec_put_channel(in, POWERS, buf, n - 1) == 0);	/* no room: nothing */
}

static void test_channels(void)
{
	dota_ch_t c;
	channel_t w;
	/* FM simplex, nothing else */
	memset(&c, 0, sizeof c);
	c.ch = base("2m S20", 145500000, 145500000, RADIO_MODE_FM);
	round_trip(&c, &c.ch);
	/* FM repeater: offset, both tones, 25 kHz, power classes, every flag, the extras */
	c.ch = base("TA2 Ankara R0", 145600000, 145000000, RADIO_MODE_FM);
	c.ch.bw = RADIO_BW_25K;
	c.ch.rx_tone = (subtone_t){ TONE_CTCSS, 885 };
	c.ch.tx_tone = (subtone_t){ TONE_DCS_INV, 0754 };
	c.ch.power = 0;
	c.ch.rx_only = c.ch.scan_skip = c.ch.vox = c.ch.no_beep = c.ch.no_eco = c.ch.dmo = true;
	c.ch.tot_s = 180;
	c.ch.alias_tx = 0x0A;
	round_trip(&c, &c.ch);
	c.ch.rx_tone = (subtone_t){ TONE_DCS, 023 };
	c.ch.tx_tone = (subtone_t){ TONE_CTCSS, 2541 };
	c.ch.power = 2;	/* high */
	round_trip(&c, &c.ch);
	c.ch.power = 1;	/* mid */
	round_trip(&c, &c.ch);
	/* cross band (no offset fits 24 bits of 25 Hz): the TX frequency itself; and an odd offset */
	c.ch = base("Cross", 145800000, 437800000, RADIO_MODE_FM);
	round_trip(&c, &c.ch);
	c.ch = base("Odd", 446006250, 446006260, RADIO_MODE_FM);
	round_trip(&c, &c.ch);
	c.ch = base("Neg", 439000000, 431400000, RADIO_MODE_FM);
	round_trip(&c, &c.ch);
	/* DMR: every contact type, CC any, slot either, a list, the "any" list, SFR, no contact */
	static const uint8_t types[] = { DOTA_CT_NONE, DOTA_CT_GROUP, DOTA_CT_PRIVATE, DOTA_CT_ALL };
	for (unsigned t = 0; t < sizeof types; t++) {
		memset(&c, 0, sizeof c);
		c.ch = base("DMR rpt 16 chars", 438500000, 430900000, RADIO_MODE_DMR);
		c.ch.color_code = (uint8_t)(t * 5u);
		c.ch.timeslot = (uint8_t)(1u + t % 2u);
		c.ct_type = types[t];
		c.ct_id = t == 3 ? 0xFFFFFFu : 2860000u + t;
		c.list = t == 0 ? DOTA_LIST_NONE : t == 1 ? DOTA_LIST_ANY : (uint8_t)(t + 1u);
		round_trip(&c, &c.ch);
	}
#if CONFIG_DMR_ANY
	c.ch.color_code = CODEPLUG_CC_ANY;
	c.ch.timeslot = CODEPLUG_TS_ANY;
	c.ch.sfr_flag1 = 2;
	c.ch.sfr_a = 0x29;
	c.ch.sfr_b = 0x87;
	c.ch.tot_s = 3840;	/* the largest the format holds: 255 x 15 s (the codeplug's own unit) */
	w = c.ch;
	w.tot_s = 3825;
	round_trip(&c, &w);
#endif
	/* A DMR channel's stale FM tones are not sent (a DMR channel has none) */
	c.ch.rx_tone = (subtone_t){ TONE_CTCSS, 885 };
	w = c.ch;
	w.rx_tone = (subtone_t){ TONE_NONE, 0 };
	w.tot_s = c.ch.tot_s ? (uint16_t)((c.ch.tot_s + 14u) / 15u > 255u ? 3825u : (c.ch.tot_s + 14u) / 15u * 15u) : 0;
	round_trip(&c, &w);
	/* identities left out: the per-channel DMR ID and the P25 unit ID */
	c.ch.dmr_id = 3101234;
	c.ch.rx_tone = (subtone_t){ TONE_NONE, 0 };
	round_trip(&c, &w);
	/* P25: TX NAC same as RX and its own, Normal / Selective, the unit ID dropped */
	memset(&c, 0, sizeof c);
	c.ch = base("P25 659", 433500000, 433500000, RADIO_MODE_P25);
	c.ch.p25_nac = 0x659;
	c.ch.p25_tg = 101;
	c.ch.p25_squelch = 1;
	c.ch.p25_id = 77285;
	w = c.ch;
	w.p25_id = 0;
	round_trip(&c, &w);
	c.ch.p25_nac = 0xF7E;
	c.ch.p25_tx_nac_override = 0x293 + 1;
	c.ch.p25_squelch = 0;
	w = c.ch;
	w.p25_id = 0;
	round_trip(&c, &w);
#if CONFIG_YSF
	memset(&c, 0, sizeof c);
	c.ch = base("YSF DG 42", 144625000, 144625000, RADIO_MODE_YSF);
	c.ch.p25_tg = 42;
	c.ch.rx_only = true;
	round_trip(&c, &c.ch);
#endif
#if CONFIG_NXDN
	c.ch = base("NXDN RAN 9", 446106250, 446106250, RADIO_MODE_NXDN);
	c.ch.p25_tg = 9;
	c.ch.rx_only = true;
	round_trip(&c, &c.ch);
#endif
#if CONFIG_DPMR
	c.ch = base("dPMR 64", 446106250, 446106250, RADIO_MODE_DPMR);
	c.ch.p25_tg = 64;
	c.ch.rx_only = true;
	round_trip(&c, &c.ch);
#endif
	/* A name with bytes outside printable ASCII is sent with '?' */
	memset(&c, 0, sizeof c);
	c.ch = base("\xc7" "anakkale\x01", 145500000, 145500000, RADIO_MODE_FM);
	w = c.ch;
	strcpy(w.name, "?anakkale?");
	round_trip(&c, &w);
	c.ch = base("", 145500000, 145500000, RADIO_MODE_FM);
	round_trip(&c, &c.ch);
	CHECK(longest <= (int)DOTA_CH_MAX);
	printf("  channels: longest record %d B\n", longest);
	/* Refusals: an unknown mode, zero frequencies, a bad tone, bad DMR fields, a zero contact ID */
	uint8_t buf[DOTA_CH_MAX];
	dota_ch_t out;
	c.ch = base("x", 145500000, 145500000, RADIO_MODE_FM);
	size_t n = dota_rec_put_channel(&c, POWERS, buf, sizeof buf);
	buf[0] = (uint8_t)((buf[0] & ~7u) | 7u);
	CHECK(dota_rec_get_channel(buf, n, POWERS, &out) == 0);
	n = dota_rec_put_channel(&c, POWERS, buf, sizeof buf);
	memset(buf + 2, 0, 4);
	CHECK(dota_rec_get_channel(buf, n, POWERS, &out) == 0);
	c.ch.rx_tone = (subtone_t){ TONE_CTCSS, 885 };
	n = dota_rec_put_channel(&c, POWERS, buf, sizeof buf);
	buf[6] = 0x40;	/* CTCSS 0.0 Hz */
	buf[7] = 0;
	CHECK(dota_rec_get_channel(buf, n, POWERS, &out) == 0);
	memset(&c, 0, sizeof c);
	c.ch = base("d", 438500000, 438500000, RADIO_MODE_DMR);
	c.ct_type = DOTA_CT_GROUP;
	c.ct_id = 9;
	n = dota_rec_put_channel(&c, POWERS, buf, sizeof buf);
	buf[7] = 0x05;	/* contact type out of range */
	CHECK(dota_rec_get_channel(buf, n, POWERS, &out) == 0);
	n = dota_rec_put_channel(&c, POWERS, buf, sizeof buf);
	buf[8] = buf[9] = buf[10] = 0;	/* contact ID 0 */
	CHECK(dota_rec_get_channel(buf, n, POWERS, &out) == 0);
	n = dota_rec_put_channel(&c, POWERS, buf, sizeof buf);
	buf[6] = 17;	/* colour code 17 */
	CHECK(dota_rec_get_channel(buf, n, POWERS, &out) == 0);
}

static void test_contacts_lists_zones(void)
{
	uint8_t buf[256];
	contact_t a = { "TG 286 Turkiye", 286, CONTACT_GROUP, 0 }, b;
	size_t n = dota_rec_put_contact(&a, buf, sizeof buf);
	CHECK(n > 0 && n <= DOTA_CT_MAX && dota_rec_get_contact(buf, n, &b) == n && memcmp(&a, &b, sizeof a) == 0);
	contact_t p = { "TA2DOU Dogan", 2862001, CONTACT_PRIVATE, 2 };
	n = dota_rec_put_contact(&p, buf, sizeof buf);
	CHECK(dota_rec_get_contact(buf, n, &b) == n && memcmp(&p, &b, sizeof p) == 0);
	for (size_t k = 0; k < n; k++)
		CHECK(dota_rec_get_contact(buf, k, &b) == 0);
	buf[3] = 0x03;	/* type 3 */
	CHECK(dota_rec_get_contact(buf, n, &b) == 0);
	/* RX group lists: up to 32 members, talkgroups and private calls */
	dota_list_t l, m;
	memset(&l, 0, sizeof l);
	strcpy(l.name, "Turkiye all TGs!");
	l.count = CODEPLUG_LIST_LEN;
	for (unsigned i = 0; i < l.count; i++)
		l.id[i] = 2860 + i * 1000u;
	l.private_bits = 0x80000001u;
	n = dota_rec_put_list(&l, buf, sizeof buf);
	CHECK(n == DOTA_LIST_MAX && n <= 128 && dota_rec_get_list(buf, n, &m) == n && memcmp(&l, &m, sizeof l) == 0);
	for (size_t k = 0; k < n; k++)
		CHECK(dota_rec_get_list(buf, k, &m) == 0);
	l.count = 2;
	l.private_bits = 4;	/* a private bit past the members */
	n = dota_rec_put_list(&l, buf, sizeof buf);
	CHECK(dota_rec_get_list(buf, n, &m) == 0);
	/* Zone parts: a named first part with runs, a later part, refusals */
	dota_zone_t z, y;
	memset(&z, 0, sizeof z);
	z.zone = 3;
	z.total = 80;
	strcpy(z.name, "Ankara + Izmir");
	z.runs = DOTA_ZONE_RUNS;
	for (unsigned i = 0; i < z.runs; i++) {
		z.start[i] = (uint16_t)(i * 30u);
		z.len[i] = 2;
	}
	n = dota_rec_put_zone(&z, buf, sizeof buf);
	CHECK(n == DOTA_ZONE_MAX - 2 && n <= 128 && dota_rec_get_zone(buf, n, &y) == n && memcmp(&z, &y, sizeof z) == 0);
	for (size_t k = 0; k < n; k++)
		CHECK(dota_rec_get_zone(buf, k, &y) == 0);
	z.part = 1;
	z.offset = 64;
	z.runs = 8;
	memset(z.start + 8, 0, sizeof z.start - 8 * sizeof z.start[0]);	/* runs past the count don't travel */
	memset(z.len + 8, 0, sizeof z.len - 8);
	memset(z.name, 0, sizeof z.name);
	n = dota_rec_put_zone(&z, buf, sizeof buf);
	CHECK(dota_rec_get_zone(buf, n, &y) == n && memcmp(&z, &y, sizeof z) == 0);
	z.runs = 9;	/* 64 + 18 members > 80 */
	n = dota_rec_put_zone(&z, buf, sizeof buf);
	CHECK(dota_rec_get_zone(buf, n, &y) == 0);
	z.runs = 1;
	z.start[0] = 1023;
	z.len[0] = 2;	/* past the 1024th channel */
	n = dota_rec_put_zone(&z, buf, sizeof buf);
	CHECK(dota_rec_get_zone(buf, n, &y) == 0);
	/* Power classes on the DM-32's three levels and on a nine-level radio */
	CHECK(dota_power_class(CODEPLUG_POWER_DEFAULT, 3) == 0 && dota_power_class(0, 3) == 1 &&
	      dota_power_class(1, 3) == 2 && dota_power_class(2, 3) == 3 && dota_power_class(8, 9) == 3);
	CHECK(dota_power_level(0, 3) == CODEPLUG_POWER_DEFAULT && dota_power_level(1, 3) == 0 &&
	      dota_power_level(2, 3) == 1 && dota_power_level(3, 3) == 2 && dota_power_level(2, 9) == 4);
}

static void test_fuzz(void)
{
	uint32_t r = 12345;
	uint8_t buf[200];
	dota_ch_t c;
	contact_t ct;
	dota_list_t l;
	dota_zone_t z;
	int taken = 0;
	for (int k = 0; k < 200000; k++) {
		size_t n = 1 + (r >> 8) % sizeof buf;
		for (size_t i = 0; i < n; i++) {
			r = r * 1103515245u + 12345u;
			buf[i] = (uint8_t)(r >> 16);
		}
		uint8_t *h = malloc(n);
		memcpy(h, buf, n);
		size_t a = dota_rec_get_channel(h, n, POWERS, &c);
		CHECK(a <= n);
		taken += a != 0;
		CHECK(dota_rec_get_contact(h, n, &ct) <= n);
		CHECK(dota_rec_get_list(h, n, &l) <= n);
		CHECK(dota_rec_get_zone(h, n, &z) <= n);
		free(h);
	}
	printf("  fuzz: %d random channel records taken of 200000 (each checked in range)\n", taken);
}

int main(void)
{
	test_channels();
	test_contacts_lists_zones();
	test_fuzz();
	printf("dota records: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
