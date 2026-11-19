/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus SFR role set on the radio (CONFIG_SFR_EDIT): the channel editor's settings go through sfrChannelEncode
 * into channel_t.sfr_flag1 / sfr_a / sfr_b, the real open-firmware encoder writes them (flag 1 bit 1 at 0x26, byte A
 * at 0x25, byte B at 0x2A), and they come back through the decoder, the flash journal, sfrChannelDecode and
 * radio_sfr_channel(). A CPS-made record nobody touched round-trips byte for byte; Off clears the bit and both bytes;
 * a two-frequency channel has no role. Built with CONFIG_SFR_EDIT=1 on upstream's codeplugEditTest.c fixture. */
#define main upstream_codeplug_main
#include "codeplugEditTest.c"
#undef main
#include "dmr/sfr.h"
#include "radio/radio_sfr.h"

#define R_SFR_A		0x25
#define R_LFLAG1	0x26
#define R_SFR_B		0x2A

static channel_t dmr_simplex(void)
{
	channel_t c = make("SFR", 433450000, 433450000);
	c.mode = RADIO_MODE_DMR;
	c.color_code = 1;
	c.timeslot = 1;
	c.contact = 1;
	return c;
}

static void set_role(channel_t *c, uint8_t role, uint8_t slot, bool cc_set, uint8_t cc)
{
	sfrChannelSettings_t s = { .role = role, .inboundSlot = slot, .outboundCCSet = cc_set, .outboundCC = cc };
	sfrChannelEncode(&s, &c->sfr_flag1, &c->sfr_a, &c->sfr_b);
}

/* Every stored role, both inbound slots, with and without an outbound color code (2): editor -> encoder -> record ->
 * decoder -> the radio's role and run configuration */
static void test_roles(void)
{
	static const uint8_t starts[] = { 0x00, 0xFF };	/* all 0 bits, erased flash (the encoder starts it blank) */
	for (uint8_t role = SFR_ROLE_OFF; role < SFR_ROLE_NUM_STORED; role++) {
		for (uint8_t slot = 0; slot < 2; slot++) {
			for (int cc_set = 0; cc_set < 2; cc_set++) {
				for (unsigned st = 0; st < sizeof starts; st++) {
					uint8_t r[CODEPLUG_OPENFW_CHANNEL_SIZE];
					channel_t c = dmr_simplex(), back;
					sfrChannelSettings_t d;
					sfrConfig_t cfg;
					set_role(&c, role, slot, cc_set, 2);
					memset(r, starts[st], sizeof r);
					codeplug_openfw_encode(&c, r);
					if (role == SFR_ROLE_OFF) {
						CHECK((r[R_LFLAG1] & SFR_CHANNEL_FLAG1_ENABLED) == 0);
						CHECK(r[R_SFR_A] == 0 && r[R_SFR_B] == 0);
					} else {
						CHECK((r[R_LFLAG1] & SFR_CHANNEL_FLAG1_ENABLED) != 0);
						CHECK(r[R_SFR_A] == (uint8_t)(role | (slot ? 0x20u : 0u)));
						CHECK(r[R_SFR_B] == (cc_set ? 0x82u : 0x00u));
					}
					CHECK(codeplug_openfw_decode(r, &back));
					CHECK(back.mode == RADIO_MODE_DMR && same_channel(&c, &back));
					CHECK(back.sfr_flag1 == c.sfr_flag1 && back.sfr_a == c.sfr_a && back.sfr_b == c.sfr_b);
					CHECK(sfrChannelDecode(back.sfr_flag1, back.sfr_a, back.sfr_b, &d) == (role != SFR_ROLE_OFF));
					CHECK(d.role == role && d.inboundSlot == (role ? slot : 0) && d.framing == SFR_FRAMING_AUTO &&
					      d.hangIndex == 0 && d.outboundCCSet == (role && cc_set));
					CHECK(radio_sfr_channel(&back, false, &cfg) == role);
					if (role != SFR_ROLE_OFF) {
						bool erdm = role == SFR_ROLE_USER_ERDM || role == SFR_ROLE_RPT_MOTOROLA;
						CHECK(cfg.role == role && cfg.inboundSlot == slot && cfg.inboundCC == 1);
						CHECK(cfg.outboundCC == (erdm && cc_set ? 2 : 1));
						CHECK(cfg.framing == (erdm ? SFR_FRAMING_BS : SFR_FRAMING_TS));
						CHECK(cfg.hangMs == 1000);
					}
					/* The VFO never has a role */
					CHECK(radio_sfr_channel(&back, true, &cfg) == SFR_ROLE_OFF);
				}
			}
		}
	}
}

