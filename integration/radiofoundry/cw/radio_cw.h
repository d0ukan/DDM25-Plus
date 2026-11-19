/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus CW decoder (CONFIG_CW_RX, docs/plus.md "CW decoder and practice"), radio task side. Installed as
 * core/radio/radio_cw.{c,h}; radio_msg.c runs it (needs CONFIG_MSG_RX).
 *
 * Tools > CW decoder asks for it (radio_msg_want(RADIO_MSG_CW)) and it runs only while that screen is on top: on an
 * FM channel it hears FM caller ID's audio, like the pager (the normal receive path: MCW is a voice band tone, no flat
 * path needed); on an AM channel (the air band) it takes the receive ring itself, like ACARS. The decoder (cwdec.h)
 * comes from the RTOS heap while it listens; the text it decodes stays in a static ring, the newest RADIO_CW_TEXT
 * characters, until the screen clears it or the radio is turned off.
 *
 * Characters are numbered from power-on (radio_cw_written): the screen lays out its lines by those numbers, so a line
 * keeps its characters as new ones come and old ones leave the ring.
 *
 * RAM: the ring and the info (~180 bytes static); heap while listening: cwdec_t (~640 bytes). */
#ifndef RADIO_CW_H
#define RADIO_CW_H

#include <stdbool.h>
#include <stdint.h>

#define RADIO_CW_TEXT	160u	/* characters kept */

typedef struct {
	bool on;		/* the decoder runs */
	bool nomem;		/* the last start found no heap */
	bool key;		/* a mark now */
	uint8_t wpm;		/* 0: none yet */
	uint16_t tone_hz;	/* 0: no tone heard (the gate is closed) */
} radio_cw_info_t;

/* Radio task (radio_msg.c) */
bool radio_cw_start(void);
void radio_cw_stop(void);
void radio_cw_audio(const int16_t *s, uint16_t n);

/* Any task */
void radio_cw_info(radio_cw_info_t *out);
/* Changes with each character and each change of the info */
uint32_t radio_cw_seq(void);
/* Characters written since power-on; those from radio_cw_first() on are still kept */
uint32_t radio_cw_written(void);
uint32_t radio_cw_first(void);
/* Characters from number `from` on, at most n (those no longer kept are skipped), NUL terminated (out holds n + 1);
 * returns the count copied */
uint16_t radio_cw_copy(uint32_t from, char *out, uint16_t n);
/* The text forgotten (the screen's OK) */
void radio_cw_clear(void);

#endif
