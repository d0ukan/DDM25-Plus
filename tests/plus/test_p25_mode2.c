/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus P25 channels of CH_MODE 2 (CONFIG_P25_MODE2, docs/plus.md): records in the DM32-nxdn-p25 multi-mode
 * derivative's format read as Plus P25 channels, in the real codeplug decoder, flash journal and reboot path.
 *
 * Built twice by tools/test_classic.py, with CONFIG_P25_MODE2=1 and =0 (YSF, NXDN, dPMR and DMR any on in both). Each
 * build prints one line per record of a fixed set and of 4000 pseudo-random records ("rec ..."); test_classic.py
 * requires the two outputs to be the same for every record whose mode byte is not 2: FM, DMR, 0xA5-0xA8 and unknown
 * modes decode exactly as before. Mode 2: hidden without the switch (Classic, Plus until now); with it, the owner's
 * channels 18 and 19 (their record bytes from offset 0x10, docs/plus.md) read as P25, NAC $293, TG 1 / 612, unit ID 1,
 * TX allowed; the derivative's special values; saving the channel (the editor's path) writes Classic r13's form (a
 * DMR base record and an RFKV P25 tag). Since the Classic r13 base the switch also reads the P25 records Classic
 * r4-r7 and the r7-based Plus wrote (0xA5), which Classic r13 refuses; test_classic.py leaves both mode bytes out
 * of its comparison. */
#define main upstream_codeplug_main
#include "codeplugEditTest.c"
#undef main
#include "codeplug/codeplug_tag.h"

/* The owner's channel 18 ("159.64375") and 19 ("159.8825") from offset 0x10 to 0x30; the rest of the record is 0 */
static const uint8_t ch18[] = { 0x75, 0x43, 0x96, 0x15, 0x75, 0x43, 0x96, 0x15, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x93, 0x02, 0x93, 0x02, 0x00, 0x00, 0x80, 0x00, 0x00, 0x01, 0x00, 0x93,
				0x00, 0x00, 0x01, 0x00, 0x00 };
static const uint8_t ch19[] = { 0x50, 0x82, 0x98, 0x15, 0x50, 0x82, 0x98, 0x15, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x93, 0x02, 0x93, 0x02, 0x00, 0x00, 0x80, 0x00, 0x00, 0x01, 0x00, 0x93,
				0x00, 0x00, 0x64, 0x02, 0x00 };

static void record(uint8_t r[CODEPLUG_OPENFW_CHANNEL_SIZE], const char *name, const uint8_t *from10, size_t n)
{
	memset(r, 0, CODEPLUG_OPENFW_CHANNEL_SIZE);
	memset(r, 0xFF, 16);
	memcpy(r, name, strlen(name));
	memcpy(r + 0x10, from10, n);
}

/* One decode as a line both builds print */
static void print_decode(unsigned k, const uint8_t *r)
{
	channel_t c;
	memset(&c, 0x5A, sizeof c);
	bool ok = codeplug_openfw_decode(r, &c);
	printf("rec %u mode 0x%02X: ", k, r[0x18]);
	if (!ok) {
		puts("not a channel");
		return;
	}
	printf("%s %u %u>%u bw%u pw%u rxo%u tone %u/%u %u/%u cc%u ts%u c%u l%u id%u dmo%u vox%u skip%u beep%u eco%u roam%u "
	       "sfr%u/%u/%u tot%u alias%u\n", c.name, (unsigned)c.mode, (unsigned)c.rx_hz, (unsigned)c.tx_hz,
	       (unsigned)c.bw, (unsigned)c.power, (unsigned)c.rx_only, (unsigned)c.rx_tone.type, (unsigned)c.rx_tone.value,
	       (unsigned)c.tx_tone.type, (unsigned)c.tx_tone.value, (unsigned)c.color_code, (unsigned)c.timeslot,
	       (unsigned)c.contact, (unsigned)c.listen_list, (unsigned)c.dmr_id, (unsigned)c.dmo, (unsigned)c.vox,
	       (unsigned)c.scan_skip, (unsigned)c.no_beep, (unsigned)c.no_eco, (unsigned)c.roam, (unsigned)c.sfr_flag1,
	       (unsigned)c.sfr_a, (unsigned)c.sfr_b, (unsigned)c.tot_s, (unsigned)c.alias_tx);
}

