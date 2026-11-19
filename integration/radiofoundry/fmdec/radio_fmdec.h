/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus tone finder (CONFIG_TONE_FIND) and DTMF / SelCall decoder (CONFIG_SELCALL), radio task side. Installed as
 * core/radio/radio_fmdec.{c,h}. They run only while their screen is open (Tools > Tone finder, Tools > DTMF /
 * SelCall), through the pager's code (radio_msg.c: RADIO_MSG_TONEF, RADIO_MSG_SELCALL): an FM channel FM caller ID
 * listens on, FM caller ID's audio (its ring, or APRS's on the APRS frequency; FM caller ID turned off in Settings
 * still takes the ring for these screens), the decoder from the RTOS heap meanwhile. The tone finder also turns the
 * flat receive path on, as the pager does (board_nxdn_flat: the FD6818's 300 Hz high pass would take the
 * sub-audible tone out; the speaker plays flat audio meanwhile). The DTMF / SelCall screen leaves the audio path as
 * it is (its tones are in the voice band).
 *
 * The tone finder keeps what it hears now and the last tone it locked (shown after the signal ends, the one OK
 * writes). The SelCall screen's list (the last RADIO_SELCALL_N sequences, newest first, with when and on which
 * frequency) is static and outlives the screen until power off; the same sequence again within 3 s refreshes the
 * newest entry.
 *
 * Experimental (TONEF_CHIP_READ, on by default): while the tone finder runs, the FD6818's registers 0x68-0x6A are
 * read four times a second (board_css_read). On the BK4819, the chip the FD6818 is said to resemble, they hold its
 * own CTCSS frequency measurement and the DCS code it receives; whether the FD6818 has them is not known, so the
 * screen only shows what they read, as a check beside the software's answer (docs/plus-radio-test.md, TF rows). */
#ifndef RADIO_FMDEC_H
#define RADIO_FMDEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "config.h"

#ifndef TONEF_DCS_INVERT
#define TONEF_DCS_INVERT	0	/* 1: the receive audio's sign is inverted (a D023N signal reads 023I) */
#endif
#ifndef TONEF_CHIP_READ
#define TONEF_CHIP_READ		1
#endif

/* radio_tonef_info_t.state */
#define RADIO_TONEF_OFF		0u	/* not listening (no audio yet, the channel doesn't suit) */
#define RADIO_TONEF_SEARCH	1u	/* listening, nothing found yet (the first 3 s, or since a tone ended) */
#define RADIO_TONEF_NONE	2u	/* listening, 3 s and more without a tone */
#define RADIO_TONEF_FOUND	3u

typedef struct {
	uint8_t state;
	uint8_t type;		/* hal_radio.h tone_type_t: what is heard now (TONE_NONE: nothing) */
	uint16_t value;		/* CTCSS Hz x 10, DCS the code */
	uint8_t quality;	/* 0-100 */
	uint8_t last_type;	/* the last tone locked, kept (TONE_NONE: none yet) */
	uint16_t last_value;
	bool nomem;		/* the decoder didn't fit in the heap */
	bool chip_ok;		/* chip[] read (TONEF_CHIP_READ) */
	uint16_t chip[3];	/* FD6818 0x68, 0x69, 0x6A */
} radio_tonef_info_t;

#define RADIO_SELCALL_N		6u
#define RADIO_SELCALL_TEXT	16u

typedef struct {
	uint32_t ms;		/* heard (radio clock) */
	uint32_t rx_hz;
	uint8_t set;		/* selcall.h: SELCALL_ZVEI1 .. SELCALL_DTMF */
	char text[RADIO_SELCALL_TEXT + 1];
} radio_selcall_t;

/* radio_msg.c (radio task): start the kind's decoder (false: no heap), stop it, its audio, every poll */
bool radio_fmdec_start(uint8_t kind, uint32_t rx_hz);
void radio_fmdec_stop(void);
void radio_fmdec_audio(const int16_t *s, uint16_t n);
void radio_fmdec_poll(uint32_t now_ms);

/* Any task: changes with each new result */
uint32_t radio_fmdec_seq(void);
void radio_tonef_info(radio_tonef_info_t *out);
/* The i-th newest sequence (a copy), false past the last */
bool radio_selcall_get(uint8_t i, radio_selcall_t *out);
uint8_t radio_selcall_count(void);
/* The SelCall set looked for: SELCALL_ANY (all) or one (UI task; RAM only, Any after power on) */
void radio_selcall_set(uint8_t only);
uint8_t radio_selcall_only(void);

/* boards/dm32: the FD6818's 0x68, 0x69, 0x6A (false: not read; a weak default for other boards) */
bool board_css_read(uint16_t r[3]);

#endif
