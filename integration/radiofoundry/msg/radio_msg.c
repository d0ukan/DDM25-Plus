/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus pager and ACARS receive, radio task side (radio_msg.h). Radio task only, except radio_msg_want (UI task)
 * and the readers (any task). */
#include "config.h"
#if CONFIG_MSG_RX
#if !CONFIG_FM_ID || !CONFIG_APRS_RX
#error "CONFIG_MSG_RX needs CONFIG_FM_ID and CONFIG_APRS_RX (the FM receive audio)"
#endif
#include <string.h>
#include "radio/radio_msg.h"
#include "radio/pocsag.h"
#include "radio/acars.h"
#include "board.h"
#include "FreeRTOS.h"
#include "task.h"
#include "aprs/aprs.h"
#if CONFIG_NXDN
#include "radio/radio_nxdn.h"
#endif
#if CONFIG_SSTV
#include "radio/radio_sstv.h"
#endif
#if CONFIG_TONE_FIND || CONFIG_SELCALL
#include "radio/radio_fmdec.h"
#endif
#if CONFIG_CW_RX
#include "radio/radio_cw.h"
static bool cw_am;		/* the CW decoder hears an AM channel (the ring is ours) */
#endif

/* The kinds that listen where the pager does, on FM caller ID's audio (the other one: ACARS) */
static bool fm_audio_kind(uint8_t k)
{
	return k == RADIO_MSG_PAGER || k == RADIO_MSG_SSTV || k == RADIO_MSG_TONEF || k == RADIO_MSG_SELCALL;
}

/* ... and of those the ones that need the flat receive path (not DTMF / SelCall: voice band tones) */
static bool flat_kind(uint8_t k)
{
	return k == RADIO_MSG_PAGER || k == RADIO_MSG_SSTV || k == RADIO_MSG_TONEF;
}

#define SETTLE_MS	300u	/* an AM channel held this long before the ring starts (dual watch hops, scan steps) */
#define RETRY_MS	1000u	/* a start that failed (no heap, the ring busy) is tried again after this */
#define FLAT_KEEP_MS	1000u	/* the flat path is checked again this often (a retune sets the band-pass again) */
#define AUDIO_GONE_MS	1000u	/* no audio this long: "waiting" again */
#define AGAIN_MS	60000u	/* the same message within this: the newest entry is refreshed, not added again */
#define READ_N		32u
#define SSTV_RETUNE_HZ	10000u	/* SSTV: a retune this small goes on with the picture */

typedef union {
	pocsag_t pocsag;
	acars_t acars;
} msg_dec_t;

static msg_dec_t *dec;		/* RTOS heap while a decoder runs */
static volatile uint8_t wanted;	/* the screen open (UI task) */
static uint8_t running;		/* the decoder running, 0 = none */
static uint8_t status;
static bool own_ring, flat;
static uint32_t now, tuned_hz, since_ms, start_retry_ms, ring_retry_ms, flat_ms, audio_ms;
static volatile uint32_t seq;
static uint16_t next_id;

static radio_pager_msg_t pager[RADIO_MSG_N];
static radio_acars_msg_t acars[RADIO_MSG_N];
static volatile uint8_t pager_head, pager_n, acars_head, acars_n;	/* head: the next entry written */

static uint32_t fnv(uint32_t h, const void *p, size_t n)
{
	const uint8_t *b = p;
	while (n-- > 0) {
		h ^= *b++;
		h *= 16777619u;
	}
	return h;
}

/* 1-0x7FFF: the message screen keeps the kind in its argument's top bit */
static uint16_t new_id(void)
{
	next_id = (uint16_t)((next_id + 1u) & 0x7FFFu);
	if (next_id == 0)
		next_id = 1;
	return next_id;
}

static uint32_t stamp(void)
{
	return now != 0 ? now : 1u;
}

/* --- decoder callbacks (radio task, inside the decoders) --- */

