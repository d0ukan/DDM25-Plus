/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus DMR receive audio diagnostics, round 2 (tools/plus_dmraudio.py): the decoded voice's digital gain in the
 * generated core/dmr/ambe/ambeVoice.c, with the real FEC, a fake audio driver and a fake vocoder whose decode writes a
 * square wave of a known amplitude. x1 leaves every sample as it was; x2, x4, x8 multiply, saturate at the 16-bit
 * limits and count the clipped samples; the peak after the gain is the call's (a new call starts it again); the beeps'
 * mixer runs after the gain (their level stays); the reset zeroes the counters and keeps the gain; the shift is 0..3. */
#include <stdio.h>
#include <string.h>

#include "dmr/ambe/ambeVoice.h"
#include "dmr/ambe/ambeFec.h"

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static audio_block_cb_t cb;
static bool fake_start(audio_block_cb_t c, void *ctx) { (void)ctx; cb = c; return true; }
static void fake_stop(void) { cb = NULL; }
static void fake_speaker(bool on) { (void)on; }
static const hal_audio_t audio = { .start = fake_start, .stop = fake_stop, .speaker_pcm = fake_speaker };

static int16_t amp = 1000;	/* the decoded square wave: +amp, -amp, ... */
static bool fake_present(void) { return true; }
static void fake_reset(void) { }
static void fake_encode(const int16_t pcm[VOCODER_FRAME_SAMPLES], uint8_t bits[VOCODER_BITS])
{
	(void)pcm;
	memset(bits, 0, VOCODER_BITS);
}
static void fake_decode(const uint8_t bits[VOCODER_BITS], int16_t pcm[VOCODER_FRAME_SAMPLES])
{
	(void)bits;
	for (int i = 0; i < VOCODER_FRAME_SAMPLES; i++)
		pcm[i] = (i & 1) ? (int16_t)-amp : amp;
}
static const hal_vocoder_t vocoder = { fake_present, fake_reset, fake_encode, fake_decode, NULL, NULL, NULL, NULL };

static bool beep_on;
static bool mix(int16_t *spk)
{
	if (!beep_on)
		return false;
	for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
		spk[i] = (int16_t)(spk[i] + 100);
	return true;
}

static int16_t mic[AUDIO_BLOCK_SAMPLES], spk[AUDIO_BLOCK_SAMPLES];

/* A call: begin, frames enough to prime and play, the end */
static void call_start(void)
{
	const dmr_voice_sink_t *s = ambeVoiceSink();
	uint8_t bits[VOCODER_BITS], frame[DMR_AMBE_FRAME_BYTES];
	memset(bits, 0, sizeof bits);
	ambeFecEncode(bits, frame);
	s->begin(NULL, s->ctx);
	for (int n = 0; n < 6; n++)
		s->frame(frame, s->ctx);
}

static void block(void)
{
	memset(spk, 0, sizeof spk);
	cb(mic, spk, NULL);
}

static void call_end(void)
{
	const dmr_voice_sink_t *s = ambeVoiceSink();
	s->end(s->ctx);
	for (int i = 0; i < 8; i++)
		block();
}

int main(void)
{
	uint32_t clipped, peak;
	ambeVoiceInit(&audio, &vocoder);
	ambeVoiceSetMixer(mix);
	CHECK(ambeVoiceRxGain() == 1);	/* x2 at power-on */
	ambeVoiceSetRxGain(0);

	/* x1: the decoder's samples as they were */
	call_start();
	block();
	CHECK(spk[0] == 1000 && spk[1] == -1000);
	ambeVoiceRxLevel(&clipped, &peak);
	CHECK(clipped == 0 && peak == 1000);

	/* x2, x4 during the same call: doubled, quadrupled, no clipping; the peak follows */
	ambeVoiceSetRxGain(1);
	block();
	CHECK(spk[0] == 2000 && spk[1] == -2000);
	ambeVoiceSetRxGain(2);
	block();
	CHECK(spk[0] == 4000 && spk[1] == -4000);
	ambeVoiceRxLevel(&clipped, &peak);
	CHECK(clipped == 0 && peak == 4000);
	call_end();

	/* x8 on a loud decoder (6,000 x 8 = 48,000): saturated at both limits, every sample counted; a new call's peak */
	ambeVoiceSetRxGain(3);
	CHECK(ambeVoiceRxGain() == 3);
	amp = 6000;
	call_start();
	block();
	CHECK(spk[0] == 32767 && spk[1] == -32768);
	ambeVoiceRxLevel(&clipped, &peak);
	CHECK(clipped == AUDIO_BLOCK_SAMPLES && peak == 32768);

	/* The beeps are mixed after the gain: their 100 is not multiplied */
	amp = 1000;
	beep_on = true;
	block();
	CHECK(spk[0] == 8100 && spk[1] == -7900);
	beep_on = false;
	call_end();

	/* The reset zeroes the counters, keeps the gain; the shift is two bits */
	ambeVoiceResetStats();
	ambeVoiceRxLevel(&clipped, &peak);
	CHECK(clipped == 0 && peak == 0 && ambeVoiceRxGain() == 3);
	ambeVoiceSetRxGain(0xFE);
	CHECK(ambeVoiceRxGain() == 2);
	ambeVoiceSetRxGain(0);
	call_start();
	block();
	CHECK(spk[0] == 1000);
	call_end();

	printf("DMR receive voice gain: %d failures\n", failures);
	return failures != 0;
}
