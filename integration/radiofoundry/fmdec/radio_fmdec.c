/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus tone finder and DTMF / SelCall decoder, radio task side (radio_fmdec.h). Radio task only, except the
 * readers and radio_selcall_set (any task).
 *
 * RAM: the tone finder's result and the SelCall list (6 x 28 bytes) are static; the decoder comes from the RTOS heap
 * while its screen listens (the tone finder about 0.7 KB, SelCall about 0.75 KB with FM caller ID's DTMF decoder and
 * the last DTMF block's audio).
 * CPU at 8 kHz: the tone finder's low pass 40 multiplies every 8th sample and 50 Goertzel steps at 1 kHz (about 0.2
 * M multiplies a second); SelCall FM caller ID's decoders (about 21 multiplies a sample) and a high pass and zero
 * crossing test a sample: each about 1 % of the core, only while its screen is open. */
#include "config.h"
#if CONFIG_TONE_FIND || CONFIG_SELCALL
#if !CONFIG_MSG_RX
#error "CONFIG_TONE_FIND and CONFIG_SELCALL need CONFIG_MSG_RX (radio_msg.c runs them)"
#endif
#include <string.h>
#include "radio/radio_fmdec.h"
#include "radio/radio_msg.h"
#if CONFIG_TONE_FIND
#include "radio/tonefind.h"
#endif
#if CONFIG_SELCALL
#include "radio/selcall.h"
#endif
#include "FreeRTOS.h"
#include "task.h"

#define CHIP_MS		250u	/* the FD6818's measurement registers read this often */
#define AGAIN_MS	3000u	/* the same sequence within this refreshes the newest entry */

typedef union {
#if CONFIG_TONE_FIND
	tonef_t tonef;
#endif
#if CONFIG_SELCALL
	selcall_t selcall;
#endif
} fmdec_t;

static fmdec_t *dec;		/* RTOS heap while a screen listens */
static uint8_t kind;		/* RADIO_MSG_TONEF / _SELCALL running, 0 none */
static uint32_t now, tuned_hz;
static volatile uint32_t seq;

__attribute__((weak)) bool board_css_read(uint16_t r[3])
{
	(void)r;
	return false;
}

/* --- tone finder --- */

#if CONFIG_TONE_FIND
static radio_tonef_info_t info;
static uint32_t chip_ms;

static void tonef_publish(const radio_tonef_info_t *n)
{
	if (memcmp(n, &info, sizeof *n) == 0)
		return;
	taskENTER_CRITICAL();
	info = *n;
	taskEXIT_CRITICAL();
	seq++;
}

static void tonef_result(const tonef_t *t)
{
	radio_tonef_info_t n = info;
	n.type = t->type;
	n.value = t->value;
	n.quality = t->quality;
	n.state = t->type != TONEF_NONE ? RADIO_TONEF_FOUND : t->idle >= 6u ? RADIO_TONEF_NONE : RADIO_TONEF_SEARCH;
	if (t->type != TONEF_NONE) {
		n.last_type = t->type;
		n.last_value = t->value;
	}
	tonef_publish(&n);
}

static void chip_poll(void)
{
#if TONEF_CHIP_READ
	if (now - chip_ms < CHIP_MS)
		return;
	chip_ms = now;
	radio_tonef_info_t n = info;
	n.chip_ok = board_css_read(n.chip);
	tonef_publish(&n);
#endif
}
#endif

void radio_tonef_info(radio_tonef_info_t *out)
{
#if CONFIG_TONE_FIND
	taskENTER_CRITICAL();
	*out = info;
	taskEXIT_CRITICAL();
#else
	memset(out, 0, sizeof *out);
#endif
}

/* --- SelCall list --- */

#if CONFIG_SELCALL
static radio_selcall_t list[RADIO_SELCALL_N];
static volatile uint8_t head, count;	/* head: the next entry written */
static volatile uint8_t only;

