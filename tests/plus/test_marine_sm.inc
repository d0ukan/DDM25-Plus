/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus marine VHF receive only (docs/plus.md "Marine VHF"): the real radio state machine on radioCoreTest's fake
 * board (its band 136-174 MHz transmits, so only the marine rule can refuse) with CONFIG_MARINE=1. Included after
 * upstream's radioCoreTest.c (its main renamed upstream_radio_main, which runs first: the whole suite with the rule
 * built in) by tools/test_classic.py. Checks: Tools > Marine VHF's requests (VFO on the coast frequency, FM, 25 kHz,
 * no tones, simplex) and PTT refused there as RX only with nothing keyed; every listenable channel and every ship /
 * coast frequency of the table refused; a TX split onto a marine frequency refused, a split away from it too (the RX
 * stays marine); a VFO typed off the channels (12.5 kHz away) and an amateur frequency transmit; a codeplug channel on
 * a marine frequency is the codeplug's (not refused); the refusal after a power cycle (the VFO's frequency is kept);
 * the radio core's other refusals unchanged (TX inhibit first). */
#include "radio/marine.h"

static int marine_fails;
#define MCHECK(c) do { if (!(c)) { marine_fails++; printf("FAIL marine %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* PTT on what the radio has now: refused as RX only and nothing keyed (want_tx false), or keyed */
static void marine_ptt(bool want_tx, uint32_t tx_hz)
{
	int before = hw.tx_calls;
	req(RREQ_PTT, 1);
	poll_for(100);
	if (want_tx) {
		MCHECK(sm.view.state == RS_TX && hw.tx_calls == before + 1 && hw.tx_hz == tx_hz);
	} else {
		MCHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_RX_ONLY && hw.tx_calls == before);
		MCHECK(radio_sm_tx_check(&sm) == TXDENY_RX_ONLY);
	}
	req(RREQ_PTT, 0);
	poll_for(100);
}

/* What ui_marine.c's listen_on() asks, in its order */
static void marine_listen(uint32_t hz)
{
	radio_req_t r = { RREQ_TONE, 0, 0 };
	req(RREQ_VFO_SET, (int32_t)hz);
	req(RREQ_MODE, RADIO_MODE_FM);
	req(RREQ_BANDWIDTH, RADIO_BW_25K);
	r.arg = radio_tone_arg(RADIO_TONE_TX, (subtone_t){ TONE_NONE, 0 });
	radio_sm_request(&sm, &r, now);
	r.arg = radio_tone_arg(RADIO_TONE_RX, (subtone_t){ TONE_NONE, 0 });
	radio_sm_request(&sm, &r, now);
	req(RREQ_VFO_TX, 0);
	poll_for(50);
}

static void test_marine_vfo(void)
{
	setup_codeplug();
	start();
	/* Channel 16 from the screen: the VFO there, FM 25 kHz, simplex; PTT refused, nothing keyed */
	marine_listen(156800000u);
	MCHECK(sm.view.vfo && sm.view.ch.rx_hz == 156800000u && sm.view.ch.tx_hz == 156800000u);
	MCHECK(sm.view.ch.mode == RADIO_MODE_FM && sm.view.ch.bw == RADIO_BW_25K && hw.rx_hz == 156800000u);
	MCHECK(sm.view.ch.tx_tone.type == TONE_NONE && sm.view.ch.rx_tone.type == TONE_NONE);
	marine_ptt(false, 0);
	/* Every channel the screen tunes, and every ship and coast frequency of the table typed into the VFO */
	for (uint8_t i = 0; i < marine_count; i++) {
		uint32_t hz;
		if (marine_listen_hz(&marine_table[i], &hz)) {
			marine_listen(hz);
			MCHECK(sm.view.ch.rx_hz == hz && radio_sm_tx_check(&sm) == TXDENY_RX_ONLY);
		}
		req(RREQ_VFO_SET, (int32_t)marine_ship_hz(&marine_table[i]));
		MCHECK(radio_sm_tx_check(&sm) == TXDENY_RX_ONLY);
		if (marine_coast_hz(&marine_table[i])) {
			req(RREQ_VFO_SET, (int32_t)marine_coast_hz(&marine_table[i]));
			MCHECK(radio_sm_tx_check(&sm) == TXDENY_RX_ONLY);
		}
	}
	/* Duplex channel 22 listened to (coast 161.700): a TX split to its ship frequency, or anywhere else, is refused */
	marine_listen(161700000u);
	req(RREQ_VFO_TX, 157100000);
	MCHECK(sm.view.ch.tx_hz == 157100000u);
	marine_ptt(false, 0);
	req(RREQ_VFO_TX, 146520000);
	MCHECK(sm.view.ch.tx_hz == 146520000u);
	marine_ptt(false, 0);		/* the receiver is still on a marine channel */
	/* Off the channels: 12.5 kHz from 16 and an amateur frequency transmit (the rule is the table's frequencies) */
	req(RREQ_VFO_SET, 156812500);
	req(RREQ_VFO_TX, 0);
	marine_ptt(true, 156812500u);
	req(RREQ_VFO_SET, 145500000);
	req(RREQ_VFO_TX, 0);
	marine_ptt(true, 145500000u);
	/* An amateur RX with a split onto a marine frequency: refused */
	req(RREQ_VFO_TX, 156800000);
	marine_ptt(false, 0);
	req(RREQ_VFO_TX, 0);
	/* A codeplug channel on 156.800 MHz is the codeplug's business: not refused by this rule */
	chans[6].rx_hz = chans[6].tx_hz = 156800000u;	/* zone "Two"'s channel */
	req(RREQ_ZONE, 1);
	poll_for(50);
	MCHECK(!sm.view.vfo && sm.view.ch.rx_hz == 156800000u);
	marine_ptt(true, 156800000u);
	setup_codeplug();
	/* TX inhibit still comes first */
	marine_listen(156800000u);
	sm.set.tx_inhibit = true;
	MCHECK(radio_sm_tx_check(&sm) == TXDENY_INHIBIT);
	sm.set.tx_inhibit = false;
	/* After a power cycle: the VFO comes back on channel 16 (its frequency is kept), still receive only */
	poll_for(5000);		/* the settings are saved */
	boot();
	poll_for(200);
	if (!sm.view.vfo)
		req(RREQ_VFO, 1);
	MCHECK(sm.view.vfo && sm.view.ch.rx_hz == 156800000u);
	MCHECK(radio_sm_tx_check(&sm) == TXDENY_RX_ONLY);
	marine_ptt(false, 0);
}

int main(void)
{
	int prior = upstream_radio_main();
	test_marine_vfo();
	printf("Plus marine VHF receive only: %d failures\n", marine_fails);
	return prior != 0 || marine_fails != 0;
}
