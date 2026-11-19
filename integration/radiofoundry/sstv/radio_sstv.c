/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus SSTV receive, radio task side (radio_sstv.h). Radio task only, except the readers. */
#include "config.h"
#if CONFIG_SSTV
#if !CONFIG_MSG_RX
#error "CONFIG_SSTV needs CONFIG_MSG_RX (radio_msg.c runs it)"
#endif
#include <string.h>
#include "radio/radio_sstv.h"
#include "FreeRTOS.h"
#include "task.h"

#define RING_N	3u

typedef struct {
	sstv_t dec;
	uint16_t last_row;	/* the display row handed on last (0xFFFF: none of this image) */
	uint8_t images;		/* dec.images as last seen */
	uint8_t row_y[RING_N], row_img[RING_N];
	uint8_t px[RING_N][RADIO_SSTV_ROW_BYTES];
} rx_t;

static rx_t *rx;		/* RTOS heap while the screen listens */
static volatile uint8_t head, tail, count;	/* the ring: head (next written) the radio task, tail the UI */
static radio_sstv_info_t info;
static uint8_t image;		/* the images seen, over every start */
static volatile uint32_t seq;

/* a new image from the decoder: a new number (the screen clears its picture), no row of it handed on yet */
static void image_check(rx_t *r)
{
	if (r->dec.images == r->images)
		return;
	r->images = r->dec.images;
	r->last_row = 0xFFFFu;
	image++;
}

static void on_row(void *ctx, const sstv_t *d, uint16_t line, uint8_t sub)
{
	rx_t *r = ctx;
	image_check(r);
	uint16_t lines = sstv_mode_lines(d->mode), rows = sstv_mode_rows(d->mode);
	if (lines == 0)
		return;
	uint16_t y = (uint16_t)((uint32_t)line * rows / lines);
	if (y == r->last_row || y >= SSTV_ROWS_MAX)
		return;		/* more picture lines than display rows: the first of each row's lines */
	r->last_row = y;
	uint8_t h = head;
	if (count >= RING_N)
		return;		/* the screen is behind: the row is lost */
	uint8_t *px = r->px[h];
	for (uint16_t x = 0; x < SSTV_W; x += 2u) {
		uint8_t rgb[3];
		sstv_rgb(d, sub, x, rgb);
		uint8_t a = sstv_dither(rgb, x, y);
		sstv_rgb(d, sub, (uint16_t)(x + 1u), rgb);
		px[x / 2u] = (uint8_t)(a << 4 | sstv_dither(rgb, (uint16_t)(x + 1u), y));
	}
	r->row_y[h] = (uint8_t)y;
	r->row_img[h] = image;
	taskENTER_CRITICAL();	/* the row before the count (and a barrier) */
	head = (uint8_t)((h + 1u) % RING_N);
	count++;
	taskEXIT_CRITICAL();
	seq++;
}

static void publish(const sstv_t *d)
{
	radio_sstv_info_t now = info;
	now.state = d->state;
	now.mode = d->mode;
	now.image = image;
	now.vis_unknown = d->vis_unknown;
	now.lines = sstv_mode_lines(d->mode);
	now.line = d->lines != 0 ? (uint16_t)(d->line * (now.lines / d->lines)) : 0;
	if (now.line > now.lines)
		now.line = now.lines;
	now.ppm = (int16_t)sstv_clock_ppm(d);
	if (memcmp(&now, &info, sizeof now) == 0)
		return;
	taskENTER_CRITICAL();
	info = now;
	taskEXIT_CRITICAL();
	seq++;
}

bool radio_sstv_start(void)
{
	rx_t *r = pvPortMalloc(sizeof *r);
	if (r == NULL) {
		if (!info.nomem) {
			info.nomem = true;
			seq++;
		}
		return false;
	}
	sstv_init(&r->dec, on_row, r);
	r->last_row = 0xFFFFu;
	r->images = 0;
	taskENTER_CRITICAL();
	rx = r;
	head = tail = count = 0;
	info.nomem = false;
	taskEXIT_CRITICAL();
	publish(&r->dec);
	return true;
}

void radio_sstv_stop(void)
{
	taskENTER_CRITICAL();
	rx_t *r = rx;
	rx = NULL;
	head = tail = count = 0;
	if (info.state == SSTV_RX)
		info.state = SSTV_LOST;	/* the screen closed or the channel changed while an image came */
	taskEXIT_CRITICAL();
	if (r != NULL)
		vPortFree(r);
	seq++;
}

void radio_sstv_audio(const int16_t *s, uint16_t n)
{
	rx_t *r = rx;
	if (r == NULL)
		return;
	sstv_audio(&r->dec, s, n);
	image_check(r);
	publish(&r->dec);
}

void radio_sstv_info(radio_sstv_info_t *out)
{
	taskENTER_CRITICAL();
	*out = info;
	taskEXIT_CRITICAL();
}

uint32_t radio_sstv_seq(void)
{
	return seq;
}

bool radio_sstv_row(uint8_t img, uint8_t *row, uint8_t px[RADIO_SSTV_ROW_BYTES])
{
	bool ok = false;
	taskENTER_CRITICAL();
	while (!ok && rx != NULL && count != 0) {
		uint8_t i = tail;
		if (rx->row_img[i] == img) {
			*row = rx->row_y[i];
			memcpy(px, rx->px[i], RADIO_SSTV_ROW_BYTES);
			ok = true;
		}
		tail = (uint8_t)((i + 1u) % RING_N);
		count--;
	}
	taskEXIT_CRITICAL();
	return ok;
}
#endif
