/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus: the shared PCM stream (core/usbaudio/audio_mux.c) without its host audio side
 * (PLUS_AUDIO_MUX_NO_HOST, tools/plus_features.py prepare_audio_mux), against a fake board stream. Built twice by
 * tools/test_classic.py: with the macro (Plus) and without it (upstream, with pcm_slip.c); the radio's side (the
 * primary user's blocks, the speaker route, the APRS receive ring, start / stop) must be the same in both, and only
 * the upstream build passes samples to a host. */
#include <stdio.h>
#include <string.h>
#include "usbaudio/audio_mux.h"

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* The board's stream: one callback, started and stopped */
static audio_block_cb_t hw_cb;
static void *hw_ctx;
static int hw_starts, hw_stops, hw_speaker;
static bool hw_start(audio_block_cb_t cb, void *ctx) { hw_cb = cb; hw_ctx = ctx; hw_starts++; return true; }
static void hw_stop(void) { hw_cb = NULL; hw_stops++; }
static void hw_speaker_pcm(bool on) { hw_speaker = on ? 1 : 0; }
static const hal_audio_t board_audio = { .start = hw_start, .stop = hw_stop, .speaker_pcm = hw_speaker_pcm };

/* The radio's user of the stream (DMR voice, P25, a beep): sees the mic, writes the speaker */
static int user_blocks;
static int16_t user_mic0;
static bool user_on;
static void user(const int16_t *mic, int16_t *spk, void *ctx)
{
	(void)ctx;
	user_blocks++;
	user_mic0 = mic[0];
	for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
		spk[i] = (int16_t)(1000 + i);
	}
}

static void run_block(int16_t base)
{
	int16_t mic[AUDIO_BLOCK_SAMPLES], spk[AUDIO_BLOCK_SAMPLES];
	for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
		mic[i] = (int16_t)(base + i);
	}
	memset(spk, 0x55, sizeof spk);
	CHECK(hw_cb != NULL);
	if (hw_cb != NULL) {
		hw_cb(mic, spk, hw_ctx);
	}
	if (user_on) {
		CHECK(spk[0] == 1000 && spk[AUDIO_BLOCK_SAMPLES - 1] == 1000 + AUDIO_BLOCK_SAMPLES - 1);
	} else {
		CHECK(spk[0] == 0 && spk[AUDIO_BLOCK_SAMPLES - 1] == 0);	/* nobody's: silence */
	}
}

int main(void)
{
	audio_mux_reset();
	const hal_audio_t *a = audio_mux_wrap(&board_audio);
	CHECK(a != NULL && a->start && a->stop && a->speaker_pcm);

	/* The radio's user: its blocks see the board's microphone, the speaker gets its output */
	CHECK(a->start(user, NULL) && hw_starts == 1);
	user_on = true;
	a->speaker_pcm(true);
	CHECK(hw_speaker == 1);
	run_block(100);
	CHECK(user_blocks == 1 && user_mic0 == 100);

	/* A host session asked for: upstream passes the stream's output on, Plus has no host side */
	audio_mux_host_rx(AUDIO_MUX_RX_OUTPUT);
	audio_mux_host_tx(true, true);
	int16_t host[AUDIO_BLOCK_SAMPLES];
	for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
		host[i] = (int16_t)(-7 - i);
	}
	audio_mux_host_push(host, AUDIO_BLOCK_SAMPLES);
	run_block(200);
	int16_t got[AUDIO_BLOCK_SAMPLES];
	memset(got, 0, sizeof got);
#ifdef PLUS_AUDIO_MUX_NO_HOST
	CHECK(user_mic0 == 200);	/* the microphone, never host audio */
	CHECK(audio_mux_host_avail() == 0 && audio_mux_host_read(got, AUDIO_BLOCK_SAMPLES) == 0);
	CHECK(audio_mux_host_tx_fill() == 0 && audio_mux_host_pushes() == 0);
	int16_t fm[40];
	memset(fm, 0x55, sizeof fm);
	audio_mux_fm_pull(fm, 40);
	for (int i = 0; i < 40; i++) {
		CHECK(fm[i] == 0);
	}
	pcm_slip_stats_t ss;
	memset(&ss, 0x55, sizeof ss);
	audio_mux_slip_stats(&ss);
	const uint8_t *z = (const uint8_t *)&ss;
	for (size_t i = 0; i < sizeof ss; i++) {
		CHECK(z[i] == 0);
	}
#else
	CHECK(audio_mux_host_avail() == AUDIO_BLOCK_SAMPLES);
	CHECK(audio_mux_host_read(got, AUDIO_BLOCK_SAMPLES) == AUDIO_BLOCK_SAMPLES && got[0] == 1000);
	CHECK(audio_mux_host_pushes() == 1);
#endif
	audio_mux_host_tx(false, false);
	audio_mux_host_rx(AUDIO_MUX_RX_OFF);

	/* APRS's receive ring takes the board's microphone (FM receive audio) block by block, in both builds */
	audio_mux_aprs_rx(true);
	run_block(300);
	run_block(300 + AUDIO_BLOCK_SAMPLES);
	int16_t ring[2 * AUDIO_BLOCK_SAMPLES];
	CHECK(audio_mux_aprs_read(ring, 2 * AUDIO_BLOCK_SAMPLES) == 2 * AUDIO_BLOCK_SAMPLES);
	for (int i = 0; i < 2 * AUDIO_BLOCK_SAMPLES; i++) {
		CHECK(ring[i] == 300 + i);
	}
	CHECK(audio_mux_aprs_read(ring, AUDIO_BLOCK_SAMPLES) == 0);

	/* The user stops: APRS keeps the board's stream, silence for the speaker */
	a->stop();
	user_on = false;
	CHECK(hw_stops == 0 && hw_cb != NULL);
	run_block(500);
	CHECK(audio_mux_aprs_read(ring, AUDIO_BLOCK_SAMPLES) == AUDIO_BLOCK_SAMPLES && ring[0] == 500);
	/* APRS off: the radio task's service stops the board's stream (no host session keeps it, in either build) */
	audio_mux_aprs_rx(false);
	audio_mux_service(10);
	CHECK(hw_stops == 1 && hw_cb == NULL);
	/* A host session asked for again: Plus never starts the stream for it */
	audio_mux_host_rx(AUDIO_MUX_RX_INPUT);
	audio_mux_service(20);
#ifdef PLUS_AUDIO_MUX_NO_HOST
	CHECK(hw_starts == 1 && hw_cb == NULL);
#else
	CHECK(hw_starts == 2 && hw_cb != NULL);
#endif
	audio_mux_host_rx(AUDIO_MUX_RX_OFF);
	audio_mux_service(30);
	CHECK(hw_cb == NULL);

	audio_mux_stats_t st;
	audio_mux_stats(&st);
	CHECK(st.blocks == 5);
#ifdef PLUS_AUDIO_MUX_NO_HOST
	CHECK(st.rx_samples == 0 && st.tx_blocks == 0 && st.tx_pushes == 0);
#else
	CHECK(st.rx_samples == AUDIO_BLOCK_SAMPLES && st.tx_blocks == 1);
#endif
	printf("audio mux (%s): %d failures\n",
#ifdef PLUS_AUDIO_MUX_NO_HOST
	       "DM32 Plus, no host side",
#else
	       "upstream",
#endif
	       failures);
	return failures != 0;
}
