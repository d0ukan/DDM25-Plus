/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus NXDN48 receive on the FM path (docs/nxdn.md), radio task side. Installed as core/radio/radio_nxdn.{c,h}
 * when CONFIG_NXDN=y (needs CONFIG_DMR for the AMBE+2 voice path and CONFIG_APRS_RX for the HR_C7000 FM receive
 * audio ring).
 *
 * An NXDN channel is tuned like an FM channel (the board stays in FM mode; radio_sm's outputs() keeps the analog
 * speaker path shut because the channel's mode is not RADIO_MODE_FM). While it is selected and not transmitting, the
 * radio task calls radio_nxdn_poll() every poll: it switches the board's receive audio to the flat discriminator
 * (board_nxdn_flat, boards/dm32 dm32_nxdn.inc), starts the HR_C7000's 8 kHz FM RX RAM copy (board.afsk rx_start, the
 * path APRS uses), runs src/nxdn on the samples, and plays admitted calls' voice through RadioFoundry's DMR voice
 * bridge (ambeVoiceSink: the stock AMBE+2 vocoder, the PCM speaker path), paced one 20 ms field per 20 ms.
 *
 * Structure follows DM32-nxdn-p25 integration/radiofoundry/nxdn/radio_nxdn.c (GPL-3.0-or-later, "lyra XE1POC"),
 * receive half, rewritten against the Classic r7 tree's APIs (hal_afsk.h receive path instead of their
 * hal_audio_tap.h fm_rx_flat, which the Classic tree does not have).
 *
 * HARDWARE-UNVERIFIED: written against the patched Classic tree, compiled nowhere yet; see docs/nxdn.md "What the
 * radio must confirm". */
#ifndef RADIO_NXDN_H
#define RADIO_NXDN_H

#include <stdbool.h>
#include <stdint.h>

/* Every radio poll. want: an NXDN channel is selected and the radio receives FM on it (not TX, not a scan step on
 * another channel, not the spectrum / P25 search sweep). rx_hz identifies the tuning (a change restarts the
 * acquisition and re-applies the flat path), ran is the channel's RAN filter (0 = any) */
void radio_nxdn_poll(bool want, uint32_t rx_hz, uint8_t ran, uint32_t now_ms);

/* NXDN frames were heard in the last 300 ms (the green LED, scan activity) */
bool radio_nxdn_heard(uint32_t now_ms);

/* The call on the channel, for Home's bottom line and the dual watch pane ("NXDN 1234 > TG 101") */
typedef struct {
	bool active;		/* a call is on (VCALL seen, no TX_REL or sync loss since) */
	bool group, encrypted, emergency, playing;
	uint16_t source, destination;
	uint8_t ran;
} radio_nxdn_caller_t;
/* Copies the caller (any task); returns a sequence number that changes whenever it does */
uint32_t radio_nxdn_caller(radio_nxdn_caller_t *out);

/* The board's flat receive path (boards/dm32 dm32_nxdn.inc): FD6818 AF filters off, HR_C7000 FM band-pass off. on:
 * false when the board can't (not in FM receive). Idempotent; call again after every retune */
bool board_nxdn_flat(bool on);

#endif
