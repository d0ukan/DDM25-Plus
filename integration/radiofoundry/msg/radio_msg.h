/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus pager and ACARS receive (CONFIG_MSG_RX, docs/plus.md), radio task side. Installed as
 * core/radio/radio_msg.{c,h} when CONFIG_MSG_RX=y (needs CONFIG_FM_ID: the pager hears the FM audio FM caller ID
 * reads, and CONFIG_APRS_RX: the HR_C7000's 8 kHz receive audio ring).
 *
 * The decoders run only while their screen is open (Tools > Pager, Tools > ACARS, or one of their messages): the UI
 * says which (radio_msg_want), the radio task listens if the channel suits:
 *   - Pager (pocsag.h): an FM channel (not air band AM), whenever FM caller ID listens there (radio_sm.c's
 *     conditions: receiving, no APRS job, no hotspot, sweep or search). The audio is the one FM caller ID reads (its
 *     own ring, or APRS's when APRS listens on the frequency): radio_fmid.c hands every block to radio_msg_audio().
 *     POCSAG is NRZ: the FM path's 300 Hz high pass and de-emphasis smear it (512 bit/s worst), so while the Pager
 *     screen listens the board's flat receive path is on (board_nxdn_flat, CONFIG_NXDN: the FD6818's AF filters and
 *     the HR_C7000's FM band-pass off, as on an NXDN channel); leaving the screen or the channel puts them back. The
 *     speaker plays that flat audio meanwhile (brighter, a little louder).
 *   - ACARS (acars.h): an FM channel received in AM (the air band, Tools > Air band). FM caller ID, APRS and NXDN
 *     never listen there, so this takes the receive ring itself (board.afsk rx_start / rx_read / rx_stop): the
 *     FD6818's AF output (its AM demodulator's in AM) then stays on the HR_C7000's input, which copies it into its
 *     8 kHz RX RAM and plays an open squelch through its own FM playback, as for APRS (radio_dm32.c speaker_source).
 *     HARDWARE-UNVERIFIED: that the HR_C7000 passes the FD6818's AM audio to its RX RAM like FM audio (the documented
 *     path takes the FD6818's AF output whatever its type; docs/plus-radio-test.md 6b, M rows).
 *
 * Messages are kept in RAM, six of each kind, newest first; the same message again (same address and content within
 * a minute: pager networks repeat pages, aircraft repeat unacknowledged blocks) only refreshes the newest entry.
 *
 * RAM: the two message lists (about 1.25 KB) are static; the decoder (about 240 bytes, one of the two) and, for
 * ACARS, the 2 KB receive ring come from the RTOS heap while a screen listens. CPU (8 kHz): the pager's six slicers
 * (three rates x two) are about 25 operations a sample each, ACARS's mixer and detector about 30: under 1.5 MHz, about
 * 1 % of the 168 MHz core, in the radio task's 25 ms polls. */
#ifndef RADIO_MSG_H
#define RADIO_MSG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RADIO_MSG_PAGER		1u
#define RADIO_MSG_ACARS		2u
/* DM32 Plus SSTV (CONFIG_SSTV, radio_sstv.h): Tools > SSTV listens like the pager (an FM channel FM caller ID listens
 * on, the flat receive path on, FM caller ID's audio), its decoder and rows in radio_sstv.c, not a message list */
#define RADIO_MSG_SSTV		3u
/* DM32 Plus Tools > Tone finder (CONFIG_TONE_FIND) and Tools > DTMF / SelCall (CONFIG_SELCALL), radio_fmdec.h: they
 * listen where the pager would, on FM caller ID's audio; the tone finder with the flat receive path on (the
 * sub-audible tones), DTMF / SelCall without it (voice band tones) */
#define RADIO_MSG_TONEF		4u
#define RADIO_MSG_SELCALL	5u
/* DM32 Plus Tools > CW decoder (CONFIG_CW_RX, radio_cw.h): on an FM channel it listens like the pager (FM caller ID's
 * audio; the normal receive path, not the flat one), on an AM channel like ACARS (the receive ring itself) */
#define RADIO_MSG_CW		6u
#define RADIO_MSG_N		6u	/* messages kept of each kind */
#define RADIO_MSG_TEXT		80u	/* text characters kept, with the terminator */

/* radio_msg_status() */
#define RADIO_MSG_ST_OFF	0u	/* not listening: the channel doesn't suit the screen open (or none is) */
#define RADIO_MSG_ST_WAIT	1u	/* listening, no audio yet (the ring starting, no heap) */
#define RADIO_MSG_ST_ON		2u	/* listening, audio coming */

/* flags */
#define RADIO_MSG_ERRORS	0x01u	/* pager: a codeword had more errors than the code corrects; ACARS: the BCS didn't
					 * match (every character's parity did) */
#define RADIO_MSG_CUT		0x02u	/* longer than kept */
#define RADIO_MSG_AGAIN		0x04u	/* heard more than once */
#define RADIO_MSG_MORE		0x08u	/* ACARS: more blocks follow (ETB) */

/* pager kinds (pocsag_text) */
#define RADIO_MSG_TONE		0u
#define RADIO_MSG_NUMERIC	1u
#define RADIO_MSG_ALPHA		2u

typedef struct {
	uint32_t ms;		/* heard last (radio clock) */
	uint32_t capcode;	/* the 21-bit address */
	uint16_t id;		/* its own number, 1-0x7FFF (the message screen finds it by this) */
	uint16_t hash;		/* of the address and content: the same message again */
	uint8_t function;	/* 0-3 */
	uint8_t rate;		/* 0 512, 1 1200, 2 2400 bit/s */
	uint8_t kind;		/* RADIO_MSG_TONE / _NUMERIC / _ALPHA */
	uint8_t flags;
	char text[RADIO_MSG_TEXT];
} radio_pager_msg_t;

typedef struct {
	uint32_t ms;
	uint16_t id, hash;
	char reg[8];		/* aircraft registration */
	char flight[7];		/* flight ID (downlinks), "" */
	char label[3];
	char mode, block;
	uint8_t flags;
	char text[RADIO_MSG_TEXT];
} radio_acars_msg_t;

/* UI task, every tick: the message screen open (RADIO_MSG_PAGER / _ACARS), 0 = none */
void radio_msg_want(uint8_t kind);
/* Radio task, every poll: which kind the channel suits now (RADIO_MSG_PAGER: an FM channel FM caller ID listens on;
 * RADIO_MSG_ACARS: one received in AM; 0: neither). Returns true when a message came or the status changed */
bool radio_msg_poll(uint8_t suits, uint32_t rx_hz, uint32_t now_ms);
/* Radio task: the FM receive audio FM caller ID reads (radio_fmid.c) */
void radio_msg_audio(const int16_t *s, uint16_t n);
/* The Pager or SSTV screen wants the FM receive audio: FM caller ID turned off in Settings still takes its ring for
 * them (radio_fmid.c). Any task */
bool radio_msg_wants_fm(void);

/* Any task */
uint8_t radio_msg_status(void);
/* changes with each new message and each status change */
uint32_t radio_msg_seq(void);
uint8_t radio_msg_count(uint8_t kind);
/* The i-th newest message, NULL past the last. Read in place: the radio task may write the oldest one meanwhile (a
 * new message: radio_msg_seq() changes and the screen draws again) */
const radio_pager_msg_t *radio_msg_pager(uint8_t i);
const radio_acars_msg_t *radio_msg_acars(uint8_t i);

#endif
