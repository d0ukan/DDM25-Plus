/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM25 Plus one-key SOS (core/ui/sos.c) against recording fakes of what it calls: the emergency key's
 * ui_svc_emergency, the APRS engine's aprs_sos_set / aprs_plus_state, the CW beacon's request and view. Proves that
 * nothing is asked to transmit before sos_start (the confirmation) nor with every action off; the order (CW first,
 * APRS once it is on air, DMR once it has ended); each action asked once; the refusals and their events; Stop; an
 * emergency / APRS SOS that was on before is never stopped. Built with -DCONFIG_SOS_KEY=1 -DCONFIG_DMR_EMERGENCY=1
 * -DCONFIG_APRS_MSG=1 -DCONFIG_CW_BEACON=1 (and once with CONFIG_CW_BEACON=0: the two start at once). */
#include <stdio.h>
#include <string.h>
#include "ui/sos.h"
#include "ui/ui_svc.h"
#include "dmr/dmr_emergency.h"
#include "aprs/aprs.h"
#include "aprs/aprs_plus.h"
#if CONFIG_CW_BEACON
#include "radio/cw_beacon.h"
#endif

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* --- the fakes --- */
static int n_emerg_on, n_emerg_off, n_aprs_on, n_aprs_off, n_cw, n_cw_stop;
static bool f_emerg_active, f_emerg_accept;
static dmr_emergency_state_t f_emerg_state;
static uint8_t f_aprs_why;
static aprs_plus_t f_aprs;
#if CONFIG_CW_BEACON
static cw_beacon_view_t f_cw;
#endif

bool ui_svc_emergency(bool on)
{
	if (!on) {
		n_emerg_off++;
		f_emerg_active = false;
		return true;
	}
	n_emerg_on++;
	return f_emerg_accept;
}
bool ui_svc_emergency_active(void) { return f_emerg_active; }
dmr_emergency_state_t dmr_emergency_state(void) { return f_emerg_state; }

uint8_t aprs_sos_set(bool on)
{
	if (!on) {
		n_aprs_off++;
		f_aprs.sos = false;
		return APRS_OK;
	}
	n_aprs_on++;
	if (f_aprs_why == APRS_OK)
		f_aprs.sos = true;
	return f_aprs_why;
}
void aprs_plus_state(aprs_plus_t *s) { *s = f_aprs; }

#if CONFIG_CW_BEACON
void radio_req_cw_beacon(uint8_t kind, uint8_t interval)
{
	if (kind >= CW_BEACON_KINDS) {
		n_cw_stop++;
		f_cw.on = f_cw.keying = false;
		return;
	}
	CHECK(kind == CW_BEACON_SOS && interval == 0);	/* the SOS text, once */
	n_cw++;
}
void cw_beacon_view(cw_beacon_view_t *out) { *out = f_cw; }
#endif

static void reset(void)
{
	n_emerg_on = n_emerg_off = n_aprs_on = n_aprs_off = n_cw = n_cw_stop = 0;
	f_emerg_active = false;
	f_emerg_accept = true;
	f_emerg_state = DMR_EMERGENCY_IDLE;
	f_aprs_why = APRS_OK;
	memset(&f_aprs, 0, sizeof f_aprs);
#if CONFIG_CW_BEACON
	memset(&f_cw, 0, sizeof f_cw);
	f_cw.sent = 7;		/* messages of earlier beacons */
#endif
	sos_stop();		/* nothing of ours left from the case before */
	n_emerg_off = n_aprs_off = n_cw_stop = 0;
}

static int asks(void)
{
	return n_emerg_on + n_aprs_on + n_cw;
}

static uint32_t now = 1000;

/* Ticks for ms, events counted */
static int evc[16];
static void run(uint32_t ms)
{
	for (uint32_t t = 0; t < ms; t += 20) {
		now += 20;
		sos_ev_t e = sos_tick(now);
		if (e != SOS_EV_NONE)
			evc[e]++;
	}
}

static void clear_ev(void)
{
	memset(evc, 0, sizeof evc);
}

