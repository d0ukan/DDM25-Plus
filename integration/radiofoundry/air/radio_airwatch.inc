// SPDX-License-Identifier: GPL-3.0-or-later
// DDM32 Plus air watch (CONFIG_AIR_WATCH, item 44b, docs/plus.md "Round 14: air band and spectrum"): the radio core's
// part. Included by core/radio/radio_sm.c before radio_sm_request (tools/plus_air.py). Tools > Airfields' "Watch all"
// gives the ring letters' own VFOs (A..F: VFOs 0..5) an airfield's frequencies (RREQ_AIR_VFO), then starts the ring as
// multi-watch does (RREQ_MWATCH): the letters are FM VFOs in the air band, so each is received in AM
// (radio_am_channel) with the AM squelch on its signal strength, and none transmits (TXDENY_AM).
//
// The user's own state is kept where a power cycle keeps it: the first RREQ_AIR_VFO stops a scan or the user's watch
// and notes the position and VFOs A / B in the settings (persist), and nothing more is noted while the air watch runs
// (persist returns at once). RREQ_AIR_END brings VFOs A / B and the position back from there, as the radio's start
// does (radio_sm_init); C..F's VFOs are emptied (RAM only, as after a power off). A power off during the air watch
// therefore comes back as before it.
//
// An ATIS (RREQ_AIR_VFO's idx | RADIO_AIR_ATIS; the UI puts it last) is watched too, but its recording never stops:
// held as any signal, it would keep the one receiver there for good. So its letter holds the ring AIR_ATIS_HOLD_MS at
// most (from the first poll of the hold, through the wait and its looks), then the ring goes on and every ATIS letter
// is passed over (nothing heard there: the ring's quiet stop) for AIR_ATIS_COOL_MS. RAM: the flag, the ATIS bits and
// two times (12 B in radio_sm_t).
enum { AIR_ATIS_HOLD_MS = 5000, AIR_ATIS_COOL_MS = 60000 };

// A VFO for an air frequency: a fresh VFO (FM, so AM in the air band), simplex
static void air_vfo_fill(channel_t *c, uint32_t hz)
{
	vfo_defaults(c);
	c->rx_hz = c->tx_hz = hz;
}

// RREQ_AIR_VFO: VFO idx (0..5) takes hz. What the receiver is tuned to stays (the ring that follows tunes), unless
// that VFO is on view: it is tuned again
static void __attribute__((noinline)) air_vfo(radio_sm_t *sm, uint8_t idx, uint32_t hz, uint32_t now_ms)
{
	bool atis = (idx & RADIO_AIR_ATIS) != 0;
	idx = (uint8_t)(idx & ~RADIO_AIR_ATIS);
	if (idx >= RADIO_DUAL_SLOTS || !radio_am_band(hz) || !board_in_band(hz, false))
		return;
	if (!sm->air) {
		if (scan_on(&sm->scan))
			scan_stop(sm, true, now_ms);	// a scan or the user's watch: back where it started (dual
							// watch: on the selected one), noted as a power off would
		persist(sm, now_ms);			// the position and VFOs A / B, as a power off keeps them
		dual_keep_drop(sm);
		sm->air = true;
		sm->air_atis = 0;
		sm->air_held_ms = 0;
		sm->air_cool_ms = now_ms;		// nothing passed over yet
	}
	if (atis)
		sm->air_atis |= (uint8_t)(1u << idx);
	else
		sm->air_atis &= (uint8_t)~(1u << idx);
	channel_t c;
	if (idx == vfox_in_use(sm)) {
		air_vfo_fill(&sm->vfo_ch, hz);
		sm->vfo_seeded = true;
		sm->vfo_talk_id = 0;
		sm->vfo_talk_private = false;
		vfox_retune(sm, now_ms);		// on view: tuned again (else only kept)
		return;
	}
	air_vfo_fill(&c, hz);
	if (idx >= 2) {
		vfox_slot_put(&sm->vfox_x[idx - 2u], &c, 0, true);
	} else if (sm->vfox_at >= 2 && idx == sm->vfo_idx) {
		vfox_slot_put(&sm->vfox_park, &c, 0, true);	// A / B waiting while a C..F VFO is in use
	} else {
		sm->vfo_other = (radio_vfo_t){ c, true, 0, false };
	}
	changed(sm);
}

