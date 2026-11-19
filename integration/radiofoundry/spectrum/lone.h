/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus lone worker (docs/plus.md): with a time set (Tools > Lone worker), no key and no PTT for that long gives
 * a warning (a beep every 10 s for 30 s, "Lone worker: press a key"), then an alarm (the emergency tone every 3 s)
 * until a key is pressed. Any key or PTT starts the time again. Kept in RAM: off after power off. Pure logic in the
 * UI task (ui.c feeds keys and ticks).
 *
 * On alarm (lone_on_t, RAM only, Sound only after power on: the radio never transmits by itself unless the owner
 * chose it): besides the tone the alarm can start upstream's DMR emergency (ui_svc_emergency, what the emergency key
 * calls: the alarm CSBKs and, per the emergency settings, the emergency call with its hot mic) or the APRS SOS
 * (aprs_sos_set, what Tools > APRS > SOS's OK calls: the Mic-E emergency position now and every 2 minutes). Both keep
 * their own gates (a DMR channel we may transmit on with an ID; callsign, amateur APRS frequency, AFSK, a position,
 * CSMA). The request is made at the alarm's beats (every 3 s) until it is accepted, then never again in that alarm.
 * A key other than PTT ends the alarm and cancels what the alarm started (the emergency key's cancel / SOS off: what
 * was sent stays sent); PTT ends the tone only, so the emergency call can be answered, and the next other key
 * cancels. An emergency or SOS the user started is never touched. */
#ifndef PLUS_LONE_H
#define PLUS_LONE_H

#include <stdbool.h>
#include <stdint.h>

#define LONE_WARN_MS		30000u	/* the warning lasts this long before the alarm */
#define LONE_WARN_EVERY_MS	10000u
#define LONE_ALARM_EVERY_MS	3000u
#define LONE_CHOICES		6u	/* off, 5, 10, 15, 30, 60 min */

typedef enum { LONE_OFF, LONE_WATCH, LONE_WARN, LONE_ALARM } lone_state_t;
typedef enum { LONE_ACT_NONE, LONE_ACT_WARN, LONE_ACT_ALARM } lone_act_t;
/* What the alarm does besides the tone */
typedef enum { LONE_ON_SOUND, LONE_ON_DMR, LONE_ON_SOS, LONE_ON_COUNT } lone_on_t;
/* What this alarm has sent: nothing asked (Sound only, or no alarm), asked and not known yet, the DMR emergency
 * started, an SOS position sent, or why nothing went: not a DMR channel we may transmit on (or no ID), no position
 * for the SOS, APRS can't send (callsign, frequency, AFSK) */
typedef enum {
	LONE_TX_NONE, LONE_TX_WAIT, LONE_TX_DMR, LONE_TX_SOS, LONE_TX_NO_DMR, LONE_TX_NO_POS, LONE_TX_NO_APRS,
} lone_tx_t;

void lone_set(uint8_t choice, uint32_t now_ms);	/* 0 = off */
uint8_t lone_choice(void);
uint8_t lone_minutes(uint8_t choice);
/* A key or PTT. Returns what it canceled: LONE_ON_DMR / LONE_ON_SOS when a key (not PTT) ended an emergency / SOS
 * that an alarm had started, else LONE_ON_SOUND */
lone_on_t lone_activity(uint32_t now_ms, bool ptt);
lone_act_t lone_tick(uint32_t now_ms);		/* what to sound now */
lone_state_t lone_state(void);
uint32_t lone_left_ms(uint32_t now_ms);		/* until the warning (watching) or the alarm (warning) */

bool lone_on_built(uint8_t on);			/* this build has it (CONFIG_DMR_EMERGENCY, CONFIG_APRS_MSG) */
lone_on_t lone_on(void);
lone_on_t lone_on_step(bool forward);		/* to the next / previous one the build has */
lone_tx_t lone_tx(void);
/* ui_lone.c: the line for lone_tx() (what was sent, or why nothing was), NULL when there is none */
const char *ui_lone_tx_text(void);

#endif
