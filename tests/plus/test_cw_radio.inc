/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus CW practice in the real radio core (radio_sm.c with CONFIG_CW_RX=1, the real beep.c, cw_train.c), on
 * radioCoreTest's fake board, after upstream's and Classic's radio suites (tests/classic/test_radio.inc) have run
 * again with it built in. Included after them by tools/test_classic.py (test_radio_cw).
 *
 * The practice page's request is taken at the radio's poll and played by the beep generator: the beep's own PCM
 * stream is run here as the audio driver runs it (a 20 ms block at a time) and what reaches the speaker is decoded
 * by the CW decoder (cwdec.c) and timed. Checked: the text played is the text asked for; its length from the first to
 * the last tone sample is the Morse's (units x dot) to a sample; no TX request of any kind at any point (no tx(), no FM
 * TX tone, no tone keying, no DTMF, no PTT, the state never TX) over a long practice at every speed; a request while
 * transmitting is refused and plays nothing; key beeps while it plays are left out (they would cut it); key beeps off
 * in Settings (level 0) still play it. */
#include "radio/beep.h"
#include "radio/cw_train.h"
#include "radio/cwdec.h"

static char cwr_got[512];
static size_t cwr_n;
static cwdec_t cwr_dec;
static int64_t cwr_first, cwr_last;	/* samples: the first and last with a tone, -1 none */
static int64_t cwr_samples;

static void cwr_text(void *ctx, const char *s)
{
	(void)ctx;
	while (*s && cwr_n + 1 < sizeof cwr_got)
		cwr_got[cwr_n++] = *s++;
	cwr_got[cwr_n] = 0;
}

static void cwr_reset(void)
{
	cwr_n = 0;
	cwr_got[0] = 0;
	cwr_first = cwr_last = -1;
	cwr_samples = 0;
	cwdec_init(&cwr_dec, cwr_text, NULL);
}

/* ms of radio polls; every 20 ms the stream's block (when one runs) goes to the speaker, here the decoder */
static void cwr_run(uint32_t ms)
{
	int16_t mic[AUDIO_BLOCK_SAMPLES] = { 0 }, spk[AUDIO_BLOCK_SAMPLES];
	for (uint32_t t = 0; t < ms; t += 20u) {
		poll_for(20);
		memset(spk, 0, sizeof spk);
		if (aud.started && aud.cb != NULL)
			aud.cb(mic, spk, aud.ctx);
		for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
			if (spk[i] > 200 || spk[i] < -200) {
				if (cwr_first < 0)
					cwr_first = cwr_samples + i;
				cwr_last = cwr_samples + i;
			}
		}
		cwr_samples += AUDIO_BLOCK_SAMPLES;
		cwdec_audio(&cwr_dec, spk, AUDIO_BLOCK_SAMPLES);
	}
}

/* Nothing that transmits happened since the marks */
static int cwr_tx0;
static bool cwr_no_tx(void)
{
	return hw.tx_calls == cwr_tx0 && tone.hz == 0 && tone.changes == 0 && dtmf_on == 0 && !sm.ptt &&
	       sm.view.state != RS_TX;
}

