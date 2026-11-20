/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus one-key SOS (sos.h): the actions' state machine on the UI task. It only calls what the lone worker,
 * Tools > APRS > SOS, Tools > CW beacon and the emergency key call; every gate is theirs. Installed as core/ui/sos.c.
 * RAM: 16 B. */
#include "config.h"
#if CONFIG_SOS_KEY
#include "ui/sos.h"
#if CONFIG_DMR_EMERGENCY
#include "ui_svc.h"
#include "dmr/dmr_emergency.h"
#endif
#if CONFIG_APRS_MSG
#include "aprs/aprs.h"
#include "aprs/aprs_plus.h"
#endif
#if CONFIG_CW_BEACON
#include "radio/cw_beacon.h"
#endif

static uint8_t mask;			/* chosen in Tools > SOS (RAM only: none after power on) */
static uint8_t r_cw, r_dmr, r_aprs;	/* sos_res_t */
static uint8_t ours;			/* what this SOS started and Stop ends */
static uint8_t evs;			/* events not yet taken: bit (sos_ev_t - 1) */
#if CONFIG_CW_BEACON
static uint8_t cw_sent;			/* the beacon's message count when ours was asked for */
static uint32_t t_cw;
#endif
#if CONFIG_DMR_EMERGENCY
static uint32_t t_dmr;
#endif

uint8_t sos_built(void)
{
	return (uint8_t)((CONFIG_APRS_MSG ? SOS_APRS : 0u) | (CONFIG_DMR_EMERGENCY ? SOS_DMR : 0u) |
			 (CONFIG_CW_BEACON ? SOS_CW : 0u));
}

uint8_t sos_mask(void)
{
	return mask;
}

void sos_mask_set(uint8_t m)
{
	mask = (uint8_t)(m & sos_built());
}

static void post(sos_ev_t ev)
{
	evs = (uint8_t)(evs | (1u << (ev - 1u)));
}

static void start_aprs(void)
{
#if CONFIG_APRS_MSG
	if (r_aprs != SOS_R_QUEUED)
		return;
	aprs_plus_t s;
	aprs_plus_state(&s);
	if (s.sos) {
		r_aprs = s.sos_sent ? SOS_R_SENT : SOS_R_WAIT;	/* on already (the user's own): not ours to stop */
		return;
	}
	uint8_t why = aprs_sos_set(true);
	if (why == APRS_OK) {
		r_aprs = SOS_R_WAIT;
		ours |= SOS_APRS;
	} else {
		r_aprs = why == APRS_NO_POSITION ? SOS_R_NO_POS : SOS_R_NO_APRS;
		post(why == APRS_NO_POSITION ? SOS_EV_APRS_NO_POS : SOS_EV_APRS_NO);
	}
#endif
}

static void start_dmr(uint32_t now_ms)
{
#if CONFIG_DMR_EMERGENCY
	if (r_dmr != SOS_R_QUEUED)
		return;
	if (ui_svc_emergency_active()) {
		r_dmr = SOS_R_SENT;	/* in emergency already (the user's own, the lone worker's): not ours */
		return;
	}
	if (ui_svc_emergency(true)) {
		r_dmr = SOS_R_WAIT;
		ours |= SOS_DMR;
		t_dmr = now_ms;
	} else {
		r_dmr = SOS_R_NO_DMR;
		post(SOS_EV_DMR_NO);
	}
#else
	(void)now_ms;
#endif
}

uint8_t sos_start(uint32_t now_ms)
{
	uint8_t m = (uint8_t)(mask & sos_built()), n = 0;
	r_cw = r_dmr = r_aprs = SOS_R_NONE;
	ours = evs = 0;
	if (m & SOS_DMR) {
		r_dmr = SOS_R_QUEUED;
		n++;
	}
	if (m & SOS_APRS) {
		r_aprs = SOS_R_QUEUED;
		n++;
	}
#if CONFIG_CW_BEACON
	if (m & SOS_CW) {
		cw_beacon_view_t v;
		cw_beacon_view(&v);
		cw_sent = (uint8_t)v.sent;
		t_cw = now_ms;
		radio_req_cw_beacon(CW_BEACON_SOS, 0);	/* interval 0: once */
		r_cw = SOS_R_WAIT;
		ours |= SOS_CW;
		return (uint8_t)(n + 1u);	/* the others follow it (sos_tick) */
	}
#endif
	start_aprs();
	start_dmr(now_ms);
	return n;
}

