/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus SSTV receive (CONFIG_SSTV, docs/plus.md), radio task side. Installed as core/radio/radio_sstv.{c,h} when
 * CONFIG_SSTV=y (needs CONFIG_MSG_RX: radio_msg.c runs it like the pager).
 *
 * Tools > SSTV asks for it (radio_msg_want(RADIO_MSG_SSTV)); radio_msg.c starts it on an FM channel FM caller ID
 * listens on, with the board's flat receive path on (as for the pager: SSTV is FM audio tones, and the flat path keeps
 * the 1100..2300 Hz tones' level even), and hands it FM caller ID's audio. The decoder (sstv.h) and a ring of
 * finished display rows come from the RTOS heap while the screen listens; leaving the screen or the channel frees
 * them. The screen keeps no picture: each row is drawn into the display's frame buffer once, as it arrives.
 *
 * Rows: the decoder's picture lines scaled to the display (SSTV_W across, at most SSTV_ROWS_MAX rows), each pixel
 * already one of the SSTV_COLOURS (sstv_dither), two to a byte (high nibble first). A row the screen did not take in
 * time is lost (it draws black); with the UI's 100 ms passes and a row every 200 ms or more that does not happen.
 *
 * RAM: about 40 bytes static; heap while the screen listens: sstv_t (about 1.1 KB) and the ring (3 rows, 366 bytes).
 * CPU: the decoder's ~30 multiplies and a divide a sample, a row's dither once a line. */
#ifndef RADIO_SSTV_H
#define RADIO_SSTV_H

#include <stdbool.h>
#include <stdint.h>
#include "radio/sstv.h"

#define RADIO_SSTV_ROW_BYTES	(SSTV_W / 2u)

typedef struct {
	uint8_t state;		/* SSTV_IDLE / _RX / _DONE / _LOST */
	uint8_t mode;		/* SSTV_NONE: none yet */
	uint8_t image;		/* changes with each new image (the screen clears the picture) */
	uint8_t vis_unknown;	/* a VIS for a mode not decoded (0: none) */
	bool nomem;		/* the last start found no heap */
	uint16_t line, lines;	/* picture lines received, of */
	int16_t ppm;		/* the sender's clock against ours (the fit's line time; 0 unknown) */
} radio_sstv_info_t;

/* Radio task (radio_msg.c) */
bool radio_sstv_start(void);
void radio_sstv_stop(void);
void radio_sstv_audio(const int16_t *s, uint16_t n);

/* Any task */
void radio_sstv_info(radio_sstv_info_t *out);
/* changes with each row and each change of the info */
uint32_t radio_sstv_seq(void);
/* UI task: the oldest row of image `image` not yet taken (rows of an older image are dropped): its display row and
 * pixels. False: none */
bool radio_sstv_row(uint8_t image, uint8_t *row, uint8_t px[RADIO_SSTV_ROW_BYTES]);

#endif
