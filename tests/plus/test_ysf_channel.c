/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus YSF channels in the real codeplug encoder, flash journal and reboot path (built with CONFIG_YSF=1):
 * the record (mode 0xA6, DG-ID in the RX group byte), RX only forced, the DG-ID limit, and P25 records unchanged. */
#define main upstream_codeplug_main
#include "codeplugEditTest.c"
#undef main

int main(void)
{
	int prior = upstream_codeplug_main();
	fresh();
	channel_t y = make("YSF room", 439000000, 439000000), back;
	y.mode = RADIO_MODE_YSF;
	y.rx_only = true;
	y.p25_nac = 0xF7E;
	y.p25_tg = 42;
	y.p25_id = 0;
	CHECK(codeplug_mode_ysf(y.mode) && codeplug_mode_p25(y.mode));	/* a native-modem channel */
	CHECK(codeplug_set_channel(11, &y));
	CHECK(codeplug_init() == CODEPLUG_OPENFW);
	CHECK(codeplug_channel(11, &back));
	CHECK(back.mode == RADIO_MODE_YSF && back.p25_tg == 42 && back.rx_only && back.rx_hz == y.rx_hz);
	uint8_t r[CODEPLUG_OPENFW_CHANNEL_SIZE];
	memset(r, 0xFF, sizeof r);
	codeplug_openfw_encode(&y, r);
	CHECK(r[0x18] == 0xA6 && r[0x2B] == 42);
	/* A channel that allows TX: refused without the YSF transmitter (CONFIG_YSF_TX=0); kept as it is with it */
	channel_t t = y;
	t.rx_only = false;
#if CONFIG_YSF_TX
	CHECK(codeplug_set_channel(12, &t) && codeplug_channel(12, &back) && back.mode == RADIO_MODE_YSF &&
	      !back.rx_only && back.p25_tg == 42 && back.tx_hz == t.tx_hz);
	CHECK(codeplug_init() == CODEPLUG_OPENFW && codeplug_channel(12, &back) && !back.rx_only);	/* after a reboot */
	CHECK(codeplug_channel(11, &back) && back.rx_only);	/* an RX only one stays RX only */
#else
	CHECK(!codeplug_set_channel(12, &t));	/* refused: no YSF TX */
#endif
	y.p25_tg = 128;
	CHECK(!codeplug_set_channel(12, &y));	/* DG-ID 0..127 */
	y.p25_tg = 0;
	CHECK(codeplug_set_channel(12, &y) && codeplug_channel(12, &back) && back.p25_tg == 0);	/* 0 = any */
	/* P25 is unchanged */
	channel_t p = make("P25 local", 438500000, 430900000);
	p.mode = RADIO_MODE_P25;
	p.p25_nac = 0xABC;
	p.p25_tg = 655;
	p.p25_id = 1234;
	CHECK(codeplug_set_channel(13, &p) && codeplug_channel(13, &back) && back.mode == RADIO_MODE_P25 &&
	      !codeplug_mode_ysf(back.mode) && back.p25_nac == 0xABC);
	CHECK(sim_flash_violations() == 0);
	puts(CONFIG_YSF_TX ? "Plus YSF channel persistence (YSF transmit): record, TX allowed or RX only, DG-ID limits, P25 unchanged"
			   : "Plus YSF channel persistence: record, RX only, DG-ID limits, P25 unchanged");
	return prior || failures ? 1 : 0;
}