static void on_pager(void *ctx, const pocsag_msg_t *m)
{
	(void)ctx;
	uint32_t h = fnv(2166136261u, &m->capcode, sizeof m->capcode);
	h = fnv(h, &m->function, 1);
	h = fnv(h, m->bits, m->nbits / 8u);
	if (m->nbits % 8u != 0) {
		uint8_t last = (uint8_t)(m->bits[m->nbits / 8u] & (0xFF00u >> (m->nbits % 8u)));
		h = fnv(h, &last, 1);
	}
	uint16_t hash = (uint16_t)(h ^ (h >> 16) ^ m->nbits);
	if (pager_n > 0) {
		radio_pager_msg_t *last = &pager[(pager_head + RADIO_MSG_N - 1u) % RADIO_MSG_N];
		if (last->hash == hash && last->capcode == m->capcode && now - last->ms < AGAIN_MS) {
			last->ms = stamp();
			last->flags |= RADIO_MSG_AGAIN;
			seq++;
			return;
		}
	}
	/* the oldest entry (shown last, if the list is full) is rewritten; the readers' text always ends in a NUL at
	 * its last byte, and the new number tells the screen to draw again */
	radio_pager_msg_t *p = &pager[pager_head];
	p->kind = pocsag_text(m, p->text, sizeof p->text);
	taskENTER_CRITICAL();
	p->ms = stamp();
	p->capcode = m->capcode;
	p->id = new_id();
	p->hash = hash;
	p->function = m->function;
	p->rate = m->rate;
	p->flags = (uint8_t)((m->errors ? RADIO_MSG_ERRORS : 0u) | (m->cut ? RADIO_MSG_CUT : 0u));
	pager_head = (uint8_t)((pager_head + 1u) % RADIO_MSG_N);
	if (pager_n < RADIO_MSG_N)
		pager_n++;
	seq++;
	taskEXIT_CRITICAL();
}

static void copy_str(char *dst, size_t size, const char *src)
{
	size_t n = 0;
	while (src[n] != '\0' && n + 1u < size) {
		dst[n] = src[n];
		n++;
	}
	dst[n] = '\0';
}

static void on_acars(void *ctx, const acars_msg_t *m)
{
	(void)ctx;
	uint32_t h = fnv(2166136261u, m->reg, sizeof m->reg);
	h = fnv(h, m->label, sizeof m->label);
	h = fnv(h, &m->block, 1);
	h = fnv(h, m->msn, sizeof m->msn);
	h = fnv(h, m->text, m->n);
	uint16_t hash = (uint16_t)(h ^ (h >> 16));
	if (acars_n > 0) {
		radio_acars_msg_t *last = &acars[(acars_head + RADIO_MSG_N - 1u) % RADIO_MSG_N];
		if (last->hash == hash && now - last->ms < AGAIN_MS) {
			last->ms = stamp();
			last->flags |= RADIO_MSG_AGAIN;
			seq++;
			return;
		}
	}
	radio_acars_msg_t *a = &acars[acars_head];
	copy_str(a->text, sizeof a->text, m->text);
	taskENTER_CRITICAL();
	a->ms = stamp();
	a->id = new_id();
	a->hash = hash;
	copy_str(a->reg, sizeof a->reg, m->reg);
	copy_str(a->flight, sizeof a->flight, m->flight);
	copy_str(a->label, sizeof a->label, m->label);
	a->mode = m->mode;
	a->block = m->block;
	a->flags = (uint8_t)((m->crc_ok ? 0u : RADIO_MSG_ERRORS) | (m->cut ? RADIO_MSG_CUT : 0u) |
			     (m->more ? RADIO_MSG_MORE : 0u));
	acars_head = (uint8_t)((acars_head + 1u) % RADIO_MSG_N);
	if (acars_n < RADIO_MSG_N)
		acars_n++;
	seq++;
	taskEXIT_CRITICAL();
}

/* --- running --- */

static void flat_set(bool on)
{
#if CONFIG_NXDN
	if (on)
		flat = board_nxdn_flat(true);
	else if (flat)
		flat = !board_nxdn_flat(false);
#else
	(void)on;
#endif
}

static void ring_stop(void)
{
	if (!own_ring)
		return;
	if (board.afsk->rx_stop != NULL)
		board.afsk->rx_stop();
	own_ring = false;
}

