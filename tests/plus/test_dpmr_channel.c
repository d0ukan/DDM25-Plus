/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus dPMR channels in the real codeplug encoder, flash journal and reboot path (built with CONFIG_NXDN=1 and
 * CONFIG_DPMR=1): the record (mode 0xA8, the colour code filter in the RX group byte: 0 any, else code number + 1),
 * RX only forced, the limit, and NXDN records unchanged. */
#define main upstream_codeplug_main
#include "codeplugEditTest.c"
#undef main

int main(void)
{
	int prior = upstream_codeplug_main();
	fresh();
	channel_t d = make("dPMR 446", 446103125, 446103125), back;
	d.mode = RADIO_MODE_DPMR;
	d.rx_only = true;
	d.p25_nac = 0;
	d.p25_tg = 17;		/* colour code 16 */
	d.p25_id = 0;
	CHECK(codeplug_mode_dpmr(d.mode) && !codeplug_mode_p25(d.mode) && !codeplug_mode_nxdn(d.mode));
	CHECK(codeplug_set_channel(11, &d));
	CHECK(codeplug_init() == CODEPLUG_OPENFW);
	CHECK(codeplug_channel(11, &back));
	/* the record keeps 10 Hz steps: dPMR446's 446.103125 MHz centre is stored as 446.10312 (5 Hz low) */
	CHECK(back.mode == RADIO_MODE_DPMR && back.p25_tg == 17 && back.rx_only && back.rx_hz == 446103120u &&
	      back.bw == RADIO_BW_12K5);
	uint8_t r[CODEPLUG_OPENFW_CHANNEL_SIZE];
	memset(r, 0xFF, sizeof r);
	codeplug_openfw_encode(&d, r);
	CHECK(r[0x18] == 0xA8 && r[0x2B] == 17);
	CHECK(codeplug_openfw_decode(r, &back) && back.mode == RADIO_MODE_DPMR && back.p25_tg == 17 && back.rx_only);
	r[0x2B] = 65;		/* past code 63: not a channel */
	CHECK(!codeplug_openfw_decode(r, &back));
	/* an edit that asks for TX is refused */
	channel_t t = d;
	t.rx_only = false;
	CHECK(!codeplug_set_channel(12, &t));
	d.p25_tg = 65;
	CHECK(!codeplug_set_channel(12, &d));	/* code numbers 0..63 (stored 1..64) */
	d.p25_tg = 64;
	CHECK(codeplug_set_channel(12, &d) && codeplug_channel(12, &back) && back.p25_tg == 64);	/* code 63 */
	d.p25_tg = 0;
	CHECK(codeplug_set_channel(12, &d) && codeplug_channel(12, &back) && back.p25_tg == 0);	/* any */
	/* NXDN is unchanged */
	channel_t n = make("NXDN", 439000000, 439000000);
	n.mode = RADIO_MODE_NXDN;
	n.rx_only = true;
	n.p25_tg = 12;
	CHECK(codeplug_set_channel(13, &n) && codeplug_channel(13, &back) && back.mode == RADIO_MODE_NXDN &&
	      back.p25_tg == 12 && !codeplug_mode_dpmr(back.mode));
	memset(r, 0xFF, sizeof r);
	codeplug_openfw_encode(&n, r);
	CHECK(r[0x18] == 0xA7 && r[0x2B] == 12);
	CHECK(sim_flash_violations() == 0);
	puts("Plus dPMR channel persistence: record, RX only, colour code limits, NXDN unchanged");
	return prior || failures ? 1 : 0;
}
