/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus DOTA Transfer: the transport-independent protocol (docs/dota.md 6 and 7). Frames (header, the sender's
 * callsign where the transport doesn't carry it, payload, CRC-16), the sender's windows with selective resends and the
 * receiver's answers (poll and response). No radio, no codeplug, no RTOS: the radio core feeds it frames and time,
 * the record layer fills and takes payloads, the host tests drive both ends through a lossy fake link.
 * Installed as core/radio/dota_proto.h. */
#ifndef DOTA_PROTO_H
#define DOTA_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DOTA_VERSION		1u	/* protocol */
#define DOTA_REC_VERSION	1u	/* record format (dota_rec.h) */
#define DOTA_LAYOUT		1u	/* codeplug layout: the open-firmware image, 80-channel zones */
#define DOTA_HDR		12u
#define DOTA_CALL		6u
#define DOTA_PAYLOAD_MAX	192u
#define DOTA_FRAME_MAX		(DOTA_HDR + DOTA_CALL + DOTA_PAYLOAD_MAX + 2u)
#define DOTA_SPAN		32u	/* frame numbers in flight from the receiver's base (the answer's bitmap) */
#define DOTA_WINDOW_MAX		7u
#define DOTA_FRAMES_MAX		512u	/* data frames in one transfer */
#define DOTA_FW_MAX		24u

enum { DOTA_T_HELLO = 1, DOTA_T_REFUSE, DOTA_T_DATA, DOTA_T_ACK, DOTA_T_END, DOTA_T_END_ACK, DOTA_T_ABORT, DOTA_T_POLL };
/* Sections, in the order they are sent: a frame of one is taken only when every frame of the ones before it is in */
enum { DOTA_SEC_NONE, DOTA_SEC_CONTACTS, DOTA_SEC_LISTS, DOTA_SEC_CHANNELS, DOTA_SEC_ZONES, DOTA_SEC_COUNT };
#define DOTA_F_POLL		0x01u	/* answer after this frame */
#define DOTA_F_CALL		0x02u	/* the sender's callsign follows the header */

/* Refusals (DOTA_T_REFUSE status) */
enum { DOTA_R_OK, DOTA_R_VERSION, DOTA_R_MODE, DOTA_R_CHANNELS, DOTA_R_ZONES, DOTA_R_CONTACTS, DOTA_R_ZONE_FULL,
       DOTA_R_NOT_READY, DOTA_R_LISTS };
/* END_ACK results */
enum { DOTA_E_DONE, DOTA_E_MISSING, DOTA_E_CRC, DOTA_E_FLASH, DOTA_E_WRITING };
/* ABORT reasons */
enum { DOTA_A_USER = 1, DOTA_A_BATTERY, DOTA_A_CODEPLUG, DOTA_A_TIMEOUT };

typedef struct {
	uint8_t type;
	uint8_t flags;		/* DOTA_F_* */
	uint8_t sec;		/* DATA: its section */
	uint8_t left;		/* frames after this one in its window (0-7) */
	uint16_t code;		/* the receiver's pairing code, 0-9999 */
	uint16_t session;
	uint16_t seq;		/* DATA: 1..N */
	uint16_t first;		/* DATA: the first record's index in its section */
	uint8_t count;		/* DATA: records */
	uint8_t len;		/* payload bytes */
	char call[DOTA_CALL + 1];	/* with DOTA_F_CALL */
	const uint8_t *payload;
} dota_frame_t;

/* The frame's bytes: header, callsign (DOTA_F_CALL), payload, CRC-16 (X.25, low byte first). 0 when max is short */
size_t dota_frame_put(const dota_frame_t *f, uint8_t *out, size_t max);
/* Bytes to a frame (payload points into in); false for any version, length or CRC fault */
bool dota_frame_get(const uint8_t *in, size_t n, dota_frame_t *f);
/* The order-free transfer check: CRC-32 of a DATA frame's numbered part (seq, first, count, len, payload) */
uint32_t dota_frame_crc(uint16_t seq, uint16_t first, uint8_t count, const uint8_t *payload, uint8_t len);

/* --- The manifest (HELLO's payload, docs/dota.md 6.1) --- */
typedef struct {
	char model[5];
	uint8_t rec_ver, layout, modes, kind;
	uint8_t zones;				/* zones (DOTA Clone; the zones section counts their parts) */
	uint16_t recs[DOTA_SEC_COUNT];		/* records per section ([0] unused) */
	uint16_t frames[DOTA_SEC_COUNT];	/* frames per section ([0] unused) */
	uint32_t bytes;
	char fw[DOTA_FW_MAX + 1];
} dota_manifest_t;
#define DOTA_MANIFEST_MAX	(29u + DOTA_FW_MAX)
size_t dota_manifest_put(const dota_manifest_t *m, uint8_t *out, size_t max);
bool dota_manifest_get(const uint8_t *in, size_t n, dota_manifest_t *m);
uint16_t dota_manifest_total(const dota_manifest_t *m);	/* data frames N */

/* --- Link parameters (the transport's) --- */
typedef struct {
	uint8_t window;		/* frames per window, <= DOTA_WINDOW_MAX */
	uint8_t frame_extra;	/* bytes the transport adds to each frame (window_bytes counts them) */
	uint16_t payload_max;	/* <= DOTA_PAYLOAD_MAX */
	uint16_t window_bytes;	/* the most bytes in one window (the key time limit), 0 = no limit */
	uint16_t turn_ms;	/* the answering side waits this long after the other's window */
	uint16_t frame_ms;	/* the longest frame's air time: the answer time when the POLL frame is lost */
	uint16_t ack_ms;	/* the sender's wait for an answer from the end of its window */
	uint16_t end_gap_ms;	/* the sender's wait before asking again while the receiver writes */
	uint32_t idle_ms;	/* the receiver gives up after this long without a frame of its session */
	uint32_t rest_after_ms;	/* FM: after this much key time the sender rests rest_ms (0: never) */
	uint16_t rest_ms;
	uint8_t polls;		/* POLLs (or HELLOs) after a missing answer before giving up */
	uint8_t tries;		/* sends of one frame before giving up */
	uint8_t end_tries;
	bool call;		/* frames carry the callsign (DMR); FM: the AX.25 source does */
	char me[DOTA_CALL + 1];	/* our callsign */
} dota_link_t;

/* --- The sender --- */

/* Fill a DATA payload with records of section sec from index first: *count records (0: as many as fit, set on
 * return). Returns the payload bytes, 0 when the records are gone (the codeplug changed) */
typedef size_t (*dota_fill_fn)(void *ctx, uint8_t sec, uint16_t first, uint8_t *count, uint8_t *out, size_t max);

typedef struct {
	uint16_t seq, first;
	uint8_t sec, count;
	uint8_t tries;
	bool sent;
} dota_slot_t;

enum { DOTA_TX_IDLE, DOTA_TX_READY, DOTA_TX_SENDING, DOTA_TX_WAIT, DOTA_TX_REST, DOTA_TX_DONE, DOTA_TX_FAILED };
/* Why it failed (dota_tx_t.fail) */
enum { DOTA_TF_NONE, DOTA_TF_NO_ANSWER, DOTA_TF_REFUSED, DOTA_TF_STOPPED, DOTA_TF_RX_STOPPED, DOTA_TF_FRAME,
       DOTA_TF_RESULT, DOTA_TF_SOURCE,
       DOTA_TF_UNSURE };	/* every frame delivered, no final answer: the receiver may have written it all */

#define DOTA_WIN_HELLO	0xFFFFu	/* window entries that aren't DATA frames */
#define DOTA_WIN_END	0xFFFEu
#define DOTA_WIN_POLL	0xFFFDu

typedef struct {
	const dota_link_t *link;
	dota_fill_fn fill;
	void *ctx;
	dota_manifest_t m;
	uint16_t code, session;
	uint16_t total;			/* N */
	uint8_t state, fail;
	uint8_t polls, end_tries;
	bool hello_ok;
	bool end_sent;			/* END has gone out: a failure from here on can't say "nothing changed" */
	uint8_t cur_sec;		/* the next new frame's records */
	uint16_t cur_first;
	uint16_t next_seq;		/* the next new frame's number */
	uint16_t acked;			/* frames acknowledged */
	uint32_t crc;			/* the transfer CRC-32 sum */
	dota_slot_t fly[DOTA_SPAN];	/* sent, not acknowledged */
	uint8_t nfly;
	uint16_t win[DOTA_WINDOW_MAX + 2];	/* this window: frame numbers or DOTA_WIN_* */
	uint8_t nwin;
	uint32_t t_ms;			/* the window's end, or the rest's start */
	uint32_t keyed_ms;		/* key time since the last rest */
	uint16_t resent;
	/* The last answer */
	uint8_t status;			/* a refusal's DOTA_R_*, END_ACK's DOTA_E_* */
	uint16_t free_ch;
	uint8_t free_zones, zone_room;
	uint16_t ch_added, ct_added, ct_matched;
	uint8_t zones_added, lists_added;
	char peer_fw[DOTA_FW_MAX + 1];
	char peer[DOTA_CALL + 1];	/* the receiver's callsign */
} dota_tx_t;

void dota_tx_start(dota_tx_t *s, const dota_link_t *link, const dota_manifest_t *m, uint16_t code, uint16_t session,
		   dota_fill_fn fill, void *ctx);
/* A window to send now? Composes it (scratch: DOTA_PAYLOAD_MAX bytes for the new frames' packing) and returns its
 * frame count, else 0 */
uint8_t dota_tx_window(dota_tx_t *s, uint32_t now, uint8_t *scratch);
/* Frame i of the window as bytes (out: DOTA_FRAME_MAX), 0 on a source fault (the transfer then fails) */
size_t dota_tx_frame(dota_tx_t *s, uint8_t i, uint8_t *out, size_t max);
/* The window went out (keyed_ms: how long the transmitter was keyed) */
void dota_tx_sent(dota_tx_t *s, uint32_t keyed_ms, uint32_t now);
/* An answer heard (from = the transport's own check, e.g. the DMR source ID, already done) */
void dota_tx_heard(dota_tx_t *s, const uint8_t *in, size_t n, const char *peer, uint32_t now);
/* Timeouts */
void dota_tx_tick(dota_tx_t *s, uint32_t now);
/* The user stops: an ABORT to send (out), the transfer ends */
size_t dota_tx_stop(dota_tx_t *s, uint8_t reason, uint8_t *out, size_t max);
/* Data frames acknowledged, 0..total */
static inline uint16_t dota_tx_done_frames(const dota_tx_t *s) { return s->acked; }

/* --- The receiver --- */

/* HELLO with our code: the manifest checked (versions, capacity). DOTA_R_OK accepts; the rest refuse (the answer
 * carries free_ch, free_zones, zone_room and our firmware, set by the callee in the receiver) */
typedef uint8_t (*dota_hello_fn)(void *ctx, const dota_manifest_t *m, const char *peer);
/* A DATA frame to keep: true when taken (queued for writing), false when there is no room now (not acknowledged) */
typedef bool (*dota_data_fn)(void *ctx, uint8_t sec, uint16_t first, uint8_t count, const uint8_t *payload, uint8_t len);
/* Every frame is in and checks: write everything now; dota_rx_commit_done when finished */
typedef void (*dota_commit_fn)(void *ctx);

enum { DOTA_RX_IDLE, DOTA_RX_ARMED, DOTA_RX_RECEIVING, DOTA_RX_COMMIT, DOTA_RX_DONE, DOTA_RX_FAILED };
enum { DOTA_RF_NONE, DOTA_RF_IDLE, DOTA_RF_STOPPED, DOTA_RF_TX_STOPPED, DOTA_RF_CRC, DOTA_RF_FLASH };

typedef struct {
	const dota_link_t *link;
	dota_hello_fn hello;
	dota_data_fn data;
	dota_commit_fn commit;
	void *ctx;
	uint16_t code, session;
	uint8_t state, fail;
	dota_manifest_t m;
	uint16_t total;
	uint16_t base;			/* the lowest frame not in */
	uint16_t got_n;
	uint8_t got[DOTA_FRAMES_MAX / 8u];
	uint32_t crc;
	bool slow;			/* a frame found no room: say so */
	uint8_t answer;			/* the answer due: DOTA_T_* or 0 */
	uint32_t answer_at;
	uint32_t last_ms;		/* the last frame of our session */
	uint8_t refused_status;		/* the last refusal's DOTA_R_* */
	uint8_t result;			/* END_ACK's */
	char peer[DOTA_CALL + 1];
	char fw[DOTA_FW_MAX + 1];	/* ours, for refusals */
	uint16_t free_ch;		/* ours, for refusals (the hello callback sets them) */
	uint8_t free_zones, zone_room;
	uint16_t ch_added, ct_added, ct_matched;	/* END_ACK's counts (set before dota_rx_commit_done) */
	uint8_t zones_added, lists_added;
} dota_rx_t;

void dota_rx_arm(dota_rx_t *r, const dota_link_t *link, uint16_t code, dota_hello_fn hello, dota_data_fn data,
		 dota_commit_fn commit, void *ctx, uint32_t now);
/* A frame heard (peer: the transport's sender name, the AX.25 source on FM; NULL when the frame carries it) */
void dota_rx_heard(dota_rx_t *r, const uint8_t *in, size_t n, const char *peer, uint32_t now);
/* The answer to send now, if one is due and the channel is clear: its bytes, else 0 */
size_t dota_rx_answer(dota_rx_t *r, uint32_t now, bool clear, uint8_t *out, size_t max);
void dota_rx_tick(dota_rx_t *r, uint32_t now);
void dota_rx_commit_done(dota_rx_t *r, bool ok);
size_t dota_rx_stop(dota_rx_t *r, uint8_t reason, uint8_t *out, size_t max);
/* 0..100 */
uint8_t dota_rx_percent(const dota_rx_t *r);

#endif
