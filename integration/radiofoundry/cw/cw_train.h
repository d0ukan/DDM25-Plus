/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus CW practice (CONFIG_CW_RX, docs/plus.md "CW decoder and practice"): Koch order characters and groups,
 * the answer's score, the playing time, and the hand-over of what to play from the UI task to the radio task.
 * Installed as core/radio/cw_train.{c,h}.
 *
 * LOCAL ONLY: the radio task plays the Morse with the beep generator (beep.h BEEP_MORSE: 8 kHz PCM to the speaker,
 * the path of the key beeps), not with the CW keyer of the CW ID and CW beacon. That keyer's tone is the FM
 * modulator's (hal_radio_t tx_tone, only while transmitting); the radio has no local-only sidetone, so it is never
 * used here. The practice code makes no TX request of any kind (no PTT, no tx(), no tx_tone()); while the radio
 * transmits a request is refused (tests/plus/test_cw_radio.inc). */
#ifndef CW_TRAIN_H
#define CW_TRAIN_H

#include <stdbool.h>
#include <stdint.h>

#define CW_TRAIN_KOCH_N		41u	/* characters in the Koch order */
#define CW_TRAIN_LEVEL_MIN	2u
#define CW_TRAIN_GROUP		5u	/* characters a group */
#define CW_TRAIN_MAX		8u	/* characters a play request holds */
#define CW_TRAIN_WPM_MIN	5u
#define CW_TRAIN_WPM_MAX	40u
#define CW_TRAIN_HZ_MIN		400u
#define CW_TRAIN_HZ_MAX		1000u
#define CW_TRAIN_HZ_STEP	50u
#define CW_TRAIN_BEEP_LEVEL	50u	/* the level when the key beeps are off (Settings): practice wants a sound */
#define CW_TRAIN_LEAD_MS	60u	/* the beep stream's route lead-in and the radio poll, before the first element */

/* G4FON / LCWO Koch order */
extern const char cw_koch[CW_TRAIN_KOCH_N + 1u];

/* A random character of the first `level` Koch characters (the newest of them every third time or so, as Koch
 * training wants); rng: any state, never 0 */
char cw_train_char(uint32_t *rng, uint8_t level);
/* n random characters (at most CW_TRAIN_MAX) into out (no NUL) */
void cw_train_group(uint32_t *rng, uint8_t level, uint8_t n, char *out);
/* Characters right: the sent count less the edit distance (missed, extra, wrong), at least 0 */
uint8_t cw_train_right(const char *sent, uint8_t n, const char *ans, uint8_t m);
/* How long text plays at wpm: its Morse units (dot 1, dash 3, gaps 1 / 3 / 7) times the dot */
uint32_t cw_train_ms(const char *text, uint8_t len, uint8_t wpm);
/* "-.-" for a character (out of at least 10 bytes); "" when it has no Morse */
void cw_train_pattern(char c, char *out);

/* --- UI task -> radio task --- */
typedef struct {
	char text[CW_TRAIN_MAX];
	uint8_t len;
	uint8_t wpm;
	uint16_t hz;
} cw_play_t;

#define CW_PLAY_IDLE		0u
#define CW_PLAY_PENDING		1u	/* asked, the radio task has not taken it yet */
#define CW_PLAY_PLAYING		2u	/* handed to the beep generator */
#define CW_PLAY_REFUSED		3u	/* the radio transmits (or the build has no beeps): not played */

/* UI task: play this (replaces one not yet taken) */
void cw_train_play(const char *text, uint8_t len, uint8_t wpm, uint16_t hz);
uint8_t cw_train_state(void);
/* Radio task, every poll: a request waiting (taken) */
bool cw_train_take(cw_play_t *out);
/* Radio task: what became of the request taken */
void cw_train_done(bool played);

#endif
