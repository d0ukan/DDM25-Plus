/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus NXDN48 conventional voice receive (docs/nxdn.md): frame sync on the demodulator's dibits, LICH, the RAN
 * filter, the caller from the VCALL (FACCH1 in the header, or the four SACCH parts of a superframe for late entry),
 * and the 72-bit AMBE+2 voice fields of admitted calls, four every 80 ms, to a sink. TX_REL or lost sync ends a call.
 * Encrypted calls (cipher type != 0) are reported and never played; nothing is decrypted.
 *
 * Ported from DM32-nxdn-p25 src/nxdn/nxdn_call.{c,h} and nxdn_link.c (GPL-3.0-or-later, "lyra XE1POC"), receive
 * half only: the transmitter, the TX ring and the pacing queue are left out (DM32 Plus hands the four fields of a
 * frame to RadioFoundry's ambeVoiceSink at once, as its YSF receiver does with five). The FSW search in both
 * polarities, tentative sync, LICH plausibility, flywheel and late entry are theirs. MMDVMHost (G4KLX,
 * GPL-2.0-or-later) NXDNControl.cpp is the reference for the frame handling.
 *
 * RAM on the CK803S (csky-elf-gcc -Os): sizeof(nxdn_rx_t) 356 bytes (the Viterbi survivors 192 of them),
 * nxdn_receiver_t 576 (test_nxdn prints the host's). Stack (-fstack-usage): about 450 bytes below nxdn_receiver_audio (the
 * Viterbi 200), plus what the callbacks use. No allocation,
 * no RTOS, no hardware. */
#ifndef NXDN_RX_H
#define NXDN_RX_H

#include "nxdn.h"
#include "nxdn_demod.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { NXDN_RX_HUNT, NXDN_RX_SYNCED };
/* Events: a call's caller is known or changed (m set), the call ended (m NULL) */
enum { NXDN_EV_CALL = 1, NXDN_EV_END = 2 };

typedef struct {
	uint8_t ran;		/* 0 hears every RAN */
	uint16_t group;		/* 0 hears every call (group, broadcast and individual) */
	uint16_t unit;		/* with a group set: individual calls to this unit ID play too (0 = none) */
	/* one 72-bit voice field (the DMR AMBE+2 air frame), in air order; called from nxdn_rx_push_dibit */
	void (*voice)(void *ctx, const uint8_t slot[NXDN_SLOT_BYTES]);
	void (*event)(void *ctx, unsigned ev, const nxdn_call_msg_t *m);
	void *ctx;
} nxdn_rx_cfg_t;

typedef struct {
	nxdn_rx_cfg_t cfg;
	/* sync */
	uint32_t shift;		/* the last 10 dibits */
	uint16_t fsw_hits;	/* bit k: a frame sync word ended k dibits ago */
	uint16_t fsw_inverted;	/* bit k: ... with every symbol's sign inverted */
	uint8_t frame[NXDN_FRAME_BYTES];
	uint16_t pos;		/* bits collected after the FSW start */
	uint8_t state, misses;
	bool tentative;		/* found by the hunt, not yet confirmed by the next frame's FSW */
	bool inverted;		/* the stream's polarity is inverted: dibits are flipped back */
	uint8_t sync_errors;	/* bit errors of the FSW a tentative sync started on */
	/* the call */
	bool in_call, have_msg, admitted, playing;
	nxdn_call_msg_t msg;
	uint8_t parts[4][3];	/* SACCH superframe parts, 18 bits each */
	uint8_t part_mask;
	uint8_t last_ran, last_lich;
	uint16_t surv[NXDN_VIT_STEPS];	/* Viterbi survivors (scratch) */
	/* counters */
	uint32_t frames, bad_lich, lich_assumed, bad_sacch, foreign_ran, calls, late_entries, releases, false_syncs;
	uint32_t slots_played, slots_muted, sync_losses;
} nxdn_rx_t;

void nxdn_rx_init(nxdn_rx_t *rx, const nxdn_rx_cfg_t *cfg);
/* One dibit in air order (0..3 as in nxdn.h). Returns 1 when a frame was processed */
int nxdn_rx_push_dibit(nxdn_rx_t *rx, uint8_t dibit);
/* Carrier lost or retuned: the call ends (NXDN_EV_END if one was on), sync is hunted again */
void nxdn_rx_reset(nxdn_rx_t *rx);

/* The demodulator and the frame layer together: 8 kHz discriminator samples in */
typedef struct {
	nxdn_demod_t demod;
	nxdn_rx_t rx;
	uint32_t samples;
} nxdn_receiver_t;

/* hpf_hz / deemph_us: the receive audio path's measured shape (0 = flat; nxdn_demod.h) */
void nxdn_receiver_init(nxdn_receiver_t *r, const nxdn_rx_cfg_t *cfg, uint16_t hpf_hz, uint16_t deemph_us);
void nxdn_receiver_audio(nxdn_receiver_t *r, const int16_t *pcm, size_t n);
/* Retuned, or the audio had a gap: acquire again */
void nxdn_receiver_restart(nxdn_receiver_t *r);

#ifdef __cplusplus
}
#endif
#endif