void sos_stop(void)
{
#if CONFIG_CW_BEACON
	if (r_cw == SOS_R_WAIT) {
		radio_req_cw_beacon(CW_BEACON_KINDS, 0);	/* a message on air is cut at once */
		r_cw = SOS_R_STOPPED;
	}
#endif
	if (r_dmr == SOS_R_QUEUED)
		r_dmr = SOS_R_STOPPED;
	if (r_aprs == SOS_R_QUEUED)
		r_aprs = SOS_R_STOPPED;
#if CONFIG_DMR_EMERGENCY
	if (ours & SOS_DMR) {
		ui_svc_emergency(false);	/* the emergency key's cancel */
		r_dmr = SOS_R_STOPPED;
	}
#endif
#if CONFIG_APRS_MSG
	if (ours & SOS_APRS) {
		aprs_sos_set(false);
		r_aprs = SOS_R_STOPPED;
	}
#endif
	ours = 0;
}

bool sos_active(void)
{
	return r_cw == SOS_R_WAIT || r_dmr == SOS_R_QUEUED || r_aprs == SOS_R_QUEUED || ours != 0;
}

sos_res_t sos_res(uint8_t action)
{
	return (sos_res_t)(action == SOS_CW ? r_cw : action == SOS_DMR ? r_dmr : action == SOS_APRS ? r_aprs : SOS_R_NONE);
}

sos_ev_t sos_tick(uint32_t now_ms)
{
#if CONFIG_CW_BEACON
	if (r_cw == SOS_R_WAIT) {
		cw_beacon_view_t v;
		cw_beacon_view(&v);
		if (v.keying)
			start_aprs();	/* on air: the APRS engine waits for the channel to be free */
		if (now_ms - t_cw >= SOS_CW_TAKE_MS && !v.on && !v.keying) {
			bool sent = (uint8_t)v.sent != cw_sent;
			r_cw = sent ? SOS_R_SENT : SOS_R_NO_CW;
			post(sent ? SOS_EV_CW_SENT : SOS_EV_CW_NO);
			ours &= (uint8_t)~SOS_CW;
			start_aprs();
			start_dmr(now_ms);
		}
	}
#endif
#if CONFIG_DMR_EMERGENCY
	if (ours & SOS_DMR) {
		bool on = ui_svc_emergency_active();
		if (r_dmr == SOS_R_WAIT && on) {
			r_dmr = SOS_R_SENT;
			post(SOS_EV_DMR_SENT);
		} else if (r_dmr == SOS_R_WAIT && now_ms - t_dmr >= SOS_DMR_READ_MS &&
			   dmr_emergency_state() == DMR_EMERGENCY_FAILED) {
			r_dmr = SOS_R_NO_DMR;
			ours &= (uint8_t)~SOS_DMR;
			post(SOS_EV_DMR_NO);
		} else if (r_dmr == SOS_R_SENT && !on) {
			ours &= (uint8_t)~SOS_DMR;	/* canceled elsewhere (the emergency key, the debug link) */
		}
	}
#endif
#if CONFIG_APRS_MSG
	if (ours & SOS_APRS) {
		aprs_plus_t s;
		aprs_plus_state(&s);
		if (!s.sos) {
			r_aprs = SOS_R_STOPPED;	/* turned off elsewhere (Tools > APRS > SOS) */
			ours &= (uint8_t)~SOS_APRS;
		} else if (s.sos_sent != 0) {
			r_aprs = SOS_R_SENT;	/* ui_aprsmsg.c toasts each one */
		}
	}
#endif
	(void)now_ms;
	for (uint8_t b = 0; b < 8u; b++) {
		if (evs & (1u << b)) {
			evs = (uint8_t)(evs & ~(1u << b));
			return (sos_ev_t)(b + 1u);
		}
	}
	return SOS_EV_NONE;
}
#endif