static uint32_t rng = 0x2468ACE1u;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

/* Records of every kind, written by the real encoder, then perturbed: printed by both builds */
static void decode_set(void)
{
	static const uint8_t modes[] = { 0, 1, 2, 3, 4, 0xA5, 0xA6, 0xA7, 0xA8, 0x05, 0xFF };
	uint8_t r[CODEPLUG_OPENFW_CHANNEL_SIZE];
	unsigned k = 0;
	channel_t f = make("FM 145.5", 145500000, 145500000);
	f.rx_tone = (subtone_t){ TONE_CTCSS, 885 };
	f.tx_tone = (subtone_t){ TONE_DCS, 023 };
	f.bw = RADIO_BW_25K;
	channel_t d = make("DMR TG91", 438500000, 430900000);
	d.mode = RADIO_MODE_DMR;
	d.color_code = 3;
	d.timeslot = 2;
	d.contact = 4;
	d.listen_list = 2;
	d.dmr_id = 2860001;
	channel_t p = make("P25 659", 433500000, 433500000);
	p.mode = RADIO_MODE_P25;
	p.p25_nac = 0x293;
	p.p25_tg = 101;
	p.p25_id = 1234567;
	channel_t y = make("YSF", 439000000, 439000000);
	y.mode = RADIO_MODE_YSF;
	y.rx_only = true;
	y.p25_nac = 0xF7E;
	y.p25_tg = 12;
	channel_t n = make("NXDN", 446100000, 446100000);
	n.mode = RADIO_MODE_NXDN;
	n.rx_only = true;
	n.p25_tg = 5;
	channel_t m = make("dPMR", 446106250, 446106250);
	m.mode = RADIO_MODE_DPMR;
	m.rx_only = true;
	m.p25_tg = 3;
	const channel_t *kinds[] = { &f, &d, &p, &y, &n, &m };
	for (unsigned i = 0; i < sizeof kinds / sizeof kinds[0]; i++) {
		memset(r, 0xFF, sizeof r);
		codeplug_openfw_encode(kinds[i], r);
		print_decode(k++, r);
		for (unsigned j = 0; j < sizeof modes; j++) {	// the same bytes under every mode byte
			uint8_t s[CODEPLUG_OPENFW_CHANNEL_SIZE];
			memcpy(s, r, sizeof s);
			s[0x18] = modes[j];
			print_decode(k++, s);
		}
	}
	for (unsigned i = 0; i < 4000u; i++) {		// random bytes, a valid frequency, a mode byte of the set
		channel_t base = *kinds[i % 6u];
		memset(r, 0xFF, sizeof r);
		codeplug_openfw_encode(&base, r);
		for (unsigned b = 0x19; b < CODEPLUG_OPENFW_CHANNEL_SIZE; b++)
			if (rnd() % 3u == 0)
				r[b] = (uint8_t)rnd();
		r[0x18] = modes[rnd() % sizeof modes];
		print_decode(k++, r);
	}
}

/* A Classic r4-r7 / r7-based Plus P25 record (CH_MODE 0xA5): RX NAC $293 (CH_CC 0xD2, CH_RX_GROUP 0x93), talkgroup
 * 101 in the contact word, unit ID 1234567 in CH_DMR_ID (LFLAG1 bit 7), no tones (0xFFFF), from offset 0x10 */
static const uint8_t a5[] = { 0x00, 0x00, 0x35, 0x43, 0x00, 0x00, 0x35, 0x43, 0xA5, 0x00, 0x00, 0x00, 0x00, 0x00,
			      0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x80, 0x12, 0xD6, 0x87, 0x00, 0x93,
			      0xD2, 0x00, 0x65, 0x00, 0x00 };

