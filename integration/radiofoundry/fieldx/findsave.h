/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus: P25 / DMR search finds saved as codeplug channels (CONFIG_FIND_SAVE, docs/plus.md "Save search finds").
 * The portable part (no RTOS, no codeplug access): what a find becomes as a channel, its name, the zone name
 * "Bulunanlar N" and the duplicate rule. Installed as core/ui/findsave.{c,h}; the sequencer that writes them through
 * the zone editor's own storage path is ui_findsave.c.
 *
 * Channel rules:
 *   - P25: mode P25, the NAC found (RX; TX NAC Same as RX), Normal squelch (every talkgroup on that NAC: "TG Any"),
 *     the talkgroup heard kept as the TX talkgroup. It may transmit only when codeplug_p25_valid() would let it and
 *     we know enough to do so safely: a talkgroup was heard (the system's own, from a voice call's link control), our
 *     own P25 unit ID is known (the radio's default, codeplug_p25_unit_id(0)), the frequency is in the 2 m or 70 cm
 *     amateur band (the channel editor's own outside-band rule) and in the radio's TX bands. Otherwise RX only.
 *   - DMR: mode DMR, the colour code found (Any when none was named), the one slot seen (Any when both or none),
 *     RX group list Any (hear every talkgroup), no TX contact: always RX only (the search never learns which
 *     talkgroup we may use; pick a contact in the channel editor to transmit).
 *   - Both: simplex on the frequency found, 12.5 kHz, the radio's power, named "P25 433.500" / "DMR 438.7125".
 * Duplicate: a channel of the same mode on the same RX frequency with the same NAC (P25) or colour code (DMR). */
#ifndef PLUS_FINDSAVE_H
#define PLUS_FINDSAVE_H

#include <stdbool.h>
#include <stdint.h>
#include "codeplug/codeplug.h"

#define FINDSAVE_ZONE_BASE	"Bulunanlar"	/* the zone's name: the base, a space and a number (not translated) */
#define FINDSAVE_ZONE_NUM_MAX	999u
#define FINDSAVE_CC_NONE	0xFFu		/* a DMR find whose colour code was not named */

enum { FINDSAVE_P25, FINDSAVE_DMR };

typedef struct {
	uint32_t hz;
	uint8_t kind;		/* FINDSAVE_P25 / FINDSAVE_DMR */
	uint8_t cc;		/* DMR: colour code, FINDSAVE_CC_NONE */
	uint8_t ts_mask;	/* DMR: slots with traffic, bit 0 TS1, bit 1 TS2 */
	uint16_t nac;		/* P25 */
	uint16_t tg;		/* P25: talkgroup heard, 0 none */
} findsave_item_t;

/* "P25 433.500", "DMR 438.7125" (MHz with 3 decimals, 4 when the 100 Hz digit needs it) */
void findsave_name(const findsave_item_t *f, char out[CODEPLUG_NAME_LEN + 1]);
/* The channel for f. own_p25_id: our P25 unit ID (0 = not known); tx_band: the radio may transmit on f->hz */
void findsave_channel(const findsave_item_t *f, uint32_t own_p25_id, bool tx_band, channel_t *out);
/* The P25 transmit rule above */
bool findsave_p25_tx(const findsave_item_t *f, uint32_t own_p25_id, bool tx_band);
/* ch is a channel for the same find (the duplicate rule) */
bool findsave_same(const channel_t *ch, const findsave_item_t *f);
/* "Bulunanlar 12" -> 12; 0 when name is not a find zone's */
uint16_t findsave_zone_number(const char *name);
/* "Bulunanlar <n>" */
void findsave_zone_name(uint16_t n, char out[CODEPLUG_NAME_LEN + 1]);

/* --- The UI task (ui_findsave.c) ---
 * Save the search screen's finds first .. end - 1. all (one false): into a new zone "Bulunanlar N" (N one more than
 * the highest there is). one: into the newest such zone (a new one when there is none or it is full). The writes
 * are the zone editor's own (ui_svc_zone_edit: ZONE_OP_NEW, then ZONE_OP_ADD_NEW for each find, the channel editor's
 * "Copy here" path: a free channel number written and put in the zone in one job), one at a time, each checked
 * before the next; a find already in that zone is skipped. A toast says how it went. False: busy, or nothing to save */
bool ui_findsave_start(uint8_t kind, uint8_t first, uint8_t end, bool one);
bool ui_findsave_busy(void);
/* Every UI tick (ui_app.c): one step. True: draw again */
bool ui_findsave_tick(void);
/* "Saving 3 of 8" while busy, for the search screens; false when idle */
bool ui_findsave_line(char *buf, uint8_t len);
/* The search screens' lists as the save began (frozen while it runs): find i, false past the end or once another
 * screen has taken the shared buffer the list lives in (ui_scratch.h; the save then stops with the count so far) */
bool ui_p25search_find(uint8_t i, findsave_item_t *out);
bool ui_dmrsearch_find(uint8_t i, findsave_item_t *out);

#endif
