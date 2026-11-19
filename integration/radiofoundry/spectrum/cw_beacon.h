/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus CW beacon and SOS (docs/plus.md): Tools > CW beacon keys a message in Morse on the channel's TX frequency
 * (an FM carrier with the CW tone keyed, the settings' speed and pitch: the radio's CW ID keyer), once or every
 * 30 / 60 / 180 s. It transmits: the channel's TX checks apply (TX inhibit, band, RX only, battery), and PTT or
 * anything else the user does stops it at once. Needs a callsign as the radio name (Settings > My radio). */
#ifndef PLUS_CW_BEACON_H
#define PLUS_CW_BEACON_H

#include <stdbool.h>
#include <stdint.h>

enum { CW_BEACON_DE, CW_BEACON_CQ, CW_BEACON_SOS, CW_BEACON_KINDS };
#define CW_BEACON_INTERVALS	4u	/* once, 30 s, 60 s, 180 s */

/* The message for a kind and callsign into out (cap includes the NUL): its length, 0 without a callsign */
uint8_t cw_beacon_text(uint8_t kind, const char *call, char *out, uint8_t cap);
uint32_t cw_beacon_interval_ms(uint8_t code);

/* The radio task's state, for the screen (read without a lock: a display only) */
typedef struct {
	bool on;		/* armed: keying now or waiting for the next one */
	bool keying;		/* on air now */
	uint8_t kind, interval;
	uint32_t next_ms;	/* when the next one keys (interval beacons) */
	uint32_t sent;		/* messages keyed since the start */
	uint8_t refused;	/* the last start was refused: a TXDENY_* code, 0 = none */
} cw_beacon_view_t;
void cw_beacon_view(cw_beacon_view_t *out);

/* Requests: kind CW_BEACON_KINDS = stop */
void radio_req_cw_beacon(uint8_t kind, uint8_t interval);

#endif
