/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus r15 "channels" (docs/plus.md "Channels, zones and DMR rows"): Home's channel stepping in the real radio
 * state machine on radioCoreTest's fake codeplug (included after upstream's radioCoreTest.c, its main renamed
 * upstream_radio_main, which runs first; tools/test_chux.py gives the fixture 90 channels, upstream's suite uses its
 * eight). Channels 11-90 fill the zone. Checks: a zone of 26 steps 1..26 and wraps; grown to 27 underneath (the codeplug changes, as a
 * zone edit's write does) Home steps on to 27 and the view says 27 channels; grown to 80 every position is reached
 * both ways, the view's zone size 80; position 80 kept after a reboot. */
static int zr_fails;
#define ZCHECK(c) do { if (!(c)) { zr_fails++; printf("FAIL zone radio %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void zr_fill(uint16_t count)
{
	zones[0].count = count;
	for (uint16_t i = 0; i < count; i++) {
		uint16_t n = (uint16_t)(11u + i);
		char name[CODEPLUG_NAME_LEN + 1];
		snprintf(name, sizeof name, "Ch %u", (unsigned)n);
		add(n, name, 145000000u + 12500u * n, i % 2u ? RADIO_MODE_DMR : RADIO_MODE_FM, false, CODEPLUG_POWER_DEFAULT);
		chans[n].color_code = 1;
		chans[n].timeslot = 1;
		chans[n].contact = 1;
		zones[0].channels[i] = n;
	}
	cp_serial++;
}

static void zr_step(int dir, uint16_t want)
{
	req(RREQ_CH_STEP, dir);
	ZCHECK(sm.view.zone_pos == want && !sm.view.vfo);
}

static void test_zone_steps(void)
{
	setup_codeplug();
	zr_fill(26);
	start();
	poll_for(RADIO_POLL_MS);
	ZCHECK(sm.view.zone_index == 0 && sm.view.zone_size == 26 && sm.view.zone_pos == 0);
	for (uint16_t p = 1; p < 26; p++)
		zr_step(1, p);
	zr_step(1, 0);		/* past the last: the first */
	zr_step(-1, 25);
	/* The 27th added underneath: the radio reads the zone again and steps on to it */
	zr_fill(27);
	poll_for(RADIO_POLL_MS);
	ZCHECK(sm.view.zone_size == 27 && sm.view.zone_pos == 25);
	zr_step(1, 26);
	zr_step(1, 0);
	/* 80: every position both ways */
	zr_fill(80);
	poll_for(RADIO_POLL_MS);
	ZCHECK(sm.view.zone_size == 80);
	for (uint16_t p = 1; p < 80; p++)
		zr_step(1, p);
	zr_step(1, 0);
	for (int p = 79; p >= 0; p--)
		zr_step(-1, (uint16_t)p);
	zr_step(-1, 79);
	poll_for(5000);		/* the position is saved */
	boot();
	poll_for(200);
	ZCHECK(sm.view.zone_size == 80 && sm.view.zone_pos == 79 && sm.view.channel_number == 90);
}

int main(void)
{
	int prior = upstream_radio_main();
	test_zone_steps();
	printf("Plus zone stepping (80 channels): %d failures\n", zr_fails);
	return prior != 0 || zr_fails != 0;
}
