/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus separate RX and TX NAC (docs/plus.md "Separate RX NAC and TX NAC") on Classic r13's channel store: the
 * TX NAC is upstream's (channel_t.p25_tx_nac_override, 0 = "Same as RX", else TX NAC + 1; the RFKV P25 parameter
 * block keeps its explicit-versus-inherited presence). Through the real codeplug encoder / decoder, RFKV tags, flash
 * journal and reboot path, with Plus's modes built in (YSF, NXDN, dPMR, CH_MODE 2 / 0xA5 read compatibility).
 * Checked: Same as RX, a TX NAC unlike RX, one equal to RX kept explicit, $000, RX Any inherited ($293) and with a TX
 * NAC of its own, the TX rule (never Any / $F7F), a YSF channel never carrying one, Plus's r7 TX NAC tag (0x5000 |
 * NAC in an 0xA5 record's TX tone word, never written on the owner's radio) read as Same as RX. */
#define main upstream_codeplug_main
#include "codeplugEditTest.c"
#undef main
#include "codeplug/codeplug_tag.h"

static uint16_t word_at(const uint8_t *r, unsigned at) { return (uint16_t)(r[at] | (r[at + 1] << 8)); }

static channel_t p25(uint16_t nac, uint16_t tg, uint32_t id)
{
	channel_t p = make("P25", 433500000, 433500000);
	p.mode = RADIO_MODE_P25;
	p.p25_nac = nac;
	p.p25_tg = tg;
	p.p25_id = id;
	p.dmr_id = id;
	p.p25_squelch = 1;
	return p;
}

/* Saved as channel n, read back after a reboot */
static bool round_trip(uint16_t n, const channel_t *in, channel_t *out)
{
	return codeplug_set_channel(n, in) && codeplug_init() == CODEPLUG_OPENFW && codeplug_channel(n, out);
}

int main(void)
{
	int prior = upstream_codeplug_main();
	channel_t c;
	fresh();
	uint8_t *at = &sim_flash_data[EE + 0x3790 + 56u * 17u];	/* channel 18's record */

	/* Same as RX: the TX NAC is the RX NAC */
	channel_t a = p25(0x293, 101, 1234567);
	CHECK(codeplug_p25_tx_nac(&a) == 0x293 && codeplug_p25_valid(&a));
	CHECK(round_trip(18, &a, &c) && c.mode == RADIO_MODE_P25 && c.kind == CODEPLUG_KIND_P25);
	CHECK(c.p25_nac == 0x293 && c.p25_tx_nac_override == 0 && c.p25_tg == 101 && c.p25_id == 1234567u && !c.rx_only);
	CHECK(at[0x18] == 1 && (at[0x33] & 0x04));	/* Classic r13: a DMR base record, receive only, and the tag */

	/* (a) a repeater: RX $293, TX $659 */
	channel_t t = a;
	t.p25_tx_nac_override = 0x659 + 1;
	CHECK(codeplug_p25_tx_nac(&t) == 0x659 && codeplug_p25_valid(&t));
	CHECK(round_trip(18, &t, &c) && c.p25_nac == 0x293 && c.p25_tx_nac_override == 0x659 + 1 && !c.rx_only);
	/* back to Same as RX */
	c.p25_tx_nac_override = 0;
	channel_t d;
	CHECK(round_trip(18, &c, &d) && d.p25_tx_nac_override == 0 && codeplug_p25_tx_nac(&d) == 0x293);
	/* A TX NAC equal to RX stays explicit (Classic r13 keeps the presence), $000 and $FFF are NACs like others */
	static const uint16_t txs[] = { 0x293, 0x000, 0xFFF, 0xF80, 0x125 };
	for (unsigned i = 0; i < sizeof txs / sizeof txs[0]; i++) {
		t.p25_tx_nac_override = (uint16_t)(txs[i] + 1u);
		CHECK(round_trip(18, &t, &c) && c.p25_tx_nac_override == txs[i] + 1u && codeplug_p25_tx_nac(&c) == txs[i]);
		CHECK(codeplug_p25_valid(&c));
	}

	/* (b) RX Any: inherited TX is Classic r13's $293; a TX NAC of its own is that one */
	channel_t any = p25(0xF7E, 101, 1234567);
	CHECK(codeplug_p25_tx_nac(&any) == 0x293 && codeplug_p25_valid(&any));
	CHECK(round_trip(19, &any, &c) && c.p25_nac == 0xF7E && c.p25_tx_nac_override == 0 && !c.rx_only);
	any.p25_tx_nac_override = 0x659 + 1;
	CHECK(round_trip(19, &any, &c) && c.p25_nac == 0xF7E && codeplug_p25_tx_nac(&c) == 0x659 && !c.rx_only);
	/* The TX rule: never TX NAC Any / $F7F, a talkgroup and an ID */
	any.p25_tx_nac_override = 0xF7E + 1;
	CHECK(!codeplug_p25_valid(&any) && !codeplug_set_channel(19, &any));
	any.p25_tx_nac_override = 0xF7F + 1;
	CHECK(!codeplug_p25_valid(&any));
	any.rx_only = true;		/* Classic r13 refuses an explicit Any / $F7F even when it only listens */
	CHECK(!codeplug_p25_valid(&any));
	any = p25(0x293, 0, 1234567);
	CHECK(!codeplug_p25_valid(&any));
	any.rx_only = true;
	CHECK(codeplug_p25_valid(&any));

	/* A YSF channel (codeplug_mode_p25 true for it too) never has a TX NAC: its own record (0xA6), no tag */
	channel_t y = make("YSF", 439000000, 439000000);
	y.mode = RADIO_MODE_YSF;
	y.rx_only = true;
	y.p25_nac = 0xF7E;
	y.p25_tg = 12;
	y.p25_tx_nac_override = 0x659 + 1;
	CHECK(round_trip(20, &y, &c) && c.mode == RADIO_MODE_YSF && c.p25_tg == 12 && c.p25_tx_nac_override == 0);
	codeplug_tag_t tag;
	CHECK(!codeplug_tag_read(20, &tag));
	uint8_t *yr = &sim_flash_data[EE + 0x3790 + 56u * 19u];
	CHECK(yr[0x18] == 0xA6 && yr[0x2B] == 12);
	/* A P25 channel saved over by YSF loses its tag */
	CHECK(codeplug_set_channel(18, &y) && !codeplug_tag_read(18, &tag) && at[0x18] == 0xA6);

	/* The r7-based Plus's TX NAC tag in an 0xA5 record (0x5000 | NAC in the TX tone word): read as Same as RX */
	uint8_t r[CODEPLUG_OPENFW_CHANNEL_SIZE];
	memcpy(r, at, sizeof r);
	r[0x18] = 0xA5; r[0x2C] = 0xD2; r[0x2B] = 0x93; r[0x2E] = 101; r[0x2F] = 0;
	r[0x22] = 0x59; r[0x23] = 0x56;
	CHECK(codeplug_openfw_decode(r, &c) && c.mode == RADIO_MODE_P25 && c.p25_nac == 0x293);
	CHECK(c.p25_tx_nac_override == 0 && c.tx_tone.type == TONE_NONE && word_at(r, 0x22) == 0x5659);
	CHECK(sim_flash_violations() == 0);
	puts("Plus RX / TX NAC on Classic r13's store: Same as RX, a TX NAC of its own, equal to RX kept, RX Any, the TX "
	     "rule, YSF without one, r7's tag read as Same as RX, flash across a reboot");
	printf("p25nac codeplug: %d failures\n", failures);
	return prior || failures ? 1 : 0;
}