int main(void)
{
	/* The build's actions; the choice keeps only those; all off after power on */
	CHECK(sos_built() == (SOS_APRS | SOS_DMR | (CONFIG_CW_BEACON ? SOS_CW : 0)));
	CHECK(sos_mask() == 0);
	sos_mask_set(0xFF);
	CHECK(sos_mask() == sos_built());
	sos_mask_set(0);

	/* Never asked to transmit without the confirmation (sos_start), whatever is chosen */
	reset();
	sos_mask_set(SOS_ALL);
	clear_ev();
	run(60000);
	CHECK(asks() == 0 && !sos_active());
	for (int e = 0; e < 16; e++)
		CHECK(evc[e] == 0);

	/* Confirmed with every action off: nothing asked, nothing running */
	reset();
	sos_mask_set(0);
	CHECK(sos_start(now) == 0);
	run(60000);
	CHECK(asks() == 0 && !sos_active());
	CHECK(sos_res(SOS_APRS) == SOS_R_NONE && sos_res(SOS_DMR) == SOS_R_NONE && sos_res(SOS_CW) == SOS_R_NONE);

	/* APRS and DMR (no CW): both at once, each asked once only */
	reset();
	clear_ev();
	sos_mask_set(SOS_APRS | SOS_DMR);
	CHECK(sos_start(now) == 2);
	CHECK(n_emerg_on == 1 && n_aprs_on == 1 && n_cw == 0 && sos_active());
	CHECK(sos_res(SOS_DMR) == SOS_R_WAIT && sos_res(SOS_APRS) == SOS_R_WAIT);
	f_emerg_active = true;	/* the radio core took the emergency */
	run(200);
	CHECK(sos_res(SOS_DMR) == SOS_R_SENT && evc[SOS_EV_DMR_SENT] == 1);
	f_aprs.sos_sent = 1;	/* the engine sent the first SOS position */
	run(200);
	CHECK(sos_res(SOS_APRS) == SOS_R_SENT);
	run(300000);
	CHECK(n_emerg_on == 1 && n_aprs_on == 1 && evc[SOS_EV_DMR_SENT] == 1 && sos_active());
	/* Stop: the emergency canceled, the SOS off; nothing asked again */
	sos_stop();
	CHECK(n_emerg_off == 1 && n_aprs_off == 1 && !sos_active());
	CHECK(sos_res(SOS_DMR) == SOS_R_STOPPED && sos_res(SOS_APRS) == SOS_R_STOPPED);
	run(60000);
	CHECK(n_emerg_on == 1 && n_aprs_on == 1);

	/* The gates say no: DMR refused at once, APRS without a position; the events say why, nothing left running */
	reset();
	clear_ev();
	f_emerg_accept = false;
	f_aprs_why = APRS_NO_POSITION;
	sos_mask_set(SOS_APRS | SOS_DMR);
	sos_start(now);
	run(100);
	CHECK(sos_res(SOS_DMR) == SOS_R_NO_DMR && sos_res(SOS_APRS) == SOS_R_NO_POS && !sos_active());
	CHECK(evc[SOS_EV_DMR_NO] == 1 && evc[SOS_EV_APRS_NO_POS] == 1);
	run(60000);
	CHECK(n_emerg_on == 1 && n_aprs_on == 1);	/* not asked again */
	/* APRS not ready (no callsign, not an amateur frequency, no AFSK) */
	reset();
	clear_ev();
	f_aprs_why = APRS_NO_CALL;
	sos_mask_set(SOS_APRS);
	sos_start(now);
	run(100);
	CHECK(sos_res(SOS_APRS) == SOS_R_NO_APRS && evc[SOS_EV_APRS_NO] == 1 && !sos_active());
	/* The emergency accepted but refused by the radio core a moment later (its own gates) */
	reset();
	clear_ev();
	sos_mask_set(SOS_DMR);
	sos_start(now);
	f_emerg_state = DMR_EMERGENCY_FAILED;
	run(SOS_DMR_READ_MS - 100);
	CHECK(sos_res(SOS_DMR) == SOS_R_WAIT);	/* read only once the radio core has had time */
	run(200);
	CHECK(sos_res(SOS_DMR) == SOS_R_NO_DMR && evc[SOS_EV_DMR_NO] == 1 && !sos_active());

	/* An emergency and an APRS SOS that were on before (the user's own, the lone worker's): not asked again, and
	 * Stop leaves them alone */
	reset();
	f_emerg_active = true;
	f_aprs.sos = true;
	f_aprs.sos_sent = 3;
	sos_mask_set(SOS_APRS | SOS_DMR);
	sos_start(now);
	CHECK(n_emerg_on == 0 && n_aprs_on == 0);
	CHECK(sos_res(SOS_DMR) == SOS_R_SENT && sos_res(SOS_APRS) == SOS_R_SENT && !sos_active());
	sos_stop();
	CHECK(n_emerg_off == 0 && n_aprs_off == 0 && f_emerg_active && f_aprs.sos);

	/* Ours turned off elsewhere (Tools > APRS > SOS, the emergency key): no longer ours to stop */
	reset();
	sos_mask_set(SOS_APRS | SOS_DMR);
	sos_start(now);
	f_emerg_active = true;
	run(100);
	f_emerg_active = false;
	f_aprs.sos = false;
	run(100);
	CHECK(!sos_active() && sos_res(SOS_APRS) == SOS_R_STOPPED);
	sos_stop();
	CHECK(n_emerg_off == 0 && n_aprs_off == 0);

#if CONFIG_CW_BEACON
	/* CW first: only the beacon asked; APRS once it is on air, DMR once its message has ended */
	reset();
	clear_ev();
	sos_mask_set(SOS_ALL);
	CHECK(sos_start(now) == 3);
	CHECK(n_cw == 1 && n_aprs_on == 0 && n_emerg_on == 0 && sos_active());
	CHECK(sos_res(SOS_CW) == SOS_R_WAIT && sos_res(SOS_APRS) == SOS_R_QUEUED && sos_res(SOS_DMR) == SOS_R_QUEUED);
	run(100);	/* the radio task has not taken it yet: nothing else */
	CHECK(n_aprs_on == 0 && n_emerg_on == 0 && sos_res(SOS_CW) == SOS_R_WAIT);
	f_cw.on = true;	/* taken */
	run(SOS_CW_TAKE_MS);
	CHECK(n_aprs_on == 0 && n_emerg_on == 0);
	f_cw.keying = true;	/* on air */
	f_cw.sent++;
	run(100);
	CHECK(n_aprs_on == 1 && n_emerg_on == 0 && sos_res(SOS_APRS) == SOS_R_WAIT && sos_res(SOS_DMR) == SOS_R_QUEUED);
	run(10000);	/* the message takes its time */
	CHECK(n_aprs_on == 1 && n_emerg_on == 0 && n_cw == 1);
	f_cw.keying = f_cw.on = false;	/* over */
	run(100);
	CHECK(sos_res(SOS_CW) == SOS_R_SENT && evc[SOS_EV_CW_SENT] == 1);
	CHECK(n_emerg_on == 1 && n_aprs_on == 1 && n_cw == 1 && sos_res(SOS_DMR) == SOS_R_WAIT);
	run(60000);
	CHECK(n_emerg_on == 1 && n_aprs_on == 1 && n_cw == 1);	/* once each */

	/* CW refused (TX inhibit, no callsign, RX only ...): the others still go, at once */
	reset();
	clear_ev();
	sos_mask_set(SOS_CW | SOS_DMR);
	sos_start(now);
	f_cw.refused = 3;	/* never keyed */
	run(SOS_CW_TAKE_MS + 100);
	CHECK(sos_res(SOS_CW) == SOS_R_NO_CW && evc[SOS_EV_CW_NO] == 1 && n_emerg_on == 1);

	/* CW only: no other action asked; done once it has ended */
	reset();
	sos_mask_set(SOS_CW);
	sos_start(now);
	f_cw.on = f_cw.keying = true;
	f_cw.sent++;
	run(1000);
	f_cw.on = f_cw.keying = false;
	run(100);
	CHECK(n_cw == 1 && n_emerg_on == 0 && n_aprs_on == 0 && sos_res(SOS_CW) == SOS_R_SENT && !sos_active());

	/* Stop while the CW keys: cut at once, the queued ones never asked */
	reset();
	sos_mask_set(SOS_CW | SOS_DMR);
	sos_start(now);
	f_cw.on = f_cw.keying = true;
	f_cw.sent++;
	run(1000);
	CHECK(sos_active());
	sos_stop();
	CHECK(n_cw_stop == 1 && sos_res(SOS_CW) == SOS_R_STOPPED && sos_res(SOS_DMR) == SOS_R_STOPPED && !sos_active());
	run(60000);
	CHECK(n_emerg_on == 0 && n_aprs_on == 0 && n_cw == 1);
#endif

	printf("sos: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