static void stop(void)
{
	if (running == RADIO_MSG_PAGER && dec != NULL)
		pocsag_flush(&dec->pocsag);	/* a page being received when the screen closed */
#if CONFIG_SSTV
	if (running == RADIO_MSG_SSTV)
		radio_sstv_stop();
#endif
#if CONFIG_TONE_FIND || CONFIG_SELCALL
	if (running == RADIO_MSG_TONEF || running == RADIO_MSG_SELCALL)
		radio_fmdec_stop();
#endif
#if CONFIG_CW_RX
	if (running == RADIO_MSG_CW)
		radio_cw_stop();
#endif
	ring_stop();
	flat_set(false);
	if (dec != NULL) {
		vPortFree(dec);
		dec = NULL;
	}
	running = 0;
}

static bool start(uint8_t kind, uint32_t rx_hz)
{
#if CONFIG_SSTV
	if (kind == RADIO_MSG_SSTV) {
		if (!radio_sstv_start())	/* its own decoder and rows (radio_sstv.c), no message decoder */
			return false;
	} else
#endif
#if CONFIG_TONE_FIND || CONFIG_SELCALL
	if (kind == RADIO_MSG_TONEF || kind == RADIO_MSG_SELCALL) {
		if (!radio_fmdec_start(kind, rx_hz))	/* its own decoder (radio_fmdec.c) */
			return false;
	} else
#endif
#if CONFIG_CW_RX
	if (kind == RADIO_MSG_CW) {
		if (!radio_cw_start())		/* its own decoder and text (radio_cw.c) */
			return false;
	} else
#endif
	{
		dec = pvPortMalloc(sizeof *dec);
		if (dec == NULL)
			return false;
	}
	if (kind == RADIO_MSG_PAGER)
		pocsag_init(&dec->pocsag, on_pager, NULL);
	else if (kind == RADIO_MSG_ACARS)
		acars_init(&dec->acars, on_acars, NULL);
	running = kind;
	tuned_hz = rx_hz;
	since_ms = now;
	audio_ms = 0;
	ring_retry_ms = 0;
	flat = false;
	return true;
}

static void acars_service(uint32_t now_ms)
{
	const hal_afsk_t *a = board.afsk;
	if (!own_ring && !aprs_rx_listening() && now_ms - since_ms >= SETTLE_MS &&
	    (ring_retry_ms == 0 || (int32_t)(now_ms - ring_retry_ms) >= 0)) {
		ring_retry_ms = 0;
		if (a == NULL || a->rx_start == NULL || a->rx_read == NULL || a->rx_rate_hz != ACARS_FS)
			ring_retry_ms = now_ms + 60000u;	/* no receive audio on this board */
		else if (a->rx_start())
			own_ring = true;
		else
			ring_retry_ms = now_ms + RETRY_MS;
	}
	if (!own_ring)
		return;
	int16_t buf[READ_N];
	for (unsigned guard = 0; guard < 64u; guard++) {
		uint16_t n = a->rx_read(buf, READ_N);
		if (n == 0)
			break;
#if CONFIG_CW_RX
		if (running == RADIO_MSG_CW)
			radio_cw_audio(buf, n);	/* the CW decoder on an AM channel: the same ring */
		else
#endif
		acars_audio(&dec->acars, buf, n);
		audio_ms = stamp();
	}
}

