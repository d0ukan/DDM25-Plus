/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus "any timeslot" in the real DMR call layer (built with CONFIG_DMR_ANY=1, inside upstream's call test
 * fixture): through a repeater the modem gets no slot lock, a call on either slot plays, the view names the slot
 * heard and goes back to "any" after it, and the other slot stays out while a call is heard. Any color code: the
 * view shows the call's and goes back to "any" (16) after it. Without any_ts the channel's slot rules as before. */
#define main upstream_call_main
#include "dmrCallTest.c"
#undef main

/* A repeater call on one slot: three headers, `superframes` superframes, a terminator (BS sync, slot known) */
static void rpt_call(const uint8_t lc[9], uint8_t cc, int8_t ts, int superframes, unsigned *frame)
{
	for (int h = 0; h < 3; h++) {
		rx_header(lc, cc, ts, DMR_SYNC_BS, false);
		run_ms(60);
	}
	for (int sf = 0; sf < superframes; sf++)
		for (int i = 0; i < 6; i++) {
			rx_voice((uint8_t)i, cc, ts, DMR_SYNC_BS, frame, i == 5 ? lc : NULL, false);
			run_ms(60);
		}
}

static void any_ts_tests(void)
{
	dmr_call_config_t c = base_cfg();
	uint8_t lc[9];
	unsigned frame = 1;
	dmr_call_stats_t st;

	c.rmo = true;
	c.ts = 1;
	mk_lc(lc, DMR_FLCO_GROUP_VOICE, TG_TALK, 2345678);

	/* The channel's slot only: a TS2 call is dropped, the modem is locked to TS1 */
	setup(&c);
	CHECK(sim.locked_ts == 1);
	rpt_call(lc, 1, 2, 2, &frame);
	CHECK(sink_log.begins == 0);
	rx_header(lc, 1, 2, DMR_SYNC_BS, true);
	run_ms(DMR_CALL_LBT_DMO_MS + 500);

	/* Any slot: no lock, the TS2 call plays and the view names TS2 */
	c.listen.any_ts = true;
	setup(&c);
	CHECK(sim.locked_ts == 0);
	CHECK(V()->ts == 0);	/* the UI shows the channel's: any */
	rpt_call(lc, 1, 2, 1, &frame);
	CHECK(sink_log.begins == 1 && V()->ts == 2);
	/* TS1 bursts while the TS2 call lasts stay out */
	dmr_call_get_stats(&st);
	uint32_t dropped = st.rx_ts_dropped;
	rx_voice(0, 1, 1, DMR_SYNC_BS, &frame, NULL, false);
	run_ms(60);
	dmr_call_get_stats(&st);
	CHECK(st.rx_ts_dropped == dropped + 1 && sink_log.begins == 1);
	rx_header(lc, 1, 2, DMR_SYNC_BS, true);
	run_ms(500);
	CHECK(V()->state == DMR_CALL_STATE_IDLE && V()->ts == 0);
	/* Then a TS1 call plays too */
	run_ms(DMR_CALL_LBT_DMO_MS + 500);
	rpt_call(lc, 1, 1, 1, &frame);
	CHECK(sink_log.begins == 2 && V()->ts == 1);
	rx_header(lc, 1, 1, DMR_SYNC_BS, true);
	run_ms(500);

	/* Reconfiguring any_ts is an RF change: the lock comes back with it off */
	c.listen.any_ts = false;
	dmr_call_configure(&c);
	run_ms(10);
	CHECK(sim.locked_ts == 1 && V()->ts == 1);

	/* Any color code with any slot: the call's CC shows, then "any" (16) again */
	c.listen.any_ts = true;
	c.listen.any_cc = true;
	setup(&c);
	CHECK(sim.cc_filter == -1 && V()->cc == 16 && V()->ts == 0);
	rpt_call(lc, 9, 2, 1, &frame);
	CHECK(sink_log.begins == 1 && V()->cc == 9 && V()->ts == 2);
	rx_header(lc, 9, 2, DMR_SYNC_BS, true);
	run_ms(500);
	CHECK(V()->cc == 16 && V()->ts == 0);

	/* Direct mode: no lock either way (as before) */
	c = base_cfg();
	c.listen.any_ts = true;
	setup(&c);
	CHECK(sim.locked_ts == 0);
	rx_call(lc, 1, true, 0, 1, true, &frame);
	CHECK(sink_log.begins == 1);
}

int main(void)
{
	int prior = upstream_call_main();
	any_ts_tests();
	puts("Plus DMR any timeslot / any color code: no slot lock, either slot plays, the view names it");
	return prior || failures ? 1 : 0;
}
