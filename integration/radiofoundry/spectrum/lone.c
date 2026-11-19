/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus lone worker timer and its alarm action (lone.h). */
#include "config.h"
#if CONFIG_LONE_WORKER
#include "ui/lone.h"
#if CONFIG_DMR_EMERGENCY
#include "ui_svc.h"
#include "dmr/dmr_emergency.h"
#endif
#if CONFIG_APRS_MSG
#include "aprs/aprs.h"
#include "aprs/aprs_plus.h"
#endif

static uint8_t choice;
static uint8_t state;		/* lone_state_t; the one-byte states share a word (RAM budget) */
static uint32_t last_ms, sound_ms;
static uint8_t on_alarm;	/* lone_on_t */
static uint8_t tx;		/* lone_tx_t */
static bool asked;		/* this alarm's request was accepted: its outcome is read at the next beats */
static uint8_t ours;		/* lone_on_t: what an alarm started and no key has canceled yet */

uint8_t lone_minutes(uint8_t c)
{
	static const uint8_t m[LONE_CHOICES] = { 0, 5, 10, 15, 30, 60 };
	return c < LONE_CHOICES ? m[c] : 0;
}

/* End what an alarm of ours started, as the emergency key's cancel / Tools > APRS > SOS's off do. A request the
 * radio task has not taken yet is dropped with it. Returns what was really on */
static lone_on_t cancel_ours(void)
{
	lone_on_t was = (lone_on_t)ours;
	ours = LONE_ON_SOUND;
#if CONFIG_DMR_EMERGENCY
	if (was == LONE_ON_DMR) {
		if (!ui_svc_emergency_active())
			was = LONE_ON_SOUND;
		ui_svc_emergency(false);
	}
#endif
#if CONFIG_APRS_MSG
	if (was == LONE_ON_SOS)
		aprs_sos_set(false);
#endif
	return was;
}

void lone_set(uint8_t c, uint32_t now_ms)
{
	choice = c < LONE_CHOICES ? c : 0;
	state = choice ? LONE_WATCH : LONE_OFF;
	last_ms = now_ms;
	tx = LONE_TX_NONE;
	asked = false;
}

uint8_t lone_choice(void)
{
	return choice;
}

lone_state_t lone_state(void)
{
	return (lone_state_t)state;
}

bool lone_on_built(uint8_t on)
{
	return on == LONE_ON_SOUND || (on == LONE_ON_DMR && CONFIG_DMR_EMERGENCY) || (on == LONE_ON_SOS && CONFIG_APRS_MSG);
}

lone_on_t lone_on(void)
{
	return (lone_on_t)on_alarm;
}

lone_on_t lone_on_step(bool forward)
{
	uint8_t on = on_alarm;
	do {
		on = (uint8_t)((on + (forward ? 1u : LONE_ON_COUNT - 1u)) % LONE_ON_COUNT);
	} while (!lone_on_built(on));
	on_alarm = on;
	return (lone_on_t)on;
}

lone_tx_t lone_tx(void)
{
	return (lone_tx_t)tx;
}

lone_on_t lone_activity(uint32_t now_ms, bool ptt)
{
	last_ms = now_ms;
	if (state != LONE_OFF)
		state = LONE_WATCH;
	tx = LONE_TX_NONE;
	asked = false;
	return ptt ? LONE_ON_SOUND : cancel_ours();
}

/* An alarm beat with an action set: ask until the request is accepted, then read what became of it. The outcome of
 * a request is read one beat (3 s) after it was made: the radio task has taken it by then (dmr_emergency_state()
 * still says what the last start did until it has) */
static void alarm_action(void)
{
	if (tx == LONE_TX_DMR || tx == LONE_TX_SOS)
		return;		/* done: once per alarm */
#if CONFIG_DMR_EMERGENCY
	if (on_alarm == LONE_ON_DMR) {
		if (ui_svc_emergency_active()) {
			tx = LONE_TX_DMR;	/* ours, or the user's own: then it is not ours to cancel */
			return;
		}
		if (asked && dmr_emergency_state() == DMR_EMERGENCY_FAILED) {
			asked = false;
			ours = LONE_ON_SOUND;
			tx = LONE_TX_NO_DMR;
		}
		if (!asked) {
			if (ui_svc_emergency(true)) {
				asked = true;
				ours = LONE_ON_DMR;
				if (tx == LONE_TX_NONE)
					tx = LONE_TX_WAIT;
			} else {
				tx = LONE_TX_NO_DMR;
			}
		}
	}
#endif
#if CONFIG_APRS_MSG
	if (on_alarm == LONE_ON_SOS) {
		aprs_plus_t s;
		aprs_plus_state(&s);
		if (s.sos) {
			/* On: ours waiting for its first transmission (the engine's own gates and CSMA), or the user's */
			if (s.sos_sent != 0)
				tx = LONE_TX_SOS;
			else if (tx == LONE_TX_NONE)
				tx = LONE_TX_WAIT;
			return;
		}
		uint8_t why = aprs_sos_set(true);
		if (why == APRS_OK) {
			asked = true;
			ours = LONE_ON_SOS;
			tx = LONE_TX_WAIT;
		} else {
			tx = why == APRS_NO_POSITION ? LONE_TX_NO_POS : LONE_TX_NO_APRS;
		}
	}
#endif
}

static uint32_t period_ms(void)
{
	return (uint32_t)lone_minutes(choice) * 60000u;
}

lone_act_t lone_tick(uint32_t now_ms)
{
	if (state == LONE_OFF)
		return LONE_ACT_NONE;
	uint32_t idle = now_ms - last_ms;
	if (state == LONE_WATCH && idle >= period_ms()) {
		state = LONE_WARN;
		sound_ms = now_ms;
		return LONE_ACT_WARN;
	}
	if (state == LONE_WARN && idle >= period_ms() + LONE_WARN_MS) {
		state = LONE_ALARM;
		sound_ms = now_ms;
		tx = LONE_TX_NONE;
		asked = false;
		alarm_action();
		return LONE_ACT_ALARM;
	}
	if (state == LONE_WARN && now_ms - sound_ms >= LONE_WARN_EVERY_MS) {
		sound_ms = now_ms;
		return LONE_ACT_WARN;
	}
	if (state == LONE_ALARM && now_ms - sound_ms >= LONE_ALARM_EVERY_MS) {
		sound_ms = now_ms;
		alarm_action();
		return LONE_ACT_ALARM;
	}
	return LONE_ACT_NONE;
}

uint32_t lone_left_ms(uint32_t now_ms)
{
	uint32_t idle = now_ms - last_ms, p = period_ms();
	if (state == LONE_WATCH)
		return idle < p ? p - idle : 0;
	if (state == LONE_WARN)
		return idle < p + LONE_WARN_MS ? p + LONE_WARN_MS - idle : 0;
	return 0;
}
#endif
