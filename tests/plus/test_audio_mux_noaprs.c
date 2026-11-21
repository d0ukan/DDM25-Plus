/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus (docs/dota.md, phase 2 step 0): the shared PCM stream (core/usbaudio/audio_mux.c) built with
 * PLUS_AUDIO_MUX_NO_APRS (and PLUS_AUDIO_MUX_NO_HOST, as the Plus image): APRS's receive ring is gone, so asking for it
 * neither starts the board's stream nor delivers samples, while the radio's own user of the stream is unchanged. The
 * DM-32 reads APRS audio from its own AFSK receive path (hrc_afsk rx_*), never from here. */
#include <stdio.h>
#include <string.h>
#include "usbaudio/audio_mux.h"

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static audio_block_cb_t hw_cb;
static void *hw_ctx;
static int hw_starts, hw_stops;
static bool hw_start(audio_block_cb_t cb, void *ctx) { hw_cb = cb; hw_ctx = ctx; hw_starts++; return true; }
static void hw_stop(void) { hw_cb = NULL; hw_stops++; }
static void hw_speaker_pcm(bool on) { (void)on; }
static const hal_audio_t board_audio = { .start = hw_start, .stop = hw_stop, .speaker_pcm = hw_speaker_pcm };

static int user_blocks;
static void user(const int16_t *mic, int16_t *spk, void *ctx)
{
	(void)mic;
	(void)ctx;
	user_blocks++;
	memset(spk, 0, AUDIO_BLOCK_SAMPLES * sizeof *spk);
}

int main(void)
{
#ifndef PLUS_AUDIO_MUX_NO_APRS
	printf("build this test with PLUS_AUDIO_MUX_NO_APRS\n");
	return 1;
#else
	audio_mux_reset();
	const hal_audio_t *a = audio_mux_wrap(&board_audio);
	CHECK(a != NULL);
	int16_t pcm[2 * AUDIO_BLOCK_SAMPLES];
	/* APRS asks for the ring: nothing starts, nothing comes */
	audio_mux_aprs_rx(true);
	audio_mux_service(10);
	CHECK(hw_starts == 0 && hw_cb == NULL);
	CHECK(audio_mux_aprs_read(pcm, AUDIO_BLOCK_SAMPLES) == 0);
	/* A user's stream runs as before; its microphone blocks are not kept for APRS */
	CHECK(a->start(user, NULL));
	CHECK(hw_starts == 1 && hw_cb != NULL);
	int16_t mic[AUDIO_BLOCK_SAMPLES], spk[AUDIO_BLOCK_SAMPLES];
	for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
		mic[i] = (int16_t)(100 + i);
	for (int k = 0; k < 3; k++)
		if (hw_cb != NULL)
			hw_cb(mic, spk, hw_ctx);
	CHECK(user_blocks == 3);
	CHECK(audio_mux_aprs_read(pcm, 2 * AUDIO_BLOCK_SAMPLES) == 0);
	/* The user stops: APRS does not keep the stream (it has no ring here) */
	a->stop();
	audio_mux_service(20);
	CHECK(hw_stops == 1 && hw_cb == NULL);
	audio_mux_aprs_rx(false);
	printf("audio mux without the APRS ring: %d failures\n", failures);
	return failures != 0;
#endif
}
