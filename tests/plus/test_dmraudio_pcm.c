/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus DMR receive audio diagnostics (tools/plus_dmraudio.py, pcm_diag.inc): the real generated PCM driver
 * (platform/hrc7000/audio_pcm.c with pcm_playback.inc and pcm_diag.inc), its 2 ms FIFO timer and its audio task, against
 * a model of the HR_C7000 prompt FIFO that plays 8 samples a millisecond and holds two 32-sample chunks.
 *
 * A DMR call is simulated as the radio makes it: a block every 20 ms, the speaker route turned on inside the first
 * block's callback (rx_block's speaker(true)), each callback taking its own time (the vocoder's decode). With the first
 * played block decoded fast and later ones slower, the shipped feeding (mode 0) runs the FIFO dry: the model hears
 * gaps, and the driver's gap counter sees them. With the prefill (mode 1, 3) the same call plays without a gap. Also:
 * the latency counters, a stream of one block (the prefill's time-out), the paced mode applied while running, the
 * reset, and that mode 0 never holds the FIFO back. */
#define PLATFORM_HRC7000_H
#define HRC_DMR_GLUE_H
#define pdFALSE 0
#define portYIELD_FROM_ISR(x) ((void)(x))
#define IRQ_PCM_WR 28
#define IRQ_TIMER6 6
#include PCM_TIMER_INC
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "modem/hrc7000/hrc7000.h"
#include "FreeRTOS.h"

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint32_t now_us;
static void (*timer_isr)(void);
static uint32_t timer_period;
static bool run_one;
static unsigned task_waits;
static jmp_buf task_done;
static void hrc_timer_ack(uint8_t n) { (void)n; }
static void hrc_timer_periodic(uint8_t n, uint32_t us) { (void)n; timer_period = us; }
static void hrc_timer_stop(uint8_t n) { (void)n; timer_period = 0; }
static uint32_t hrc_micros(void) { return now_us; }
static uint32_t hrc_irq_save(void) { return 0; }
static void hrc_irq_restore(uint32_t s) { (void)s; }
static void hrc_irq_enable(unsigned irq, bool on) { (void)irq; (void)on; }
static void hrc_irq_register(unsigned irq, void (*fn)(void)) { if (irq == IRQ_TIMER6) timer_isr = fn; }
static void hrc_barrier(void) { }
static bool hrc_dmr_tx_active(void) { return false; }
void hrc7000_set_crit(hrc7000_t *d, uint32_t (*a)(void), void (*b)(uint32_t)) { d->crit_enter = a; d->crit_exit = b; }
void hrc7000_audio_bits(hrc7000_t *d, uint32_t set, uint32_t clear) { d->audio_ctl = (d->audio_ctl & ~clear) | set; }
void vTaskNotifyGiveFromISR(TaskHandle_t t, BaseType_t *w) { (void)t; (void)w; }
BaseType_t xTaskCreate(void (*fn)(void *), const char *n, unsigned s, void *c, unsigned p, TaskHandle_t *h)
{ (void)fn; (void)n; (void)s; (void)c; (void)p; *h = (void *)1; return pdPASS; }
uint32_t ulTaskNotifyTake(BaseType_t c, TickType_t t)
{ (void)c; (void)t; if (run_one && task_waits++) longjmp(task_done, 1); return 0; }
void xTaskNotifyGive(TaskHandle_t t) { (void)t; }

/* --- The prompt FIFO model: the driver's pushes land here (pcm_playback.inc's playback_push) --- */
#define FIFO_CAP	64u	/* two chunks: AUDIO_CONTROL b31 (ring_full_two) */
static uint32_t fifo_n;		/* samples in the FIFO */
static bool fifo_started;	/* the first sample played */
static uint32_t model_gaps, model_gap_steps;	/* the FIFO ran dry while the call had more to play */
static bool model_dry;
static int16_t last_sample;
static uint32_t order_errors;
static bool test_prompt_push(hrc7000_t *d, const int16_t *s)
{
	(void)d;
	if (fifo_n + C7_PROMPT_CHUNK > FIFO_CAP)
		return false;
	fifo_n += C7_PROMPT_CHUNK;
	for (unsigned i = 0; i < C7_PROMPT_CHUNK; i++) {
		if (fifo_started && s[i] != (int16_t)(last_sample + 1))
			order_errors++;
		last_sample = s[i];
		fifo_started = true;
	}
	return true;
}
#define hrc7000_prompt_push test_prompt_push
#include AUDIO_PCM_C
#undef hrc7000_prompt_push

static unsigned route_calls;
static void fake_speaker(bool on) { (void)on; route_calls++; }

/* Time moves: the FIFO plays one sample each 125 us, the timer interrupt every 2 ms */
static bool call_live;		/* more of the call is still to come: a dry FIFO is a gap */
static void advance(uint32_t us)
{
	for (uint32_t t = 0; t < us; t += 125u) {
		now_us += 125u;
		if (fifo_n > 0) {
			fifo_n--;
			model_dry = false;
		} else if (fifo_started && call_live) {
			model_gap_steps++;
			if (!model_dry)
				model_gaps++;
			model_dry = true;
		}
		if (now_us % 2000u == 0 && timer_isr != NULL && timer_period != 0)
			timer_isr();
	}
}

/* One block of the call: its decode takes cb_us; the first turns the route on, as rx_block does */
static uint32_t cb_us;
static unsigned cb_count;
static int16_t next_sample = 1;
static void call_block(const int16_t *mic, int16_t *spk, void *ctx)
{
	(void)mic;
	(void)ctx;
	if (cb_count++ == 0)
		speaker_pcm(true);
	advance(cb_us);
	for (unsigned i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
		spk[i] = next_sample++;
}

static void one_block(void)
{
	mic_ready = 0;
	pd_ready_us = now_us;	/* isr_pcm_wr's stamp (its loop reads the modem's SRAM) */
	task_waits = 0;
	run_one = true;
	if (!setjmp(task_done))
		audio_task(NULL);
	run_one = false;
}

static uint32_t regs[1024];
static uint8_t buf[4096];
static hrc7000_t chip = { .regs = regs, .buf = buf };

static void fresh(uint8_t mode)
{
	stop();
	speaker_pcm(false);
	fifo_n = 0;
	fifo_started = model_dry = false;
	model_gaps = model_gap_steps = order_errors = 0;
	last_sample = 0;
	next_sample = 1;
	cb_count = 0;
	call_live = true;
	now_us = (now_us / 20000u + 1u) * 20000u;
	hrc_audio_pcm_set_mode(mode);
	hrc_audio_pcm_reset_stats();
	CHECK(start(call_block, NULL));
}

/* A call of n blocks: the first played block (the second, the first is dropped by the route change) decodes in
 * fast_us, the later ones alternate between fast_us and slow_us */
static void call(unsigned n, uint32_t fast_us, uint32_t slow_us)
{
	for (unsigned k = 0; k < n; k++) {
		cb_us = k < 2 ? fast_us : (k & 1u) ? slow_us : fast_us;
		one_block();
		if (k + 1 == n)
			call_live = false;
		advance(20000u - cb_us);
	}
	advance(60000u);
}

int main(void)
{
	hrc_audio_pcm_attach(&chip, fake_speaker);
	hrc_pcm_diag_t d;
	hrc_audio_pcm_diag(&d);
	CHECK(d.mode == (HRC_PCM_PREFILL | HRC_PCM_PACED) && d.loud_blocks == 0 && d.lat_min_us == UINT32_MAX);	/* m3 at power-on */

	/* Mode 0, the feeding up to beta.5: the FIFO starts on the first block with no margin, so the slower blocks after it
	 * leave it dry, and the driver's gap model sees what the FIFO model hears */
	fresh(0);
	call(50, 2000u, 7000u);
	hrc_audio_pcm_diag(&d);
	printf("mode 0: model gaps %u (%u ms dry), driver gaps %u longest %u us, latency %u-%u us, slow %u\n",
	       (unsigned)model_gaps, (unsigned)(model_gap_steps / 8u), (unsigned)d.gaps, (unsigned)d.gap_max_us,
	       (unsigned)d.lat_min_us, (unsigned)d.lat_max_us, (unsigned)d.slow);
	CHECK(model_gaps > 0);
	CHECK(d.gaps > 0 && d.gaps <= model_gaps && d.gap_max_us >= PD_GAP_US);
	CHECK(d.lat_min_us >= 2000u && d.lat_min_us < 3000u && d.lat_max_us >= 7000u && d.lat_max_us < 9000u);
	CHECK(d.slow == 0 && d.prefills == 0 && d.mode == 0 && d.loud_blocks == 49);
	CHECK(order_errors == 0 && d.full > 0);
	CHECK(d.dropped == 0 && play_diag[9] == 1);	/* the first block, stale once the route turned on: not a loss */
	CHECK(!pd_wait);

	/* Mode 1, the prefill: one block of margin before the FIFO starts; the same call plays without a gap */
	fresh(HRC_PCM_PREFILL);
	call(50, 2000u, 7000u);
	hrc_audio_pcm_diag(&d);
	printf("mode 1: model gaps %u, driver gaps %u, prefills %u\n", (unsigned)model_gaps, (unsigned)d.gaps,
	       (unsigned)d.prefills);
	CHECK(model_gaps == 0 && d.gaps == 0 && d.prefills == 1 && d.mode == HRC_PCM_PREFILL);
	CHECK(order_errors == 0 && d.dropped == 0);
	/* even a decode that takes 15 ms (the margin is a block): no gap, one slow block counted each time */
	fresh(HRC_PCM_PREFILL);
	call(30, 2000u, 15500u);
	hrc_audio_pcm_diag(&d);
	CHECK(model_gaps == 0 && d.gaps == 0 && d.slow > 0);

	/* Mode 3, prefill and the P25 pacing for every stream: no gap either, one chunk a tick */
	fresh(HRC_PCM_PREFILL | HRC_PCM_PACED);
	CHECK(play_paced);
	call(50, 2000u, 7000u);
	hrc_audio_pcm_diag(&d);
	printf("mode 3: model gaps %u, driver gaps %u\n", (unsigned)model_gaps, (unsigned)d.gaps);
	CHECK(model_gaps == 0 && d.gaps == 0 && order_errors == 0 && d.full == 0);

	/* A stream of one block (a short beep): the prefill gives up waiting and plays it */
	fresh(HRC_PCM_PREFILL);
	cb_us = 1000u;
	one_block();	/* the route turns on: this block is dropped (as before) */
	advance(19000u);
	one_block();	/* the one block that plays */
	call_live = false;
	uint32_t before = play_diag[2];
	advance(10000u);
	CHECK(play_diag[2] == before);	/* waiting for a second block */
	advance(PD_PREFILL_US);	/* 35 ms after it was queued: the FIFO started at 25 ms */
	CHECK(play_diag[2] > before && !pd_wait);
	advance(20000u);
	CHECK(play_diag[2] == before + AUDIO_BLOCK_SAMPLES / C7_PROMPT_CHUNK);

	/* The mode changes while a stream runs: paced at once, and off again */
	fresh(0);
	CHECK(!play_paced);
	hrc_audio_pcm_set_mode(HRC_PCM_PACED);
	CHECK(play_paced && hrc_audio_pcm_mode() == HRC_PCM_PACED);
	hrc_audio_pcm_set_mode(0);
	CHECK(!play_paced && hrc_audio_pcm_mode() == 0);
	hrc_audio_pcm_set_mode(0xFF);
	CHECK(hrc_audio_pcm_mode() == (HRC_PCM_PREFILL | HRC_PCM_PACED));

	/* The idle stream (a DMR channel between calls: blocks every 20 ms, the speaker route off) fills the queue and
	 * drops a block every 20 ms after that: not a loss. The same while the speaker plays and the FIFO takes nothing
	 * (held) is one */
	fresh(0);
	cb_count = 1;	/* no route change in the callback: the route stays off */
	for (unsigned k = 0; k < 10; k++) {
		cb_us = 1000u;
		one_block();
		advance(19000u);
	}
	hrc_audio_pcm_diag(&d);
	CHECK(play_diag[8] == 10u - (SPK_BLOCKS - 1u) && d.dropped == 0);
	speaker_pcm(true);
	fifo_held = true;
	for (unsigned k = 0; k < 6; k++) {
		one_block();
		advance(19000u);
	}
	fifo_held = false;
	hrc_audio_pcm_diag(&d);
	CHECK(d.dropped == 6u - (SPK_BLOCKS - 1u));

	/* Mode 0 never holds the FIFO back, after any drop; the reset zeroes every counter */
	fresh(0);
	call(10, 2000u, 2000u);
	CHECK(!pd_wait);
	hrc_audio_pcm_reset_stats();
	hrc_audio_pcm_diag(&d);
	CHECK(d.gaps == 0 && d.gap_max_us == 0 && d.loud_blocks == 0 && d.lat_max_us == 0 && d.lat_min_us == UINT32_MAX);
	CHECK(d.chunks == 0 && d.full == 0 && d.dropped == 0 && d.overruns == 0 && d.slow == 0 && d.prefills == 0);
	stop();

	printf("DMR receive audio PCM diagnostics: %d failures\n", failures);
	return failures != 0;
}