static void on_seq(void *ctx, const selcall_seq_t *s)
{
	(void)ctx;
	uint32_t at = now != 0 ? now : 1u;
	if (count > 0) {
		radio_selcall_t *last = &list[(head + RADIO_SELCALL_N - 1u) % RADIO_SELCALL_N];
		if (last->set == s->set && strcmp(last->text, s->text) == 0 && now - last->ms < AGAIN_MS) {
			last->ms = at;	/* sent twice (many radios do) */
			seq++;
			return;
		}
	}
	radio_selcall_t e;
	memset(&e, 0, sizeof e);
	e.ms = at;
	e.rx_hz = tuned_hz;
	e.set = s->set;
	for (size_t i = 0; i < RADIO_SELCALL_TEXT && s->text[i] != '\0'; i++)
		e.text[i] = s->text[i];
	taskENTER_CRITICAL();
	list[head] = e;
	head = (uint8_t)((head + 1u) % RADIO_SELCALL_N);
	if (count < RADIO_SELCALL_N)
		count++;
	taskEXIT_CRITICAL();
	seq++;
}
#endif

bool radio_selcall_get(uint8_t i, radio_selcall_t *out)
{
#if CONFIG_SELCALL
	bool ok = false;
	taskENTER_CRITICAL();
	if (i < count) {
		*out = list[(head + RADIO_SELCALL_N - 1u - i) % RADIO_SELCALL_N];
		ok = true;
	}
	taskEXIT_CRITICAL();
	return ok;
#else
	(void)i;
	(void)out;
	return false;
#endif
}

uint8_t radio_selcall_count(void)
{
#if CONFIG_SELCALL
	return count;
#else
	return 0;
#endif
}

void radio_selcall_set(uint8_t set)
{
#if CONFIG_SELCALL
	only = set < SELCALL_SETS ? set : SELCALL_ANY;
#else
	(void)set;
#endif
}

uint8_t radio_selcall_only(void)
{
#if CONFIG_SELCALL
	return only;
#else
	return 0;
#endif
}

/* --- running --- */

bool radio_fmdec_start(uint8_t k, uint32_t rx_hz)
{
	size_t size = 0;
#if CONFIG_TONE_FIND
	if (k == RADIO_MSG_TONEF)
		size = sizeof(tonef_t);
#endif
#if CONFIG_SELCALL
	if (k == RADIO_MSG_SELCALL)
		size = sizeof(selcall_t);
#endif
	if (size == 0)
		return false;
	fmdec_t *d = pvPortMalloc(size);
#if CONFIG_TONE_FIND
	if (k == RADIO_MSG_TONEF) {
		radio_tonef_info_t n = info;
		n.nomem = d == NULL;
		n.state = RADIO_TONEF_OFF;
		tonef_publish(&n);
		if (d != NULL)
			tonef_init(&d->tonef, TONEF_DCS_INVERT != 0);
	}
#endif
#if CONFIG_SELCALL
	if (k == RADIO_MSG_SELCALL && d != NULL)
		selcall_init(&d->selcall, only, on_seq, NULL);
#endif
	if (d == NULL)
		return false;
	dec = d;
	kind = k;
	tuned_hz = rx_hz;
	return true;
}

void radio_fmdec_stop(void)
{
	if (dec == NULL)
		return;
#if CONFIG_SELCALL
	if (kind == RADIO_MSG_SELCALL)
		selcall_flush(&dec->selcall);	/* a sequence or DTMF string being received */
#endif
#if CONFIG_TONE_FIND
	if (kind == RADIO_MSG_TONEF) {
		radio_tonef_info_t n = info;
		n.state = RADIO_TONEF_OFF;
		n.type = 0;
		n.value = 0;
		n.quality = 0;
		n.chip_ok = false;
		tonef_publish(&n);
	}
#endif
	vPortFree(dec);
	dec = NULL;
	kind = 0;
}

void radio_fmdec_audio(const int16_t *s, uint16_t n)
{
	if (dec == NULL)
		return;
#if CONFIG_TONE_FIND
	if (kind == RADIO_MSG_TONEF) {
		tonef_audio(&dec->tonef, s, n);
		tonef_result(&dec->tonef);
	}
#endif
#if CONFIG_SELCALL
	if (kind == RADIO_MSG_SELCALL)
		selcall_audio(&dec->selcall, s, n);
#endif
}

void radio_fmdec_poll(uint32_t now_ms)
{
	now = now_ms;
	if (dec == NULL)
		return;
#if CONFIG_TONE_FIND
	if (kind == RADIO_MSG_TONEF)
		chip_poll();
#endif
#if CONFIG_SELCALL
	if (kind == RADIO_MSG_SELCALL && dec->selcall.only != only)
		selcall_only(&dec->selcall, only);
#endif
}

uint32_t radio_fmdec_seq(void)
{
	return seq;
}
#endif