/* Through the flash journal and a reboot: each role saved, read back, then turned off */
static void test_flash(void)
{
	fresh();
	channel_t back;
	sfrConfig_t cfg;
	for (uint8_t role = SFR_ROLE_USER_STANDARD; role < SFR_ROLE_NUM_STORED; role++) {
		channel_t c = dmr_simplex();
		set_role(&c, role, role & 1u, role == SFR_ROLE_RPT_MOTOROLA, 7);
		CHECK(codeplug_set_channel(30, &c));
		CHECK(codeplug_init() == CODEPLUG_OPENFW);
		CHECK(codeplug_channel(30, &back) && same_channel(&c, &back) && back.sfr_flag1 == c.sfr_flag1 &&
		      back.sfr_a == c.sfr_a && back.sfr_b == c.sfr_b);
		CHECK(radio_sfr_channel(&back, false, &cfg) == role && cfg.inboundSlot == (role & 1u));
		CHECK(cfg.outboundCC == (role == SFR_ROLE_RPT_MOTOROLA ? 7 : 1));
	}
	/* Off from the editor (sfrChannelEncode of SFR_ROLE_OFF) clears the stored role */
	CHECK(codeplug_channel(30, &back));
	set_role(&back, SFR_ROLE_OFF, 0, false, 0);
	CHECK(back.sfr_flag1 == 0 && back.sfr_a == 0 && back.sfr_b == 0);
	CHECK(codeplug_set_channel(30, &back));
	CHECK(codeplug_init() == CODEPLUG_OPENFW);
	CHECK(codeplug_channel(30, &back) && back.sfr_flag1 == 0 && back.sfr_a == 0 && back.sfr_b == 0);
	CHECK(radio_sfr_channel(&back, false, &cfg) == SFR_ROLE_OFF);
	/* Save as new channel: the role goes with the copy into a free number (a blank record) */
	channel_t c = dmr_simplex();
	set_role(&c, SFR_ROLE_USER_ERDM, 1, true, 2);
	uint16_t n = codeplug_free_channel();
	CHECK(n != 0 && codeplug_set_channel(n, &c) && codeplug_channel(n, &back) && back.sfr_flag1 == c.sfr_flag1 &&
	      back.sfr_a == 0x22 && back.sfr_b == 0x82);
	CHECK(sim_flash_violations() == 0);
}

/* CPS-made records the editor never touched: decode -> encode gives every byte back, whatever the SFR bytes hold
 * (framing, hang, B's spare bits, a role this firmware never writes, bytes without the bit) and whatever the other
 * flag 1 bits are */
static void test_cps_preserved(void)
{
	static const uint8_t pats[][3] = {	/* flag 1, byte A, byte B */
		{ 0x02, 0x01, 0x00 }, { 0x02, 0xDB, 0x8C }, { 0x02, 0x7C, 0xF5 }, { 0x02, 0x07, 0x00 }, { 0x02, 0x00, 0x00 },
		{ 0x02, 0xE3, 0x7F }, { 0xC7, 0x2A, 0x85 }, { 0x00, 0xFF, 0xFF }, { 0x00, 0x03, 0x82 }, { 0xFD, 0x01, 0x80 },
		{ 0x00, 0x00, 0x00 },
	};
	for (unsigned i = 0; i < sizeof pats / sizeof pats[0]; i++) {
		for (int mode = 0; mode < 2; mode++) {
			uint8_t r[CODEPLUG_OPENFW_CHANNEL_SIZE], old[CODEPLUG_OPENFW_CHANNEL_SIZE];
			channel_t c = mode ? dmr_simplex() : make("FM", 145500000, 145500000), d;
			memset(r, 0x00, sizeof r);
			codeplug_openfw_encode(&c, r);
			r[R_LFLAG1] = (uint8_t)((r[R_LFLAG1] & ~0x02u) | pats[i][0]);
			r[R_SFR_A] = pats[i][1];
			r[R_SFR_B] = pats[i][2];
			memcpy(old, r, sizeof r);
			CHECK(codeplug_openfw_decode(r, &d));
			codeplug_openfw_encode(&d, r);
			CHECK(memcmp(old, r, sizeof r) == 0);
			/* An edit of another field leaves them too */
			d.no_beep = !d.no_beep;
			codeplug_openfw_encode(&d, r);
			CHECK(r[R_SFR_A] == old[R_SFR_A] && r[R_SFR_B] == old[R_SFR_B] &&
			      ((r[R_LFLAG1] ^ old[R_LFLAG1]) & 0x02u) == 0);
		}
	}
	/* Through flash: the record written by "the CPS" (encoded, then its SFR bytes set), read and saved back */
	fresh();
	channel_t c = dmr_simplex(), back;
	CHECK(codeplug_set_channel(31, &c));
	CHECK(codeplug_channel(31, &back));
	back.sfr_flag1 = 0x02;
	back.sfr_a = 0xDB;	/* hang 3, framing BS, TS2, Rpt */
	back.sfr_b = 0x8C;
	CHECK(codeplug_set_channel(31, &back));
	CHECK(codeplug_channel(31, &c) && c.sfr_flag1 == 0x02 && c.sfr_a == 0xDB && c.sfr_b == 0x8C);
	c.tot_s = 60;	/* the editor saves another field */
	CHECK(codeplug_set_channel(31, &c) && codeplug_channel(31, &back) && back.sfr_a == 0xDB && back.sfr_b == 0x8C &&
	      back.tot_s == 60);
	/* The VFO's record in the CPS view: built from the settings (no role), its stored bytes kept */
	uint8_t r[CODEPLUG_OPENFW_CHANNEL_SIZE];
	memset(r, 0x00, sizeof r);
	channel_t vfo = dmr_simplex();
	codeplug_openfw_encode(&vfo, r);
	r[R_LFLAG1] |= 0x02u;
	r[R_SFR_A] = 0x03;
	r[R_SFR_B] = 0x11;
	codeplug_openfw_sfr(r, &vfo);
	CHECK(vfo.sfr_flag1 == 0x02 && vfo.sfr_a == 0x03 && vfo.sfr_b == 0x11);
	codeplug_openfw_encode(&vfo, r);
	CHECK((r[R_LFLAG1] & 0x02u) && r[R_SFR_A] == 0x03 && r[R_SFR_B] == 0x11);
	r[R_LFLAG1] &= (uint8_t)~0x02u;
	r[R_SFR_A] = 0xFF;
	codeplug_openfw_sfr(r, &vfo);
	CHECK(vfo.sfr_flag1 == 0 && vfo.sfr_a == 0 && vfo.sfr_b == 0);
	codeplug_openfw_encode(&vfo, r);
	CHECK(!(r[R_LFLAG1] & 0x02u) && r[R_SFR_A] == 0xFF && r[R_SFR_B] == 0x11);
	CHECK(sim_flash_violations() == 0);
}