bool radio_msg_poll(uint8_t suits, uint32_t rx_hz, uint32_t now_ms)
{
	static uint32_t told;
	now = now_ms;
	uint8_t want = wanted;
	/* SSTV, the tone finder and DTMF / SelCall listen where the pager would */
	uint8_t kind = want != 0 && (want == suits || (fm_audio_kind(want) && suits == RADIO_MSG_PAGER)) ? want : 0;
#if CONFIG_CW_RX
	/* CW listens where the pager would (FM caller ID's audio) and where ACARS would (AM: the ring itself); FM <-> AM on
	 * the same frequency (Tools > Air band) starts it again on the other path */
	if (want == RADIO_MSG_CW && (suits == RADIO_MSG_PAGER || suits == RADIO_MSG_ACARS))
		kind = RADIO_MSG_CW;
	if (running == RADIO_MSG_CW && kind == RADIO_MSG_CW && (suits == RADIO_MSG_ACARS) != cw_am)
		stop();
	if (running == 0)
		cw_am = suits == RADIO_MSG_ACARS;
#endif
	/* SSTV keeps its picture through small retunes (Satellites' Doppler steps during an ISS pass) */
	if (running == RADIO_MSG_SSTV && rx_hz != tuned_hz &&
	    (rx_hz > tuned_hz ? rx_hz - tuned_hz : tuned_hz - rx_hz) <= SSTV_RETUNE_HZ)
		tuned_hz = rx_hz;
	if (running != 0 && (kind != running || rx_hz != tuned_hz))
		stop();		/* the screen closed, or another channel: the decoder (and the flat path) from scratch */
	if (kind != 0 && running == 0 && (start_retry_ms == 0 || (int32_t)(now_ms - start_retry_ms) >= 0))
		start_retry_ms = start(kind, rx_hz) ? 0 : now_ms + RETRY_MS;
	if (flat_kind(running)) {
		if (!flat || now_ms - flat_ms >= FLAT_KEEP_MS) {
			flat_ms = now_ms;
			flat_set(true);
		}
	} else if (running == RADIO_MSG_ACARS) {
		acars_service(now_ms);
	}
#if CONFIG_CW_RX
	else if (running == RADIO_MSG_CW && cw_am) {
		acars_service(now_ms);
	}
#endif
#if CONFIG_TONE_FIND || CONFIG_SELCALL
	radio_fmdec_poll(now_ms);
#endif
	uint8_t st = running == 0 ? RADIO_MSG_ST_OFF :
		     audio_ms != 0 && now_ms - audio_ms < AUDIO_GONE_MS ? RADIO_MSG_ST_ON : RADIO_MSG_ST_WAIT;
	if (st != status) {
		status = st;
		seq++;
	}
	bool news = seq != told;
	told = seq;
	return news;
}

void radio_msg_audio(const int16_t *s, uint16_t n)
{
#if CONFIG_SSTV
	if (running == RADIO_MSG_SSTV) {
		radio_sstv_audio(s, n);
		audio_ms = stamp();
		return;
	}
#endif
#if CONFIG_TONE_FIND || CONFIG_SELCALL
	if (running == RADIO_MSG_TONEF || running == RADIO_MSG_SELCALL) {
		radio_fmdec_audio(s, n);
		audio_ms = stamp();
		return;
	}
#endif
#if CONFIG_CW_RX
	if (running == RADIO_MSG_CW) {
		if (!cw_am) {
			radio_cw_audio(s, n);
			audio_ms = stamp();
		}
		return;
	}
#endif
	if (running != RADIO_MSG_PAGER || dec == NULL)
		return;
	pocsag_audio(&dec->pocsag, s, n);
	audio_ms = stamp();
}

void radio_msg_want(uint8_t kind)
{
	wanted = kind;
}

bool radio_msg_wants_fm(void)
{
#if CONFIG_CW_RX
	if (wanted == RADIO_MSG_CW)
		return true;	/* Tools > CW decoder (on an AM channel FM caller ID doesn't listen anyway) */
#endif
	return fm_audio_kind(wanted);
}

uint8_t radio_msg_status(void)
{
	return status;
}

uint32_t radio_msg_seq(void)
{
	return seq;
}

uint8_t radio_msg_count(uint8_t kind)
{
	return kind == RADIO_MSG_PAGER ? pager_n : kind == RADIO_MSG_ACARS ? acars_n : 0;
}

const radio_pager_msg_t *radio_msg_pager(uint8_t i)
{
	if (i >= pager_n)
		return NULL;
	return &pager[(pager_head + RADIO_MSG_N - 1u - i) % RADIO_MSG_N];
}

const radio_acars_msg_t *radio_msg_acars(uint8_t i)
{
	if (i >= acars_n)
		return NULL;
	return &acars[(acars_head + RADIO_MSG_N - 1u - i) % RADIO_MSG_N];
}
#endif