// RREQ_AIR_END: the ring stops; VFOs A / B and the position as the settings keep them (radio_sm_init's way), C..F's
// VFOs empty. Nothing without an air watch
static void __attribute__((noinline)) air_end(radio_sm_t *sm, uint32_t now_ms)
{
	if (!sm->air)
		return;
	sm->air = false;
	sm->air_atis = 0;
	if (scan_on(&sm->scan))
		scan_stop(sm, false, now_ms);
	dual_keep_drop(sm);
	vfox_unpark(sm);				// a C..F VFO in use goes: vfo_ch is VFO vfo_idx again
	memset(sm->vfox_x, 0, sizeof sm->vfox_x);	// C..F: empty, as after a power off
	const settings_t *st = settings_begin();	// read only (settings_end below finds no change)
	vfo_defaults(&sm->vfo_ch);			// VFO 1 first, then moved to vfo_other, as radio_sm_init
	sm->vfo_seeded = false;
	sm->vfo_talk_id = 0;
	sm->vfo_talk_private = false;
	restore_vfo(sm, st, 1);
	if (sm->vfo_seeded)
		vfox_load(st, 1, &sm->vfo_ch);
	sm->vfo_other = (radio_vfo_t){ sm->vfo_ch, sm->vfo_seeded, sm->vfo_talk_id, sm->vfo_talk_private };
	vfo_defaults(&sm->vfo_ch);
	sm->vfo_seeded = false;
	sm->vfo_talk_id = 0;
	sm->vfo_talk_private = false;
	restore_vfo(sm, st, 0);
	if (sm->vfo_seeded)
		vfox_load(st, 0, &sm->vfo_ch);
	bool vfo = st->vfo;
	uint8_t idx = st->vfo_index;
	uint16_t zi = st->zone_index, number = st->channel, pos = st->zone_pos;
	settings_end(now_ms);
	sm->vfo_idx = 0;
	sm->view.vfo_index = 0;
	vfo_use(sm, idx);
	select_position(sm, zi, number, pos);
	if (!sm->have_channels || (vfo && VFO_AVAILABLE)) {
		if (VFO_AVAILABLE)
			enter_vfo(sm, now_ms);
	} else {
		tune(sm, now_ms);
	}
	changed(sm);
}

// Every scan poll (scan_service, before the scan engine): the ring on an ATIS letter of the air watch. Passed over
// while it cools (nothing heard: the ring's quiet stop); held AIR_ATIS_HOLD_MS (the hold and the wait after it, looks
// and all): true, the ring goes on and every ATIS letter cools. Any other stop, or a new one, starts the time over
static bool air_atis_poll(radio_sm_t *sm, bool *active, bool *carrier, bool *sync, uint32_t now_ms)
{
	const scan_t *sc = &sm->scan;
	if (!sm->air || sm->air_atis == 0)
		return false;
	const radio_dual_ref_t *r = &sm->dual[sm->dual_at];
	bool atis = dual_running(sm) && r->vfo && r->pos < RADIO_DUAL_SLOTS && ((sm->air_atis >> r->pos) & 1u) != 0;
	bool held = sc->phase == SCAN_PH_HOLD || sc->phase == SCAN_PH_RESUME;
	if (!atis || !held)
		sm->air_held_ms = 0;
	if (!atis)
		return false;
	if ((int32_t)(sm->air_cool_ms - now_ms) > 0) {
		*active = *carrier = *sync = false;	// cooling: passed over
		return false;
	}
	if (!held)
		return false;
	if (sm->air_held_ms == 0) {
		sm->air_held_ms = now_ms | 1u;		// 0 is "not held"
		return false;
	}
	if (now_ms - sm->air_held_ms < AIR_ATIS_HOLD_MS)
		return false;
	sm->air_held_ms = 0;
	sm->air_cool_ms = now_ms + AIR_ATIS_COOL_MS;
	return true;
}

// The requests (radio_req_air_vfo / radio_req_air_end): refused, as RREQ_MWATCH is, while transmitting or in hotspot
// mode
static void air_request(radio_sm_t *sm, const radio_req_t *req, uint32_t now_ms)
{
	if (sm->view.state == RS_TX || sm->hotspot || sm->ptt)
		return;
	if (req->type == RREQ_AIR_VFO)
		air_vfo(sm, (uint8_t)req->arg, req->arg2, now_ms);
	else
		air_end(sm, now_ms);
}
