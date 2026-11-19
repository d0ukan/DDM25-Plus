/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus FM caller ID (CONFIG_FM_ID, docs/plus.md), radio task side. Installed as core/radio/radio_fmid.{c,h}
 * when CONFIG_FM_ID=y (needs CONFIG_APRS_RX: the HR_C7000's 8 kHz FM receive audio ring, and CONFIG_DMR for its
 * interrupt).
 *
 * While an FM channel receives (not TX, not air band AM, broadcast FM, the spectrum sweep or the P25 search), the
 * decoders (fmid.h: MDC1200, FleetSync I, DTMF) read the FM receive audio all the time, squelch open or not: an ID
 * sent at key-up arrives while the squelch is still opening. The audio comes through the board's APRS receive path
 * (board.afsk rx_start / rx_read, the 2 KB heap ring of fm_rx_audio.c). While APRS itself listens (an FM channel on
 * the APRS frequency) APRS owns that ring and hands every block it reads to radio_fmid_audio() too; when APRS wants
 * the ring, it calls radio_fmid_yield() first. NXDN channels are not FM channels, so the two never overlap.
 *
 * Side effect, the same as on the APRS frequency today: while the ring runs, the DM-32 plays FM receive audio through
 * the HR_C7000's FM playback instead of the FD6818's direct path (radio_dm32.c speaker_source, aprs_listen_route).
 * Settings > Talk & listen > FM caller ID turns it off (radio_fmid_enable; kept in settings_t.cp_flags bit 7,
 * SETTINGS_CP_NO_FM_ID, 0 = on): the decoders stay quiet and the ring is taken only while the Pager or SSTV screen
 * wants the FM audio (radio_msg_wants_fm), so FM audio keeps the direct path otherwise.
 *
 * HARDWARE-UNVERIFIED (docs/plus-radio-test.md 6b, F rows). */
#ifndef RADIO_FMID_H
#define RADIO_FMID_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Every radio poll. want: an FM channel receives (see above). Returns true when what Home shows changed (a new ID,
 * a DTMF digit, a line that timed out): the caller redraws */
bool radio_fmid_poll(bool want, uint32_t rx_hz, uint32_t now_ms);
/* APRS is about to start its receive path: give the ring back (radio task) */
void radio_fmid_yield(void);
/* Audio APRS read from the ring while it owns it (radio task) */
void radio_fmid_audio(const int16_t *s, uint16_t n);
/* Settings > Talk & listen > FM caller ID: on (the default) or off. Any task (the UI at start-up and on a change) */
void radio_fmid_enable(bool on);
/* The receive ring is wanted: fm_rx an FM channel receives (radio_fmid_poll's want), id_on the setting, screen the
 * Pager or SSTV screen wants the FM audio */
static inline bool radio_fmid_ring_wanted(bool fm_rx, bool id_on, bool screen)
{
	return fm_rx && (id_on || screen);
}

/* Kinds, also the Recent list's ui_recent_t mode values (0 DMR, 2-4 RADIO_MODE_P25 / _YSF / _NXDN) */
#define RADIO_FMID_MDC		0x80u
#define RADIO_FMID_MDC_EMERG	0x81u
#define RADIO_FMID_FLEETSYNC	0x82u
#define RADIO_FMID_DTMF		0x83u
#define RADIO_FMID_DTMF_MAX	16u

typedef struct {
	uint8_t kind;		/* RADIO_FMID_*, 0 = none */
	uint16_t unit;		/* MDC unit ID (shown in hex), FleetSync unit */
	uint16_t fleet;		/* FleetSync fleet */
	char dtmf[RADIO_FMID_DTMF_MAX + 1];
} radio_fmid_id_t;

/* "MDC 1A2B", "FleetSync 107-1772", "DTMF 1234#" (protocol names, not translated; out of at least 24 bytes) */
void radio_fmid_text(uint8_t kind, uint16_t unit, uint16_t fleet, const char *dtmf, char *out, size_t size);

/* What Home's bottom line shows on an FM channel (kind 0: nothing). Any task */
void radio_fmid_shown(radio_fmid_id_t *out);
/* The last finished ID for the call history: an MDC or FleetSync ID as it is decoded, a DTMF string once a few
 * seconds pass without a digit. Returns a number that changes with each one (any task) */
uint32_t radio_fmid_logged(radio_fmid_id_t *out);

#endif
