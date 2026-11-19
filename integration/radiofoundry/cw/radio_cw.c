/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus CW decoder, radio task side (radio_cw.h). Radio task only, except the readers and radio_cw_clear. */
#include "config.h"
#if CONFIG_CW_RX
#if !CONFIG_MSG_RX
#error "CONFIG_CW_RX needs CONFIG_MSG_RX (radio_msg.c runs it)"
#endif
#include <string.h>
#include "radio/radio_cw.h"
#include "radio/cwdec.h"
#include "FreeRTOS.h"
#include "task.h"

static cwdec_t *dec;		/* RTOS heap while the screen listens */
static char ring[RADIO_CW_TEXT];
static uint32_t written, cleared;	/* characters ever written; the first one after the last clear */
static radio_cw_info_t info;
static volatile uint32_t seq;

static void on_text(void *ctx, const char *s)
{
	(void)ctx;
	taskENTER_CRITICAL();
	for (; *s != '\0'; s++)
		ring[written++ % RADIO_CW_TEXT] = *s;
	taskEXIT_CRITICAL();
	seq++;
}

static void publish(void)
{
	radio_cw_info_t now = info;
	now.on = dec != NULL;
	now.key = dec != NULL && cwdec_key(dec) && cwdec_tone(dec) != 0;
	now.tone_hz = dec != NULL ? cwdec_tone(dec) : 0;
	if (dec != NULL && cwdec_wpm(dec) != 0)
		now.wpm = cwdec_wpm(dec);	/* the last speed stays shown after a stop */
	if (memcmp(&now, &info, sizeof now) == 0)
		return;
	taskENTER_CRITICAL();
	info = now;
	taskEXIT_CRITICAL();
	seq++;
}

bool radio_cw_start(void)
{
	cwdec_t *d = pvPortMalloc(sizeof *d);
	if (d == NULL) {
		if (!info.nomem) {
			info.nomem = true;
			seq++;
		}
		return false;
	}
	cwdec_init(d, on_text, NULL);
	taskENTER_CRITICAL();
	dec = d;
	info.nomem = false;
	info.wpm = 0;
	taskEXIT_CRITICAL();
	publish();
	return true;
}

void radio_cw_stop(void)
{
	cwdec_t *d = dec;
	if (d != NULL)
		cwdec_flush(d);		/* the character being received */
	taskENTER_CRITICAL();
	dec = NULL;
	taskEXIT_CRITICAL();
	if (d != NULL)
		vPortFree(d);
	publish();
}

void radio_cw_audio(const int16_t *s, uint16_t n)
{
	if (dec == NULL)
		return;
	cwdec_audio(dec, s, n);
	publish();
}

void radio_cw_info(radio_cw_info_t *out)
{
	taskENTER_CRITICAL();
	*out = info;
	taskEXIT_CRITICAL();
}

uint32_t radio_cw_seq(void)
{
	return seq;
}

uint32_t radio_cw_written(void)
{
	return written;
}

static uint32_t first(void)
{
	uint32_t f = written > RADIO_CW_TEXT ? written - RADIO_CW_TEXT : 0;
	return cleared > f ? cleared : f;
}

uint32_t radio_cw_first(void)
{
	taskENTER_CRITICAL();
	uint32_t f = first();
	taskEXIT_CRITICAL();
	return f;
}

uint16_t radio_cw_copy(uint32_t from, char *out, uint16_t n)
{
	uint16_t k = 0;
	taskENTER_CRITICAL();
	uint32_t f = first();
	if (from < f)
		from = f;
	for (; k < n && from + k < written; k++)
		out[k] = ring[(from + k) % RADIO_CW_TEXT];
	taskEXIT_CRITICAL();
	out[k] = '\0';
	return k;
}

void radio_cw_clear(void)
{
	taskENTER_CRITICAL();
	cleared = written;
	taskEXIT_CRITICAL();
	seq++;
}
#endif
