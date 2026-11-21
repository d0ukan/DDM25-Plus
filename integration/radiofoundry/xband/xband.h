/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus cross-band store and forward (CONFIG_XBAND, docs/plus.md "Cross-band store and forward", ROADMAP item
 * 46). The DM-32UV has one transceiver with one synthesizer (docs/r14.md 46): it cannot receive on one band while it
 * transmits on the other, so a real-time cross-band repeater is impossible. This one records a whole over heard on one
 * frequency, then transmits it on the other: A to B and B to A, the receiver going between the two while nothing is
 * heard. Attended operation only (the screen says so before it starts); the radio's screen is locked while it runs,
 * only the stop key works (ui_xband.c).
 *
 * DMR only (owner: DMR first): an over is kept as the 72-bit AMBE+2 air frames the call stack hands its voice sink (9
 * bytes a 20 ms frame) and sent again through the call stack's voice source, so nothing is decoded or encoded again.
 * The memory is the P25 worker's (its voice context, its audio FIFO and the call replay's frames, ~17 KB, about 38 s)
 * lent while the session runs and given back after (p25_lab.c, plus_xband.py). FM (PCM is 16 KB/s; through the AMBE
 * codec it would need the owner's stock codec, which no host test can run) and P25 (its frames would need the very
 * buffers the recording takes, and an IMBE transmit path the P25 worker does not have) are refused (XB_R_MODE).
 *
 * Rules (owner, 2026-11-20): both frequencies in the amateur bands (144-146, 430-440 MHz), never PMR446
 * (446.000-446.200 MHz: no repeaters there, the radio is not type-approved for it), never a repeater input or output
 * (the IARU Region 1 band plan's repeater segments, and a channel whose RX and TX differ), never the satellite
 * segments or the APRS frequency; a callsign (the radio name) for the CW ID, sent on each frequency transmitted on at
 * the first over and then at least every 10 minutes while it relays; at most 3 minutes an over (a longer one is cut,
 * with a beep; the memory cuts it sooner). The portable part (no RTOS): the rules, the state machine, the frame store.
 * Installed as core/radio/xband.{c,h}; the radio core's side is radio_xband.inc. */
#ifndef XBAND_H
#define XBAND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define XB_FRAME_BYTES	9u		/* one 72-bit AMBE+2 air frame */
#define XB_FRAME_MS	20u
#define XB_DWELL_MS	1000u		/* listening: each frequency this long, longer while bursts are heard */
#define XB_HOLD_MS	400u		/* bursts heard this recently hold the frequency */
#define XB_OVER_MAX_MS	180000u		/* owner: an over is cut after 3 minutes (the memory cuts it sooner) */
#define XB_MIN_FRAMES	25u		/* shorter than 0.5 s: a key-up, not relayed */
#define XB_REC_QUIET_MS	1500u		/* recording: no frame and no call heard this long ends the over */
#define XB_ID_EVERY_MS	600000u		/* owner: the callsign in CW at least every 10 minutes */
#define XB_SEND_SLACK_MS 4000u		/* a transmission still on this long after its frames should be out: cut */
#define XB_KEY_WAIT_MS	2000u		/* the transmitter off this long after the retune: keyed now */
#define XB_SEGMENTS	3u
#define XB_CALL_MAX	7u		/* a callsign: radio_cwid_callsign's 4 to 7 characters */

enum { XB_MODE_DMR, XB_MODE_FM, XB_MODE_P25, XB_MODES };

/* Why a frequency or a request is refused (0: allowed) */
enum {
	XB_R_OK,
	XB_R_SAME,		/* A and B are the same frequency */
	XB_R_PMR446,		/* 446.000-446.200 MHz */
	XB_R_BAND,		/* outside 144-146 and 430-440 MHz */
	XB_R_REPEATER,		/* a repeater input or output segment */
	XB_R_SATELLITE,		/* a satellite segment */
	XB_R_APRS,		/* the APRS frequency */
	XB_R_MODE,		/* FM / P25: not built (DMR only) */
	XB_R_CALL,		/* no callsign for the CW ID */
	XB_R_ID,		/* no DMR ID */
	XB_R_OFFSET,		/* a channel whose RX and TX frequencies differ (a repeater channel) */
	XB_R_COUNT
};

/* Phases (xband_view_t.phase) */
enum { XB_P_OFF, XB_P_LISTEN, XB_P_RECORD, XB_P_SEND, XB_P_ID };
/* Why the last session ended (xband_view_t.ended) */
enum { XB_E_NONE, XB_E_STOPPED, XB_E_BATTERY, XB_E_MEMORY, XB_E_REFUSED };
/* Sounds the engine asks for (xband_io_t.beep) */
enum { XB_BEEP_CUT, XB_BEEP_ERROR };

typedef struct {
	uint32_t hz[2];			/* A, B */
	uint8_t mode;			/* XB_MODE_* */
	uint8_t cc, ts;			/* DMR colour code 0-15, slot 1-2 (both sides) */
	uint8_t power;			/* 1 low, 2 mid, 3 high (a power class) */
	char call[XB_CALL_MAX + 1];	/* the CW ID's callsign (radio_cwid_callsign of the radio name) */
} xband_req_t;

typedef struct {
	uint8_t phase;
	uint8_t side;			/* LISTEN / RECORD: the frequency heard; SEND: sent to; ID: identified on */
	uint8_t ended;			/* XB_E_*: why the last session ended */
	bool cut;			/* the over being recorded was cut */
	uint16_t frames;		/* recorded, or left to send */
	uint16_t capacity;		/* frames the memory holds */
	uint8_t relayed, ids;		/* overs sent, CW IDs sent (past 255: from 0 again) */
	uint16_t serial;		/* bumped on every change worth a redraw */
} xband_view_t;

/* --- The rules (any task) --- */
uint8_t xband_check_hz(uint32_t hz);
/* Both frequencies, the pair, the mode, the callsign */
uint8_t xband_check(const xband_req_t *r);
/* A power class (1 low, 2 mid, 3 high) as the board's level */
uint8_t xband_power_level(uint8_t cls, uint8_t power_count);
/* The frames that n bytes hold; their length in ms */
uint16_t xband_frames_for(size_t bytes);

/* --- The UI (any task) --- */
bool xband_on(void);
/* The request: the UI's form edits it while no session runs (the one copy; the radio core reads it during one) */
xband_req_t *xband_req(void);
/* Start what xband_req() holds (false: refused by xband_check, or one runs already). The radio core takes it at its
 * next request */
bool xband_start(void);
void xband_stop(void);
void xband_view(xband_view_t *v);
/* The radio task's request (radio_task.c): arg 1 a session to take, 0 one to end */
void radio_req_xband(bool on);

/* --- The radio core (radio task) --- */
typedef struct {
	void (*tune)(uint8_t side);			/* receive on hz[side] (the session's DMR channel) */
	bool (*key)(uint8_t side, uint32_t dst, bool group);	/* transmit the over on hz[side] to dst (false: refused) */
	void (*unkey)(void);				/* the over is out: let go (the call stack ends its superframe) */
	bool (*cw)(uint8_t side);			/* the CW ID on hz[side] (false: refused) */
	void (*beep)(uint8_t what);			/* XB_BEEP_* */
} xband_io_t;
typedef struct {
	bool rx;			/* the receiver hears a call now */
	bool activity;			/* a burst heard on our slot and colour code within XB_HOLD_MS */
	bool tx;			/* our transmitter is on (an over or the CW ID) */
	bool battery_critical;
} xband_in_t;
/* RREQ_XBAND arg 1: the session to take; false when none waits (then xband_req() is what the core tunes) */
bool xband_begin(uint32_t now);
/* The memory lent for the session (n segments); n 0: none could be had (the session ends, XB_E_MEMORY) */
void xband_memory(uint8_t *const seg[], const size_t len[], uint8_t n, uint32_t now);
/* Every poll while the session runs */
void xband_poll(const xband_io_t *io, const xband_in_t *in, uint32_t now);
/* The radio core is the session's still: false once it ended (then the core gives the memory back and tunes back) */
bool xband_owns_radio(void);
/* The memory is no longer the engine's (the core gives it back) */
void xband_release(void);
/* PTT pressed during a session: refused (the core plays the deny tone) */
void xband_ptt(void);
/* The call stack's voice (dmr_call_poll, the radio task): a call heard and its frames, and the frames to send */
void xband_rx_begin(uint32_t src, uint32_t dst, bool group, bool encrypted);
void xband_rx_frame(const uint8_t frame[XB_FRAME_BYTES]);
void xband_rx_end(void);
bool xband_tx_next(uint8_t frame[XB_FRAME_BYTES]);

#endif
