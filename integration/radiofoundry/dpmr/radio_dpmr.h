/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus dPMR receive on the FM path (docs/dpmr.md), radio task side. Installed as core/radio/radio_dpmr.h when
 * CONFIG_DPMR=y (needs CONFIG_NXDN: the same FM receive audio, flat path, 4FSK demodulator, voice pacing and RAM).
 *
 * A dPMR channel is tuned like an NXDN one (the board stays in FM mode, the analog speaker shut). The functions are in
 * core/radio/radio_nxdn.c, beside NXDN's: one receiver object serves whichever of the two the selected channel is
 * (a union; a mode change stops one before the other starts), so dPMR adds no receive buffers of its own.
 *
 * HARDWARE-UNVERIFIED: see docs/dpmr.md "What the radio must confirm". */
#ifndef RADIO_DPMR_H
#define RADIO_DPMR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Every radio poll, like radio_nxdn_poll(). cc: the channel's colour code filter (0 = any, else code number + 1) */
void radio_dpmr_poll(bool want, uint32_t rx_hz, uint8_t cc, uint32_t now_ms);

/* dPMR frames were heard in the last 300 ms (the green LED) */
bool radio_dpmr_heard(uint32_t now_ms);

/* The call on the channel, for Home's bottom line and the call history */
typedef struct {
	bool active;		/* a call is on (header or CCH seen, no end frame or sync loss since) */
	bool all_call, data, emergency, playing;
	bool have_source, have_destination;
	uint8_t cc;		/* the colour code heard, 0..63; 0xFF before one is */
	uint32_t source, destination;	/* 24-bit air interface IDs (dpmr_id_text) */
} radio_dpmr_caller_t;
/* Copies the caller (any task); returns a sequence number that changes whenever it does */
uint32_t radio_dpmr_caller(radio_dpmr_caller_t *out);

/* Home's bottom line as text alternates (core/ui/font.h 0x1D / 0x1F / 0x1E), longest first:
 * "dPMR 1234567 > 7654321 CC 16" | "1234567>7654321 CC16" | "1234567>7654321"; "dPMR call CC 16" before the IDs,
 * "dPMR data ..." for a data call. 0 when no call is on */
size_t radio_dpmr_caller_line(char *out, size_t cap);

#endif
