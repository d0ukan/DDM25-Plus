/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus lone worker timer (lone.c): off by default, warning after the time, repeated warnings, the alarm after
 * the warning, a key starts again from any state, turning it off.
 *
 * With -DLONE_TEST_TX (CONFIG_DMR_EMERGENCY and CONFIG_APRS_MSG on) also the alarm action. The DMR side is upstream's
 * own: ui_svc_emergency.c and dmr_emergency.c are linked as they are, the test plays the radio task (it calls
 * dmr_emergency_tick with can_transmit, the radio core's data_can_send: a DMR channel, every TX check, an ID) and
 * the data service below records the alarm transmissions asked for. The APRS side is a fake engine with
 * aprs_sos_set's contract (aprs_plus.h). Checked: Sound only never asks for anything; each action starts once per
 * alarm; a refusal keeps the alarm, says why and goes once the gate opens; a key cancels what the alarm started,
 * PTT doesn't; an emergency / SOS the user started is left alone. */
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "ui/lone.h"

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void radio_tick(uint32_t now);

static int count(lone_act_t want, uint32_t *now, uint32_t ms)
{
	int n = 0;
	for (uint32_t end = *now + ms; *now < end; *now += 100u) {
		n += lone_tick(*now) == want;
		radio_tick(*now);
	}
	return n;
}

#ifdef LONE_TEST_TX
#include "ui_svc.h"
#include "dmr/dmr_emergency.h"
#include "dmr/dmrDataService.h"
#include "aprs/aprs.h"
#include "aprs/aprs_plus.h"
#include "codeplug/codeplug.h"

/* --- The radio's side of the DMR emergency --- */
static bool can_transmit;		/* radio_sm.c's data_can_send() */
static int alarms, alarm_cancels;	/* alarm sequences handed to the data service, its cancels */
static bool alarm_busy;

bool dmrDataServiceEmergencyAlarm(uint32_t dst, bool group, uint8_t fid, uint8_t tries)
{
	(void)fid;
	CHECK(dst == 91 && group && tries == DMR_EMERGENCY_TRIES_DEFAULT);
	alarms++;
	alarm_busy = true;
	return true;
}
void dmrDataServiceEmergencyAlarmCancel(void) { alarm_cancels++; alarm_busy = false; }
bool dmrDataServiceEmergencyAlarmBusy(void) { return alarm_busy; }
bool ui_notify(const ui_event_t *ev) { (void)ev; return true; }
bool codeplug_contact_find(uint32_t id, bool group, contact_t *out) { (void)id; (void)group; (void)out; return false; }

static void radio_tick(uint32_t now)
{
	dmr_emergency_channel_t c = { .can_transmit = can_transmit, .talk_id = 91, .talk_private = false };
	dmr_emergency_tick(now, &c);
}

/* --- The APRS engine's SOS (aprs.c's aprs_sos_set / aprs_plus_state) --- */
static uint8_t sos_block;		/* aprs_block_t: why it can't be turned on now */
static bool sos_on, sos_air;		/* on; the engine gets its frames out (its own gates, CSMA) */
static int sos_starts, sos_stops;
static uint16_t sos_sent;

uint8_t aprs_sos_set(bool on)
{
	if (on && sos_block != APRS_OK)
		return sos_block;
	if (on && !sos_on) {
		sos_starts++;
		sos_sent = 0;
	}
	if (!on && sos_on)
		sos_stops++;
	sos_on = on;
	return APRS_OK;
}

void aprs_plus_state(aprs_plus_t *s)
{
	memset(s, 0, sizeof *s);
	if (sos_on && sos_air && sos_sent == 0)
		sos_sent = 1;	/* the engine's next tick sent it */
	s->sos = sos_on;
	s->sos_sent = sos_sent;
}

/* From watching to the alarm's first beat (5 min + the warning) */
static void to_alarm(uint32_t *now)
{
	lone_activity(*now, true);
	CHECK(count(LONE_ACT_ALARM, now, 300000u + LONE_WARN_MS + 200u) == 1 && lone_state() == LONE_ALARM);
}

static void pick(lone_on_t on)
{
	while (lone_on() != on)
		lone_on_step(true);
}

static void test_tx(uint32_t now)
{
	dmr_emergency_init(NULL);	/* the defaults: the alarm, then the call with 10 s of hot mic, to the channel's target */
	lone_set(1, now);

	/* Sound only (the default): a long alarm asks for nothing, with every gate open */
	can_transmit = true;
	sos_air = true;
	CHECK(lone_on() == LONE_ON_SOUND);
	to_alarm(&now);
	CHECK(count(LONE_ACT_ALARM, &now, 60000u) == 20);
	CHECK(alarms == 0 && sos_starts == 0 && lone_tx() == LONE_TX_NONE && !ui_svc_emergency_active());
	CHECK(lone_activity(now, false) == LONE_ON_SOUND && alarm_cancels == 0 && sos_stops == 0);

	/* The picker: every action of the build, both ways */
	CHECK(lone_on_step(true) == LONE_ON_DMR && lone_on_step(true) == LONE_ON_SOS && lone_on_step(true) == LONE_ON_SOUND);
	CHECK(lone_on_step(false) == LONE_ON_SOS && lone_on_step(false) == LONE_ON_DMR);

	/* DMR emergency: nothing before the alarm (the warning never transmits), one start in a long alarm */
	lone_activity(now, true);
	CHECK(count(LONE_ACT_WARN, &now, 300000u + LONE_WARN_MS - 500u) == 3 && alarms == 0 && !ui_svc_emergency_active());
	CHECK(count(LONE_ACT_ALARM, &now, 1000u) == 1 && alarms == 1 && ui_svc_emergency_active());
	CHECK(count(LONE_ACT_ALARM, &now, 3000u) == 1 && lone_tx() == LONE_TX_DMR);
	alarm_busy = false;		/* the alarm's tries are out: the hot mic call, then in emergency until canceled */
	CHECK(count(LONE_ACT_ALARM, &now, 60000u) == 20 && alarms == 1 && lone_tx() == LONE_TX_DMR);
	CHECK(dmr_emergency_state() == DMR_EMERGENCY_ACTIVE);
	/* PTT ends the tone, not the emergency; the next key cancels it as the emergency key would */
	CHECK(lone_activity(now, true) == LONE_ON_SOUND && lone_state() == LONE_WATCH);
	radio_tick(now);
	CHECK(ui_svc_emergency_active() && alarm_cancels == 0);
	CHECK(lone_activity(now, false) == LONE_ON_DMR);
	radio_tick(now);
	CHECK(!ui_svc_emergency_active() && dmr_emergency_state() == DMR_EMERGENCY_IDLE && alarm_cancels == 1);
	CHECK(lone_activity(now, false) == LONE_ON_SOUND);	/* once */
	radio_tick(now);
	CHECK(alarm_cancels == 1);

	/* The next alarm starts it again (once), a key in the alarm cancels it while the alarm CSBKs still go out */
	to_alarm(&now);
	CHECK(alarms == 2 && dmr_emergency_state() == DMR_EMERGENCY_ALARM);
	CHECK(lone_activity(now, false) == LONE_ON_DMR);
	radio_tick(now);
	CHECK(!ui_svc_emergency_active() && alarm_cancels == 2 && !alarm_busy && lone_tx() == LONE_TX_NONE);

	/* A channel that can't (FM, RX only, TX inhibit, no ID: data_can_send false): the alarm goes on, the reason shows,
	 * nothing reaches the data service; a key then has nothing to cancel */
	can_transmit = false;
	to_alarm(&now);
	CHECK(alarms == 2 && !ui_svc_emergency_active());
	CHECK(count(LONE_ACT_ALARM, &now, 3000u) == 1 && lone_tx() == LONE_TX_NO_DMR);
	CHECK(count(LONE_ACT_ALARM, &now, 30000u) == 10 && alarms == 2 && lone_tx() == LONE_TX_NO_DMR);
	CHECK(lone_state() == LONE_ALARM && dmr_emergency_state() == DMR_EMERGENCY_FAILED);
	CHECK(lone_activity(now, false) == LONE_ON_SOUND);
	radio_tick(now);
	CHECK(alarm_cancels == 3 && alarms == 2);	/* the cancel also drops a request not taken yet */
	/* ... and once the channel can (the gate opens in the alarm) it goes, once */
	to_alarm(&now);
	CHECK(count(LONE_ACT_ALARM, &now, 9000u) == 3 && alarms == 2 && lone_tx() == LONE_TX_NO_DMR);
	can_transmit = true;
	CHECK(count(LONE_ACT_ALARM, &now, 60000u) == 20 && alarms == 3 && lone_tx() == LONE_TX_DMR);
	CHECK(lone_activity(now, false) == LONE_ON_DMR);
	radio_tick(now);
	CHECK(!ui_svc_emergency_active());

	/* The user's own emergency (the emergency key) running when the alarm comes: not started again, not ours to cancel */
	alarm_busy = false;
	CHECK(ui_svc_emergency(true));
	radio_tick(now);
	CHECK(alarms == 4 && ui_svc_emergency_active());
	to_alarm(&now);
	CHECK(alarms == 4 && lone_tx() == LONE_TX_DMR);
	CHECK(lone_activity(now, false) == LONE_ON_SOUND);
	radio_tick(now);
	CHECK(ui_svc_emergency_active());
	ui_svc_emergency(false);
	radio_tick(now);
	CHECK(!ui_svc_emergency_active());
	int alarms_dmr = alarms;

	/* APRS SOS: on at the alarm, "sent" once the engine has sent one, once per alarm, never the DMR emergency */
	pick(LONE_ON_SOS);
	sos_air = false;
	to_alarm(&now);
	CHECK(sos_starts == 1 && sos_on && lone_tx() == LONE_TX_WAIT);
	CHECK(count(LONE_ACT_ALARM, &now, 9000u) == 3 && lone_tx() == LONE_TX_WAIT);	/* e.g. a busy APRS frequency */
	sos_air = true;
	CHECK(count(LONE_ACT_ALARM, &now, 60000u) == 20 && lone_tx() == LONE_TX_SOS && sos_starts == 1);
	CHECK(lone_activity(now, true) == LONE_ON_SOUND && sos_on);		/* PTT: still on */
	CHECK(lone_activity(now, false) == LONE_ON_SOS && !sos_on && sos_stops == 1);
	CHECK(lone_activity(now, false) == LONE_ON_SOUND && sos_stops == 1);
	/* No position / APRS can't send: the alarm goes on with the reason; it starts when the gate opens */
	sos_block = APRS_NO_POSITION;
	to_alarm(&now);
	CHECK(lone_tx() == LONE_TX_NO_POS && sos_starts == 1 && !sos_on);
	sos_block = APRS_NOT_AMATEUR;
	CHECK(count(LONE_ACT_ALARM, &now, 9000u) == 3 && lone_tx() == LONE_TX_NO_APRS && sos_starts == 1 && !sos_on);
	CHECK(lone_activity(now, false) == LONE_ON_SOUND && sos_stops == 1);
	to_alarm(&now);
	sos_block = APRS_OK;
	CHECK(count(LONE_ACT_ALARM, &now, 6000u) == 2 && lone_tx() == LONE_TX_SOS && sos_starts == 2);
	CHECK(lone_activity(now, false) == LONE_ON_SOS && sos_stops == 2);
	/* The user's own SOS: left on */
	CHECK(aprs_sos_set(true) == APRS_OK && sos_starts == 3);
	to_alarm(&now);
	CHECK(sos_starts == 3 && (lone_tx() == LONE_TX_WAIT || lone_tx() == LONE_TX_SOS));
	CHECK(lone_activity(now, false) == LONE_ON_SOUND && sos_on && sos_stops == 2);
	aprs_sos_set(false);
	CHECK(alarms == alarms_dmr && !ui_svc_emergency_active());

	/* Turned off in the alarm's settings: back to Sound only, nothing more */
	pick(LONE_ON_SOUND);
	to_alarm(&now);
	CHECK(count(LONE_ACT_ALARM, &now, 30000u) == 10 && sos_starts == 3 && alarms == alarms_dmr);
	lone_set(0, now);
	printf("Plus lone worker alarm action: Sound only silent, DMR emergency and APRS SOS once per alarm, gates, "
	       "key cancel, PTT, the user's own: %d failures\n", failures);
}
#else
static void radio_tick(uint32_t now) { (void)now; }
#endif

int main(void)
{
	uint32_t now = 1000;
	CHECK(lone_state() == LONE_OFF && lone_tick(now) == LONE_ACT_NONE);
	lone_set(1, now);	/* 5 min */
	CHECK(lone_state() == LONE_WATCH && lone_minutes(1) == 5 && lone_left_ms(now) == 300000u);
	CHECK(count(LONE_ACT_WARN, &now, 299000u) == 0 && lone_state() == LONE_WATCH);
	/* a key half way starts the time again */
	lone_activity(now, false);
	CHECK(count(LONE_ACT_WARN, &now, 299000u) == 0);
	CHECK(count(LONE_ACT_WARN, &now, 2000u) == 1 && lone_state() == LONE_WARN);
	/* warnings every 10 s for 30 s, then the alarm every 3 s */
	CHECK(count(LONE_ACT_WARN, &now, 27500u) == 2);
	CHECK(count(LONE_ACT_ALARM, &now, 2000u) == 1 && lone_state() == LONE_ALARM);
	CHECK(count(LONE_ACT_ALARM, &now, 9000u) == 3);
	/* a key stops the alarm and watches again */
	CHECK(lone_activity(now, false) == LONE_ON_SOUND);
	CHECK(lone_state() == LONE_WATCH && lone_tick(now) == LONE_ACT_NONE && lone_tx() == LONE_TX_NONE);
	/* off: nothing ever */
	lone_set(0, now);
	CHECK(count(LONE_ACT_WARN, &now, 4000000u) == 0 && lone_state() == LONE_OFF);
	lone_set(99, now);
	CHECK(lone_choice() == 0);
	CHECK(lone_on() == LONE_ON_SOUND && lone_on_built(LONE_ON_SOUND) && !lone_on_built(LONE_ON_COUNT));
#ifndef LONE_TEST_TX
	/* a build without the DMR emergency and APRS messages offers Sound only */
	CHECK(lone_on_step(true) == LONE_ON_SOUND && lone_on_step(false) == LONE_ON_SOUND);
#endif
	printf("Plus lone worker: time, key restart, warnings, alarm, key stop, off: %d failures\n", failures);
#ifdef LONE_TEST_TX
	test_tx(now + 1000u);
#endif
	return failures ? 1 : 0;
}
