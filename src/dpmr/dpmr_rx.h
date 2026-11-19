/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus dPMR voice receive (docs/dpmr.md): frame sync on the 4FSK dibits, the header (HI), the payload frames'
 * control channel (CCH: frame number, the IDs in quarters, communication mode), the channel / colour code filter, the
 * 72-bit voice blocks of admitted calls to a sink (the DMR AMBE+2 air frame), the end frame. Ours.
 *
 * Sync: in hunt, FS1 (at most 3 wrong symbol signs of 24 and 8 wrong bits of 48) starts a header, FS2 (at most 2
 * wrong bits of 24) a payload frame; both polarities (the FM RX RAM's sign on the DM-32 is unmeasured; an inverted
 * FS1 is FS4, the packet data header's, which the header's CRC tells apart). A payload frame found by the hunt is
 * tentative: its CCH is held and its voice not played until the next frame's sync field (FS2 or a colour code, within
 * 2 / 1 bits) follows 192 symbols later. Once synced every frame's sync field is checked where it must be (FS2 / FS3
 * after frames 2 and 4, a colour code after frames 1 and 3, as the CCH's frame number says) on the symbols' signs;
 * three misses in a row are ridden over, the fourth loses sync. FS3 ends the call. A new FS1 anywhere restarts on a
 * header (back to back transmissions).
 *
 * A call starts with a communication start header, or by late entry with a voice CCH. Its IDs come from the header,
 * or from the four CCH quarters of a superframe. Voice plays when the mode is voice (000, 001, 101; in 101 not the
 * frames the slow data marks as data) and the colour code is the channel's (cfg.cc 0 = any, else code + 1); a
 * foreign colour code ends the call. Data calls are reported, never played.
 *
 * The demodulator is NXDN48's (src/nxdn/nxdn_demod.h): the same 2400 symbols/s 4FSK, deviation, symbol mapping and
 * root raised cosine 0.2 x x/sin(x) receive filter (TS 102 490 12.2.2). No allocation, no RTOS, no hardware. */
#ifndef DPMR_RX_H
#define DPMR_RX_H

#include "dpmr.h"
#include "nxdn/nxdn_demod.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { DPMR_RX_HUNT, DPMR_RX_HEADER, DPMR_RX_SYNC, DPMR_RX_CCH, DPMR_RX_TCH, DPMR_RX_END };
/* Events: the call (m set: started, or its IDs / mode / colour code changed), the call ended (m NULL) */
enum { DPMR_EV_CALL = 1, DPMR_EV_END = 2 };

typedef struct {
	uint32_t called, own;		/* 24-bit air interface IDs, valid with have_called / have_own */
	bool have_called, have_own;
	uint8_t mode, format, flags;	/* dpmr.h: dpmr_mode_voice(), dpmr_all_call(), dpmr_emergency() */
	uint8_t cc;			/* the colour code heard (0..63), DPMR_CC_NONE before one is */
} dpmr_call_t;

typedef struct {
	uint8_t cc;		/* 0 hears every colour code, else code number + 1 (1..64) */
	/* one 72-bit voice block (the DMR AMBE+2 air frame) in air order, from dpmr_rx_push_dibit */
	void (*voice)(void *ctx, const uint8_t slot[DPMR_SLOT_BYTES]);
	void (*event)(void *ctx, unsigned ev, const dpmr_call_t *c);
	void *ctx;
} dpmr_rx_cfg_t;

typedef struct {
	dpmr_rx_cfg_t cfg;
	uint64_t shift;		/* the last 24 dibits */
	uint8_t buf[33];	/* the field being collected, air bits (a header's HI0 CC HI1 is the longest) */
	uint16_t pos;		/* dibits of it in */
	uint8_t state, slot, fn_next, misses, sync_errors;
	uint8_t hold;		/* dibits the demodulator keeps its acquisition after a preamble */
	bool inverted, tentative, hunt_header, pending;
	dpmr_cch_t held;	/* a tentative frame's CCH (pending), taken once the next sync field confirms it */
	/* the call */
	bool in_call, admitted, data_frame, playing;
	dpmr_call_t call;
	uint16_t quarter[4];	/* CCH ID quarters by frame number */
	uint8_t quarters;	/* bit k: quarter k is in */
	/* counters (frames: with a CCH that decoded, or in a call) */
	uint32_t frames, headers, data_headers, bad_hi, bad_cch, foreign_cc, calls, late_entries, ends, end_frames;
	uint32_t false_syncs, sync_losses, slots_played, slots_muted;
} dpmr_rx_t;

void dpmr_rx_init(dpmr_rx_t *rx, const dpmr_rx_cfg_t *cfg);
/* One dibit in air order (0..3 as in dpmr.h) */
void dpmr_rx_push_dibit(dpmr_rx_t *rx, uint8_t dibit);
/* Carrier lost or retuned: the call ends (DPMR_EV_END if one was on), sync is hunted again */
void dpmr_rx_reset(dpmr_rx_t *rx);

/* The demodulator and the frame layer together: 8 kHz discriminator samples in */
typedef struct {
	nxdn_demod_t demod;
	dpmr_rx_t rx;
	uint32_t samples;
} dpmr_receiver_t;

/* hpf_hz / deemph_us: the receive audio path's measured shape (0 = flat; nxdn_demod.h) */
void dpmr_receiver_init(dpmr_receiver_t *r, const dpmr_rx_cfg_t *cfg, uint16_t hpf_hz, uint16_t deemph_us);
void dpmr_receiver_audio(dpmr_receiver_t *r, const int16_t *pcm, size_t n);
/* Retuned, or the audio had a gap: acquire again */
void dpmr_receiver_restart(dpmr_receiver_t *r);

#ifdef __cplusplus
}
#endif
#endif
