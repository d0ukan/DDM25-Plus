/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM25 Plus one-key SOS (CONFIG_SOS_KEY, docs/plus.md): a side key set to "SOS" (Settings > Keys; not assigned by
 * default) opens a confirmation page listing the actions chosen in Tools > SOS; OK there (or a long press of the SOS
 * key) starts them, Back cancels. Nothing transmits before that confirmation, and nothing at all while no action is
 * chosen. The choice is RAM only: every action is off after power on (no settings bit is taken).
 *
 * The actions are the existing ones, each with all its own gates (as the lone worker's):
 *   - CW SOS: the CW beacon's "SOS SOS SOS DE <call>" keyed once on the channel (radio_req_cw_beacon: TX inhibit,
 *     band, RX only, battery, a callsign as the radio name; not on a P25 channel, the hotspot or an SFR repeater).
 *   - APRS SOS: aprs_sos_set(true), Tools > APRS > SOS (the Mic-E emergency position now and every 2 minutes;
 *     callsign, an amateur APRS frequency, AFSK, a position, CSMA).
 *   - DMR emergency: ui_svc_emergency(true), the emergency key's call (a DMR channel we may transmit on with an ID;
 *     the alarm, then per upstream's emergency settings a hot mic call).
 * Order: the CW beacon is cut by any other radio request and keys the channel out of DMR, so it goes first; the APRS
 * SOS is turned on once the CW is on air (the APRS engine waits for the channel to be free), the DMR emergency once
 * the CW message has ended. Without CW both start at once. The SOS key again while one is running offers "Stop SOS":
 * the CW cut, the emergency canceled, the APRS SOS turned off (what went out stays sent); an emergency or an APRS SOS
 * that was on before (the user's own, the lone worker's) is left alone.
 *
 * Pure state machine on the UI task (sos.c); ui_sos.c draws the pages and turns the events into toasts. */
#ifndef PLUS_SOS_H
#define PLUS_SOS_H

#include <stdbool.h>
#include <stdint.h>
#include "hal_input.h"

/* The actions (a mask) */
#define SOS_APRS	0x01u
#define SOS_DMR		0x02u
#define SOS_CW		0x04u
#define SOS_ALL		0x07u

#define SOS_CW_TAKE_MS	500u	/* the radio task has taken the beacon request and keyed (or refused) by then */
#define SOS_DMR_READ_MS	1000u	/* a refused emergency start shows in dmr_emergency_state() by then */

/* What became of an action */
typedef enum {
	SOS_R_NONE,		/* not chosen, or not started */
	SOS_R_QUEUED,		/* chosen, waits for the CW message */
	SOS_R_WAIT,		/* asked, not sent yet */
	SOS_R_SENT,		/* on air (the emergency started, an SOS position or the CW message sent) */
	SOS_R_NO_DMR,		/* the emergency's gates: not a DMR channel we may transmit on, no ID */
	SOS_R_NO_POS,		/* APRS: no position */
	SOS_R_NO_APRS,		/* APRS: no callsign, no amateur APRS frequency, no AFSK */
	SOS_R_NO_CW,		/* CW: refused (the channel may not transmit, no callsign, the radio busy) */
	SOS_R_STOPPED,		/* Stop SOS */
} sos_res_t;

/* Events for the UI's toasts */
typedef enum {
	SOS_EV_NONE, SOS_EV_CW_SENT, SOS_EV_CW_NO, SOS_EV_DMR_SENT, SOS_EV_DMR_NO, SOS_EV_APRS_NO_POS,
	SOS_EV_APRS_NO,
} sos_ev_t;

uint8_t sos_mask(void);
void sos_mask_set(uint8_t mask);		/* only what this build has (sos_built) */
uint8_t sos_built(void);			/* the actions this build has */
/* Start what is chosen (the confirmation was given): the number of actions started (0: none chosen, nothing sent) */
uint8_t sos_start(uint32_t now_ms);
/* Stop what this SOS started */
void sos_stop(void);
/* Something of ours still running or waiting: the SOS key offers Stop */
bool sos_active(void);
/* Every UI tick: what happened since (one event a call) */
sos_ev_t sos_tick(uint32_t now_ms);
sos_res_t sos_res(uint8_t action);		/* SOS_APRS / SOS_DMR / SOS_CW */

/* --- ui_sos.c (the UI task) --- */
/* An input event before the key lock (ui_app.c, as the emergency key): the SOS key (ACT_SOS) opens the confirmation
 * page, or Stop SOS while one runs; its long press on that page confirms. True: used */
bool ui_sos_key(const input_event_t *ev);
/* Every UI tick: sos_tick and its toasts; true: draw again */
bool ui_sos_tick(uint32_t now_ms);

#endif