int main(void)
{
	int prior = upstream_codeplug_main();
	uint8_t r[CODEPLUG_OPENFW_CHANNEL_SIZE];
	channel_t c;
	decode_set();
	record(r, "159.64375", ch18, sizeof ch18);
#if CONFIG_P25_MODE2
	/* The owner's channel 18: P25, NAC $293 (the tone words: a 12-bit number, $293 = 659), TG 1, unit ID 1 */
	CHECK(codeplug_openfw_decode(r, &c));
	CHECK(c.mode == RADIO_MODE_P25 && codeplug_mode_p25(c.mode) && !codeplug_mode_ysf(c.mode));
	CHECK(c.p25_nac == 0x293 && c.p25_tg == 1 && c.p25_id == 1 && !c.rx_only && codeplug_p25_valid(&c));
	CHECK(c.p25_squelch == 1 && c.p25_tx_nac_override == 0 && c.contact == 0 && c.listen_list == 0);
	CHECK(c.rx_hz == 159643750u && c.tx_hz == 159643750u && c.bw == RADIO_BW_12K5);
	CHECK(c.rx_tone.type == TONE_NONE && c.tx_tone.type == TONE_NONE && strcmp(c.name, "159.64375") == 0);
	CHECK(c.power == CODEPLUG_POWER_DEFAULT && !c.dmo && !c.roam && !c.vox && c.sfr_flag1 == 0);
	/* channel 19: TG 612 */
	record(r, "159.8825", ch19, sizeof ch19);
	CHECK(codeplug_openfw_decode(r, &c) && c.p25_nac == 0x293 && c.p25_tg == 612 && c.p25_id == 1 && !c.rx_only);
	CHECK(c.rx_hz == 159882500u);
	/* The derivative's special values (its codeplug.c decode_digital): RX NAC F7E / F7F / over FFF = every NAC, TX
	 * NAC F7E and up = its default 293. A TX NAC unlike the RX one is the channel's TX NAC (Classic r13's split NAC:
	 * p25_tx_nac_override = TX + 1), so none of these reads as RX only */
	uint8_t s[CODEPLUG_OPENFW_CHANNEL_SIZE];
	static const struct { uint16_t rx, tx; uint16_t nac; } nacs[] = {
		{ 0x293, 0x293, 0x293 }, { 0x125, 0x125, 0x125 }, { 0x000, 0x000, 0x000 },
		{ 0xF7D, 0xF7D, 0xF7D }, { 0xF7E, 0x293, 0xF7E }, { 0xF7F, 0x293, 0xF7E },
		{ 0xFFFF, 0xFFFF, 0xF7E }, { 0x1000, 0x293, 0xF7E }, { 0x293, 0xF7E, 0x293 },
		{ 0x293, 0xFFFF, 0x293 }, { 0x293, 0x125, 0x293 }, { 0x659, 0x293, 0x659 },
	};
	for (unsigned i = 0; i < sizeof nacs / sizeof nacs[0]; i++) {
		record(s, "nac", ch18, sizeof ch18);
		s[0x20] = (uint8_t)nacs[i].rx; s[0x21] = (uint8_t)(nacs[i].rx >> 8);
		s[0x22] = (uint8_t)nacs[i].tx; s[0x23] = (uint8_t)(nacs[i].tx >> 8);
		bool ok = codeplug_openfw_decode(s, &c);
		uint16_t tx = nacs[i].tx >= 0xF7Eu ? 0x293u : nacs[i].tx;
		uint16_t want = tx != nacs[i].nac ? (uint16_t)(tx + 1u) : 0;
		CHECK(ok && c.p25_nac == nacs[i].nac && !c.rx_only && c.p25_tx_nac_override == want);
		CHECK(ok && codeplug_p25_tx_nac(&c) == (want ? tx : nacs[i].nac == 0xF7Eu ? 0x293u : nacs[i].nac));
		CHECK(codeplug_p25_valid(&c));
		if (!ok || c.p25_nac != nacs[i].nac || c.rx_only || c.p25_tx_nac_override != want)
			printf("  NAC case %u: rx %04X tx %04X -> %03X rx_only %u tx+1 %03X\n", i, nacs[i].rx, nacs[i].tx,
			       c.p25_nac, c.rx_only, c.p25_tx_nac_override);
	}
	/* Hear every talkgroup (CH_FLAG2 bit 6): Classic r13's Normal squelch (every group on the RX NAC), the
	 * talkgroup kept for TX */
	record(s, "all", ch18, sizeof ch18);
	s[0x31] = 0x40;
	CHECK(codeplug_openfw_decode(s, &c) && c.p25_tg == 1 && c.p25_squelch == 0 && !c.rx_only && c.timeslot == 1);
	/* No unit ID (the derivative would send the radio's DMR ID): RX only, the talkgroup kept */
	record(s, "noid", ch18, sizeof ch18);
	s[0x26] = 0;
	CHECK(codeplug_openfw_decode(s, &c) && c.p25_id == 0 && c.p25_tg == 1 && c.rx_only);
	s[0x26] = 0x80; s[0x27] = 0xFF; s[0x28] = 0xFF; s[0x29] = 0xFF;	/* 16777215: beyond P25's 0xFFFFFE */
	CHECK(codeplug_openfw_decode(s, &c) && c.p25_id == 0 && c.rx_only);
	s[0x27] = 0xFF; s[0x28] = 0xFF; s[0x29] = 0xFE;			/* 16777214: Classic r13's top unit ID */
	CHECK(codeplug_openfw_decode(s, &c) && c.p25_id == 0xFFFFFEu && !c.rx_only);
	/* No talkgroup: RX only (Any) */
	record(s, "notg", ch18, sizeof ch18);
	s[0x2E] = s[0x2F] = 0;
	CHECK(codeplug_openfw_decode(s, &c) && c.p25_tg == 0 && c.rx_only);
	/* The record's RX only flag, its 25 kHz bit (P25 is 12.5 kHz), scan skip and power are kept / normalized */
	record(s, "flags", ch18, sizeof ch18);
	s[0x33] = 0x04 | 0x02 | 0x20;
	s[0x19] = 10;
	CHECK(codeplug_openfw_decode(s, &c) && c.rx_only && c.bw == RADIO_BW_12K5 && c.scan_skip && c.power == 8);
	/* A frequency that isn't BCD: not a channel, as for every mode */
	record(s, "bad", ch18, sizeof ch18);
	s[0x10] = 0xAB;
	CHECK(!codeplug_openfw_decode(s, &c));
	/* The derivative's NXDN (3) and P25 trunking (4) records stay hidden */
	record(s, "nxdn", ch18, sizeof ch18);
	s[0x18] = 3;
	CHECK(!codeplug_openfw_decode(s, &c));
	s[0x18] = 4;
	CHECK(!codeplug_openfw_decode(s, &c));
	/* A Classic r7 / r7-based Plus P25 record (0xA5): read as it read there (no tag; TX NAC Same as RX) */
	record(s, "r7 p25", a5, sizeof a5);
	CHECK(codeplug_openfw_decode(s, &c) && c.mode == RADIO_MODE_P25 && c.kind == CODEPLUG_KIND_NONE);
	CHECK(c.p25_nac == 0x293 && c.p25_tg == 101 && c.p25_id == 1234567u && c.p25_squelch == 1 && !c.rx_only);
	CHECK(c.p25_tx_nac_override == 0 && codeplug_p25_tx_nac(&c) == 0x293 && c.contact == 0 && codeplug_p25_valid(&c));
	CHECK(c.rx_hz == 433500000u && c.rx_tone.type == TONE_NONE && c.tx_tone.type == TONE_NONE);
	s[0x2C] = 0xC2;				/* CH_CC's high nibble is not 0xD: not one */
	CHECK(!codeplug_openfw_decode(s, &c));

	/* In the codeplug: the record as stored (channel 18 of a codeplug this firmware made), read after a reboot, then
	 * saved the editor's way (codeplug_set_channel with the channel as read): Classic r13's P25 form (a DMR base
	 * record, receive only, and an RFKV tag of kind P25 with the parameter block) */
	fresh();
	channel_t fm = make("159.64375", 159643750, 159643750);
	CHECK(codeplug_set_channel(18, &fm));
	uint8_t *at = &sim_flash_data[EE + 0x3790 + 56u * 17u];
	CHECK(at[0x18] == 0 && at[0x10] == 0x75 && at[0] == '1');	/* bank 0, slot 17: where the record is */
	record(r, "159.64375", ch18, sizeof ch18);
	memcpy(at, r, sizeof r);
	CHECK(codeplug_init() == CODEPLUG_OPENFW && !codeplug_foreign());	/* mode 2 is not foreign here */
	CHECK(codeplug_channel(18, &c) && c.mode == RADIO_MODE_P25 && c.p25_nac == 0x293 && c.p25_tg == 1 && c.p25_id == 1);
	CHECK(memcmp(at, r, sizeof r) == 0);		/* reading wrote nothing */
	c.p25_tx_nac_override = 0x659 + 1;		/* and a TX NAC of its own, as the editor sets it */
	CHECK(codeplug_set_channel(18, &c));
	CHECK(at[0x18] == 1 && (at[0x33] & 0x04));	/* the DMR base record, receive only */
	codeplug_tag_t tag;
	CHECK(codeplug_tag_read(18, &tag) && tag.kind == CODEPLUG_KIND_P25);
	channel_t back;
	CHECK(codeplug_init() == CODEPLUG_OPENFW && codeplug_channel(18, &back));
	CHECK(back.mode == RADIO_MODE_P25 && back.kind == CODEPLUG_KIND_P25 && back.p25_nac == 0x293 && back.p25_tg == 1 &&
	      back.p25_id == 1 && !back.rx_only && back.rx_hz == 159643750u && strcmp(back.name, "159.64375") == 0);
	CHECK(back.p25_tx_nac_override == 0x659 + 1 && codeplug_p25_tx_nac(&back) == 0x659);
	/* The same for an r7 0xA5 record */
	record(r, "r7 p25", a5, sizeof a5);
	memcpy(at, r, sizeof r);
	CHECK(codeplug_tag_remove(18));
	CHECK(codeplug_init() == CODEPLUG_OPENFW && !codeplug_foreign() && codeplug_channel(18, &c));
	CHECK(c.mode == RADIO_MODE_P25 && c.p25_nac == 0x293 && c.p25_tg == 101 && c.p25_id == 1234567u);
	CHECK(memcmp(at, r, sizeof r) == 0);
	CHECK(codeplug_set_channel(18, &c) && at[0x18] == 1);
	CHECK(codeplug_init() == CODEPLUG_OPENFW && codeplug_channel(18, &back));
	CHECK(back.mode == RADIO_MODE_P25 && back.kind == CODEPLUG_KIND_P25 && back.p25_nac == 0x293 && back.p25_tg == 101 &&
	      back.p25_id == 1234567u && back.p25_squelch == 1 && back.p25_tx_nac_override == 0 && !back.rx_only);
	CHECK(sim_flash_violations() == 0);
	puts("Plus CH_MODE 2 and 0xA5 P25 channels: the owner's records, special values, RX only cases, read without a "
	     "write, saved in Classic r13's RFKV form");
#else
	/* Without the switch a mode 2 or 0xA5 record is not a channel, and a codeplug holding one is another firmware's
	 * (Classic r13: Start new) */
	CHECK(!codeplug_openfw_decode(r, &c));
	record(r, "159.8825", ch19, sizeof ch19);
	CHECK(!codeplug_openfw_decode(r, &c));
	record(r, "r7 p25", a5, sizeof a5);
	CHECK(!codeplug_openfw_decode(r, &c));
	fresh();
	channel_t fm = make("159.64375", 159643750, 159643750);
	CHECK(codeplug_set_channel(18, &fm));
	memcpy(&sim_flash_data[EE + 0x3790 + 56u * 17u], r, sizeof r);
	(void)codeplug_init();
	CHECK(codeplug_foreign());	/* shown as no codeplug until Start new */
	puts("CH_MODE 2 and 0xA5 hidden without CONFIG_P25_MODE2 (a codeplug holding one: Start new, as Classic r13)");
#endif
	return prior || failures ? 1 : 0;
}
