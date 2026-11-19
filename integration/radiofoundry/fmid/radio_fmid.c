/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus FM caller ID, radio task side (radio_fmid.h). Radio task only, except radio_fmid_shown / _logged.
 *
 * RAM: the decoders' state (fmid_t, about 220 bytes) and two IDs, static; the 2 KB receive ring comes from the RTOS
 * heap while an FM channel listens (as APRS's does). CPU: per 8 kHz sample a high pass, a band pass, one
 * discriminator multiply and eight Goertzel steps (two 32-bit multiplies each), about 21 multiplies and roughly 150
 * cycles: about 1.2 MHz, under 1 % of the 168 MHz core, in the radio task's 25 ms polls (200 samples each). */
#include "config.h"
#if CONFIG_FM_ID
#if !CONFIG_APRS_RX
#error "CONFIG_FM_ID needs CONFIG_APRS_RX (the FM receive audio ring)"
#endif
#include <string.h>
#include "radio/radio_fmid.h"
#include "radio/fmid.h"
#include "board.h"
#include "FreeRTOS.h"
#include "task.h"
#include "aprs/aprs.h"
#if CONFIG_MSG_RX
#include "radio/radio_msg.h"	/* DM32 Plus pager: it hears the same audio */
#endif

#define SETTLE_MS	300u	/* an FM channel held this long before the ring starts (dual watch hops, scan steps) */
#define RETRY_MS	1000u	/* a start that failed (no heap) is tried again after this */
#define SHOW_MS		5000u	/* an MDC / FleetSync ID stays on Home */
#define DTMF_END_MS	4000u	/* DTMF: this long without a digit ends the string (logged, Home clears) */
#define READ_N		32u

static fmid_t dec;
static bool wanted, own_ring, news;
static volatile bool id_off;	/* Settings > Talk & listen > FM caller ID off (UI task) */
static bool decoding;		/* the decoders hear the audio (FM caller ID on while the ring is wanted) */
static uint32_t want_since, retry_at, tuned_hz, now, shown_at, last_digit_at;
static radio_fmid_id_t shown, logged;
static uint32_t logged_seq;

static void publish(const radio_fmid_id_t *id, bool log)
{
	taskENTER_CRITICAL();
	shown = *id;
	if (log) {
		logged = *id;
		logged_seq++;
	}
	taskEXIT_CRITICAL();
	shown_at = now;
	news = true;
}

static void clear_shown(void)
{
	if (shown.kind == 0)
		return;
	taskENTER_CRITICAL();
	memset(&shown, 0, sizeof shown);
	taskEXIT_CRITICAL();
	news = true;
}

/* The DTMF string so far went quiet (or the channel changed): into the call history, off Home */
static void dtmf_end(void)
{
	if (shown.kind != RADIO_FMID_DTMF)
		return;
	taskENTER_CRITICAL();
	logged = shown;
	logged_seq++;
	memset(&shown, 0, sizeof shown);
	taskEXIT_CRITICAL();
	news = true;
}

static void on_event(void *ctx, const fmid_event_t *e)
{
	(void)ctx;
	radio_fmid_id_t id;
	memset(&id, 0, sizeof id);
	if (e->kind == FMID_DTMF) {
		if (shown.kind == RADIO_FMID_DTMF)
			id = shown;
		else
			id.kind = RADIO_FMID_DTMF;
		size_t n = strlen(id.dtmf);
		if (n >= RADIO_FMID_DTMF_MAX) {
			memmove(id.dtmf, id.dtmf + 1, RADIO_FMID_DTMF_MAX - 1u);	/* keep the newest */
			n = RADIO_FMID_DTMF_MAX - 1u;
		}
		id.dtmf[n] = e->digit;
		id.dtmf[n + 1] = '\0';
		last_digit_at = now;
		publish(&id, false);
		return;
	}
	dtmf_end();	/* an ID burst after digits: the digits are done */
	id.kind = e->kind == FMID_MDC_EMERG ? RADIO_FMID_MDC_EMERG :
		  e->kind == FMID_FLEETSYNC ? RADIO_FMID_FLEETSYNC : RADIO_FMID_MDC;
	id.unit = e->unit;
	id.fleet = e->fleet;
	/* the same ID again (MDC at key-up and release, FleetSync's repeats) only keeps it on Home */
	bool again = shown.kind == id.kind && shown.unit == id.unit && shown.fleet == id.fleet;
	publish(&id, !again);
}

static void ring_stop(void)
{
	if (!own_ring)
		return;
	if (board.afsk->rx_stop != NULL)
		board.afsk->rx_stop();
	own_ring = false;
}

void radio_fmid_yield(void)
{
	ring_stop();
}

void radio_fmid_audio(const int16_t *s, uint16_t n)
{
	if (wanted && !own_ring) {
		if (decoding)
			fmid_audio(&dec, s, n);
#if CONFIG_MSG_RX
		radio_msg_audio(s, n);
#endif
	}
}

void radio_fmid_enable(bool on)
{
	id_off = !on;
}

bool radio_fmid_poll(bool want, uint32_t rx_hz, uint32_t now_ms)
{
	now = now_ms;
	news = false;
	bool id_on = !id_off;
	/* FM caller ID off: the ring only for the Pager / SSTV screen (they hear this audio), else the direct path */
#if CONFIG_MSG_RX
	want = radio_fmid_ring_wanted(want, id_on, radio_msg_wants_fm());
#else
	want = radio_fmid_ring_wanted(want, id_on, false);
#endif
	if (!want) {
		ring_stop();
		wanted = false;
		decoding = false;
		dtmf_end();
		clear_shown();
		return news;
	}
	if (!wanted || rx_hz != tuned_hz || id_on != decoding) {
		/* listening starts, another frequency, or the setting changed: decoders from scratch, nothing of the last
		 * one shown */
		if (wanted)
			dtmf_end();
		clear_shown();
		fmid_init(&dec, on_event, NULL);
		tuned_hz = rx_hz;
		decoding = id_on;
		if (!wanted)
			want_since = now_ms;
		wanted = true;
	}
	const hal_afsk_t *a = board.afsk;
	if (!own_ring && !aprs_rx_listening() && now_ms - want_since >= SETTLE_MS &&
	    (retry_at == 0 || (int32_t)(now_ms - retry_at) >= 0)) {
		retry_at = 0;
		if (a == NULL || a->rx_start == NULL || a->rx_read == NULL || a->rx_rate_hz != FMID_FS) {
			retry_at = now_ms + 60000u;	/* no receive audio on this board */
		} else if (a->rx_start()) {
			own_ring = true;
		} else {
			retry_at = now_ms + RETRY_MS;
		}
	}
	if (own_ring) {
		int16_t buf[READ_N];
		for (unsigned guard = 0; guard < 64u; guard++) {
			uint16_t n = a->rx_read(buf, READ_N);
			if (n == 0)
				break;
			if (decoding)
				fmid_audio(&dec, buf, n);
#if CONFIG_MSG_RX
			radio_msg_audio(buf, n);
#endif
		}
	}
	if (shown.kind == RADIO_FMID_DTMF) {
		if (now_ms - last_digit_at >= DTMF_END_MS)
			dtmf_end();
	} else if (shown.kind != 0 && now_ms - shown_at >= SHOW_MS) {
		clear_shown();
	}
	return news;
}

static size_t put(char *out, size_t size, size_t at, const char *s)
{
	while (*s != '\0' && at + 1u < size)
		out[at++] = *s++;
	out[at] = '\0';
	return at;
}

static size_t put_u(char *out, size_t size, size_t at, uint32_t v)
{
	char d[11];
	int n = 0;
	do {
		d[n++] = (char)('0' + v % 10u);
		v /= 10u;
	} while (v != 0 && n < 10);
	while (n > 0 && at + 1u < size)
		out[at++] = d[--n];
	out[at] = '\0';
	return at;
}

void radio_fmid_text(uint8_t kind, uint16_t unit, uint16_t fleet, const char *dtmf, char *out, size_t size)
{
	if (size == 0)
		return;
	out[0] = '\0';
	size_t at;
	if (kind == RADIO_FMID_MDC || kind == RADIO_FMID_MDC_EMERG) {
		/* Motorola radios show the unit ID as four hex digits */
		at = put(out, size, 0, "MDC ");
		for (int shift = 12; shift >= 0 && at + 1u < size; shift -= 4)
			out[at++] = "0123456789ABCDEF"[(unit >> shift) & 15u];
		out[at] = '\0';
	} else if (kind == RADIO_FMID_FLEETSYNC) {
		at = put(out, size, 0, "FleetSync ");
		at = put_u(out, size, at, fleet);
		at = put(out, size, at, "-");
		put_u(out, size, at, unit);
	} else if (kind == RADIO_FMID_DTMF) {
		at = put(out, size, 0, "DTMF ");
		put(out, size, at, dtmf != NULL ? dtmf : "");
	}
}

void radio_fmid_shown(radio_fmid_id_t *out)
{
	taskENTER_CRITICAL();
	*out = shown;
	taskEXIT_CRITICAL();
}

uint32_t radio_fmid_logged(radio_fmid_id_t *out)
{
	taskENTER_CRITICAL();
	uint32_t seq = logged_seq;
	if (out != NULL)
		*out = logged;
	taskEXIT_CRITICAL();
	return seq;
}
#endif
