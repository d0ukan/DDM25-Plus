/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus "any" DMR channels in the real codeplug encoder, flash journal and reboot path (built with
 * CONFIG_DMR_ANY=1): color code 16, timeslot 3 and listen list 0xFFFF go into the high bits of the record's color
 * code byte (0x80, 0x40, 0x20) and come back; ordinary DMR, P25 and FM records are unchanged. */
#define main upstream_codeplug_main
#include "codeplugEditTest.c"
#undef main

int main(void)
{
	int prior = upstream_codeplug_main();
	fresh();
	uint8_t r[CODEPLUG_OPENFW_CHANNEL_SIZE];
	channel_t back;

	/* All three "any" values, RX only */
	channel_t d = make("DMR any", 439100000, 439100000);
	d.mode = RADIO_MODE_DMR;
	d.color_code = CODEPLUG_CC_ANY;
	d.timeslot = CODEPLUG_TS_ANY;
	d.listen_list = CODEPLUG_LIST_ANY;
	d.contact = 0;
	d.rx_only = true;
	memset(r, 0xFF, sizeof r);
	codeplug_openfw_encode(&d, r);
	CHECK(r[0x18] == 1 && r[0x2C] == 0xE0 && r[0x2B] == 0 && (r[0x31] & 0x40) == 0);
	CHECK(codeplug_set_channel(21, &d));
	CHECK(codeplug_init() == CODEPLUG_OPENFW);
	CHECK(codeplug_channel(21, &back));
	CHECK(back.mode == RADIO_MODE_DMR && back.color_code == CODEPLUG_CC_ANY && back.timeslot == CODEPLUG_TS_ANY &&
	      back.listen_list == CODEPLUG_LIST_ANY && back.rx_only);

	/* Each one alone */
	channel_t one = d;
	one.timeslot = 2;
	one.listen_list = 0;
	memset(r, 0xFF, sizeof r);
	codeplug_openfw_encode(&one, r);
	CHECK(r[0x2C] == 0x80 && (r[0x31] & 0x40) != 0);
	CHECK(codeplug_openfw_decode(r, &back) && back.color_code == CODEPLUG_CC_ANY && back.timeslot == 2 &&
	      back.listen_list == 0);
	one = d;
	one.color_code = 5;
	one.listen_list = 0;
	memset(r, 0xFF, sizeof r);
	codeplug_openfw_encode(&one, r);
	CHECK(r[0x2C] == 0x45);
	CHECK(codeplug_openfw_decode(r, &back) && back.color_code == 5 && back.timeslot == CODEPLUG_TS_ANY);
	one = d;
	one.color_code = 9;
	one.timeslot = 1;
	one.rx_only = false;
	memset(r, 0xFF, sizeof r);
	codeplug_openfw_encode(&one, r);
	CHECK(r[0x2C] == 0x29);
	CHECK(codeplug_openfw_decode(r, &back) && back.color_code == 9 && back.timeslot == 1 &&
	      back.listen_list == CODEPLUG_LIST_ANY && !back.rx_only);

	/* An ordinary DMR channel: no high bits, the list index kept */
	channel_t plain = make("DMR TG", 439200000, 431600000);
	plain.mode = RADIO_MODE_DMR;
	plain.color_code = 7;
	plain.timeslot = 2;
	plain.listen_list = 3;
	memset(r, 0xFF, sizeof r);
	codeplug_openfw_encode(&plain, r);
	CHECK(r[0x2C] == 0x07 && r[0x2B] == 3);
	CHECK(codeplug_set_channel(22, &plain) && codeplug_channel(22, &back) && back.color_code == 7 &&
	      back.timeslot == 2 && back.listen_list == 3);

	/* Going back from any to a fixed value clears the bits */
	channel_t fixed = d;
	fixed.color_code = 1;
	fixed.timeslot = 1;
	fixed.listen_list = 0;
	CHECK(codeplug_set_channel(21, &fixed) && codeplug_channel(21, &back) && back.color_code == 1 &&
	      back.timeslot == 1 && back.listen_list == 0);

	/* P25 keeps its 0xD0 marker and NAC; the DMR bits are not read from it */
	channel_t p = make("P25 local", 438500000, 430900000);
	p.mode = RADIO_MODE_P25;
	p.p25_nac = 0xF7E;
	p.p25_tg = 0;
	p.p25_id = 1234;
	p.rx_only = true;
	CHECK(codeplug_set_channel(23, &p) && codeplug_channel(23, &back) && back.mode == RADIO_MODE_P25 &&
	      back.p25_nac == 0xF7E && back.p25_tg == 0);

	/* FM: high bits in a stray byte change nothing */
	channel_t f = make("FM", 145500000, 145500000);
	memset(r, 0xFF, sizeof r);
	codeplug_openfw_encode(&f, r);
	r[0x2C] = 0xE3;
	CHECK(codeplug_openfw_decode(r, &back) && back.mode == RADIO_MODE_FM && back.color_code == 3 &&
	      back.timeslot != CODEPLUG_TS_ANY && back.listen_list != CODEPLUG_LIST_ANY);

	CHECK(sim_flash_violations() == 0);
	puts("Plus DMR any channels: color code, timeslot and talkgroup bits round trip; DMR, P25 and FM unchanged");
	return prior || failures ? 1 : 0;
}
