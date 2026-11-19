/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus fox hunt (docs/plus.md): the signal strength of the channel in use as a meter on Tools > Fox hunt and as a
 * "vario" tone in the speaker, higher and faster as the signal grows, so the antenna can be swung without looking.
 * The attenuator is virtual (the receiver has none): it moves the scale down so a strong signal near the fox still
 * changes the tone. Pure logic; the radio task plays the tone (beep_set_tone + BEEP_TONE). */
#ifndef PLUS_FOX_H
#define PLUS_FOX_H

#include <stdbool.h>
#include <stdint.h>

#define FOX_FLOOR_DBM		(-125)	/* no tone below this (after the attenuator) */
#define FOX_TOP_DBM		(-45)	/* the highest, fastest tone at this */
#define FOX_TONE_MS		50u
#define FOX_IDLE_MS		5000u	/* the screen didn't look this long: the tone stops */

void fox_set(bool on, uint8_t att_db, uint32_t now_ms);	/* att 0, 10, 20 or 30 dB */
bool fox_on(void);
uint8_t fox_att(void);
/* Every radio poll with the reading: the tone to play now (0 = none) */
uint16_t fox_service(int16_t rssi_dbm, uint32_t now_ms);
/* The tone and the gap after it for a reading (exposed for tests): hz 0 = silent */
uint16_t fox_tone_hz(int16_t rssi_dbm, uint8_t att_db, uint16_t *period_ms);
/* The UI's screen still looks (called from its draw) */
void fox_seen(void);

void radio_req_fox(bool on, uint8_t att_db);

#endif