/* Off clears flag 1 bit 1 and both bytes of a record that had a role; the other flag 1 bits stay */
static void test_off_clears(void)
{
	uint8_t r[CODEPLUG_OPENFW_CHANNEL_SIZE];
	channel_t c = dmr_simplex(), d;
	c.no_beep = c.no_eco = c.dmo = true;
	memset(r, 0xFF, sizeof r);
	codeplug_openfw_encode(&c, r);
	r[R_LFLAG1] |= 0x02u;
	r[R_SFR_A] = 0xDB;
	r[R_SFR_B] = 0x8C;
	uint8_t flags = r[R_LFLAG1];
	CHECK(codeplug_openfw_decode(r, &d) && d.sfr_flag1 == 0x02);
	set_role(&d, SFR_ROLE_OFF, 1, true, 5);
	codeplug_openfw_encode(&d, r);
	CHECK(r[R_LFLAG1] == (uint8_t)(flags & ~0x02u) && r[R_SFR_A] == 0 && r[R_SFR_B] == 0);
	CHECK(codeplug_openfw_decode(r, &d) && d.sfr_flag1 == 0 && d.no_beep && d.no_eco && d.dmo);
}

/* A role needs one frequency (the editor refuses the row elsewhere; the radio ignores stored bytes there); a repeater
 * also needs TX; FM has none */
static void test_one_frequency(void)
{
	sfrConfig_t cfg;
	channel_t c = dmr_simplex();
	set_role(&c, SFR_ROLE_USER_STANDARD, 0, false, 0);
	CHECK(radio_sfr_channel(&c, false, &cfg) == SFR_ROLE_USER_STANDARD);
	c.tx_hz = c.rx_hz + 5000000u;	/* two frequencies */
	CHECK(radio_sfr_channel(&c, false, &cfg) == SFR_ROLE_OFF);
	set_role(&c, SFR_ROLE_RPT_STANDARD, 0, false, 0);
	CHECK(radio_sfr_channel(&c, false, &cfg) == SFR_ROLE_OFF);
	c.tx_hz = c.rx_hz;
	CHECK(radio_sfr_channel(&c, false, &cfg) == SFR_ROLE_RPT_STANDARD);
	c.rx_only = true;
	CHECK(radio_sfr_channel(&c, false, &cfg) == SFR_ROLE_OFF);
	set_role(&c, SFR_ROLE_USER_ERDM, 0, false, 0);
	CHECK(radio_sfr_channel(&c, false, &cfg) == SFR_ROLE_USER_ERDM);	/* a user may listen only */
	c.mode = RADIO_MODE_FM;
	CHECK(radio_sfr_channel(&c, false, &cfg) == SFR_ROLE_OFF);
	/* The bytes stay with a two-frequency channel (the row says it needs one frequency, the radio ignores them) */
	uint8_t r[CODEPLUG_OPENFW_CHANNEL_SIZE];
	channel_t two = dmr_simplex(), back;
	two.tx_hz = two.rx_hz - 7600000u;
	set_role(&two, SFR_ROLE_USER_STANDARD, 1, false, 0);
	memset(r, 0x00, sizeof r);
	codeplug_openfw_encode(&two, r);
	CHECK(codeplug_openfw_decode(r, &back) && back.sfr_a == 0x21 && radio_sfr_channel(&back, false, &cfg) == SFR_ROLE_OFF);
}

int main(void)
{
	int prior = upstream_codeplug_main();
	test_roles();
	test_flash();
	test_cps_preserved();
	test_off_clears();
	test_one_frequency();
	puts("Plus SFR edit: every role round trips, CPS bytes kept, Off clears, two frequencies have no role");
	return prior || failures ? 1 : 0;
}