static void test_cw_practice_radio(void)
{
	classic_busy = classic_stop = classic_receiving = false;	/* no native P25 call (the beep would be mixed into it) */
	setup_codeplug();
	start();
	now += 1000;
	radio_sm_poll(&sm, now);
	memset(&tone, 0, sizeof tone);
	dtmf_on = 0;
	cwr_tx0 = hw.tx_calls;

	/* PARIS at 20 WPM, 700 Hz: what the speaker plays decodes to it, and lasts 43 units (PARIS without the word
	 * space) of 60 ms, to the sample (the 2 ms fades at the ends aside) */
	cwr_reset();
	cw_train_play("PARIS", 5, 20, 700);
	CHECK(cw_train_state() == CW_PLAY_PENDING);
	cwr_run(5000);
	cwdec_flush(&cwr_dec);
	CHECK(cw_train_state() == CW_PLAY_PLAYING);
	CHECK(strstr(cwr_got, "PARIS") != NULL);
	int64_t span = cwr_last - cwr_first + 1, want = (int64_t)cw_train_ms("PARIS", 5, 20) * 8;
	CHECK(cwr_first >= 0 && span <= want && span >= want - 2 * 16);
	CHECK(want == 43 * 60 * 8);
	CHECK(cwr_no_tx() && !aud.pcm && !aud.started);	/* the stream and the speaker route given back after */
	printf("Plus CW practice: PARIS at 20 WPM played as \"%s\", %lld of %lld samples, no TX\n", cwr_got,
	       (long long)span, (long long)want);

	/* Every speed and tone of the range, groups of Koch characters: decoded back, never a TX request */
	uint32_t rng = 12345;
	int played = 0, right = 0;
	for (unsigned wpm = CW_TRAIN_WPM_MIN; wpm <= CW_TRAIN_WPM_MAX; wpm += 5) {
		for (unsigned hz = CW_TRAIN_HZ_MIN; hz <= CW_TRAIN_HZ_MAX; hz += 300) {
			char g[CW_TRAIN_GROUP + 1];
			cw_train_group(&rng, CW_TRAIN_KOCH_N, CW_TRAIN_GROUP, g);
			g[CW_TRAIN_GROUP] = 0;
			cwr_reset();
			cw_train_play(g, CW_TRAIN_GROUP, (uint8_t)wpm, (uint16_t)hz);
			cwr_run(cw_train_ms(g, CW_TRAIN_GROUP, (uint8_t)wpm) + 4000u);
			cwdec_flush(&cwr_dec);
			played++;
			right += strstr(cwr_got, g) != NULL;
			if (strstr(cwr_got, g) == NULL)
				printf("  %u WPM %u Hz: sent %s, heard \"%s\"\n", wpm, hz, g, cwr_got);
		}
	}
	CHECK(right == played && cwr_no_tx());
	printf("Plus CW practice: %d groups at 5-40 WPM, 400-1000 Hz decoded back (%d right), no TX request\n", played,
	       right);

	/* Key beeps while it plays are left out (a key beep would replace the Morse); after it they sound again */
	beep_stats_t b0, b1;
	cwr_reset();
	cw_train_play("TEST", 4, 15, 600);
	cwr_run(300);
	beep_get_stats(&b0);
	CHECK(beep_morse_playing());
	req(RREQ_BEEP, BEEP_KEY);
	req(RREQ_BEEP, BEEP_KEY);
	beep_get_stats(&b1);
	CHECK(b1.played == b0.played);
	cwr_run(4000);
	cwdec_flush(&cwr_dec);
	CHECK(strstr(cwr_got, "TEST") != NULL && !beep_morse_playing());
	req(RREQ_BEEP, BEEP_KEY);
	beep_get_stats(&b1);
	CHECK(b1.played == b0.played + 1);
	cwr_run(500);

	/* Beeps off in Settings (level 0): the practice still sounds */
	uint8_t level = sm.set.beep_level;
	sm.set.beep_level = 0;
	cwr_reset();
	cw_train_play("K", 1, 20, 700);
	cwr_run(1500);
	cwdec_flush(&cwr_dec);
	CHECK(strstr(cwr_got, "K") != NULL);
	sm.set.beep_level = level;
	CHECK(cwr_no_tx());

	/* While the radio transmits: refused, nothing played, the transmission left as it was */
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX && hw.tx_calls == cwr_tx0 + 1);
	beep_get_stats(&b0);
	cwr_reset();
	cw_train_play("SOS", 3, 20, 700);
	cwr_run(200);
	beep_get_stats(&b1);
	CHECK(cw_train_state() == CW_PLAY_REFUSED && b1.played == b0.played && sm.view.state == RS_TX &&
	      hw.tx_calls == cwr_tx0 + 1);
	req(RREQ_PTT, 0);
	cwr_run(2000);
	cwdec_flush(&cwr_dec);
	CHECK(cwr_got[0] == 0 && sm.view.state != RS_TX);
	printf("Plus CW practice: key beeps left out while it plays, level 0 still plays, refused while transmitting\n");
}

int main(void)
{
	int prior = classic_radio_main();
	int before = failures;
	test_cw_practice_radio();
	printf("Plus CW practice in the radio core: %d failures (upstream's and Classic's suites with it built in: %s)\n",
	       failures - before, prior ? "FAILED" : "passed");
	return prior != 0 || failures != before;
}
