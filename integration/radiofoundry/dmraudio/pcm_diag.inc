/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus DMR receive audio diagnostics (CONFIG_DMR_AUDIO_DIAG, docs/plus.md "DMR receive audio"). Included in
 * platform/hrc7000/audio_pcm.c after pcm_playback.inc (tools/plus_dmraudio.py), so it sees the driver's queue, the
 * prompt FIFO feed and the P25 pacing clock.
 *
 * What it measures, for Tools > Diagnostics (boards/dm32/diag_dm32.c) and the debug link's diag lines:
 *  - FIFO gaps: the prompt FIFO has no empty flag, so each pushed chunk is taken to play 4 ms after the later of its
 *    push and the previous chunk's end. A loud chunk pushed more than PD_GAP_US after that end found the FIFO empty:
 *    the speaker had nothing to play (a click or a crackle). Counted with the longest such gap.
 *  - Block latency: from the microphone interrupt that completes a 20 ms block to the speaker block being queued (the
 *    audio task's wake-up, the vocoder's decode, any task of the same priority in between), over loud blocks only.
 *    Its spread is the jitter the speaker queue must absorb.
 *
 * And two ways of feeding the FIFO, chosen at run time (RAM only). Power-on default since v1.0.0-beta.6: both
 * (mode 3), the owner's radio on 2026-11-21: m0 rough at any gain, m3 clean (docs/plus.md "Result"):
 *  - PD_PREFILL: after the queue is emptied (the speaker route turning on, a TX gate edge, a stream start) the FIFO
 *    starts only once PD_PREFILL_BLOCKS blocks are queued, or the first one is PD_PREFILL_US old. Without it the first
 *    block is pushed into an empty FIFO at once, so the next one has no margin at all: any block later than the first
 *    leaves the FIFO empty (FIFO gaps above) until enough late blocks have built a margin.
 *  - PD_PACED: the P25 path's pacing (one 32-sample chunk per 4 ms by the CPU clock, pcm_playback.inc) for every stream,
 *    DMR voice and beeps included, instead of pushing while AUDIO_CONTROL b31 says there is room. */
#if CONFIG_DMR_AUDIO_DIAG
#if !CONFIG_P25
#error "CONFIG_DMR_AUDIO_DIAG needs the pacing clock of pcm_playback.inc (CONFIG_P25)"
#endif

#define PD_PREFILL_BLOCKS	2u	/* one block of margin under the one playing */
#define PD_PREFILL_US		25000u	/* a stream that stops after one block still plays it */
#define PD_GAP_US		1000u	/* shorter gaps are within the model's error */
#define PD_CHUNK_US		(C7_PROMPT_CHUNK * 125u)	/* 32 samples at 8 kHz */
#define PD_SLOW_US		15000u	/* a block this late used most of its 20 ms */

static volatile uint8_t pd_mode = HRC_PCM_PREFILL | HRC_PCM_PACED;	/* the default: both (owner, 2026-11-21) */
static bool pd_wait;			/* prefill: the FIFO waits for the margin */
static bool pd_have_first;		/* a block was queued since the drop (pd_first_us) */
static uint32_t pd_first_us;
static bool pd_playing;			/* pd_end_us holds the FIFO's end of play */
static uint32_t pd_end_us;
static hrc_pcm_diag_t pd;

static void pd_clear(void)
{
	memset(&pd, 0, sizeof pd);
	pd.lat_min_us = UINT32_MAX;
}

/* Paced feeding for the stream that runs now (start, a mode change while running). Interrupts off */
static void pd_pace_apply(void)
{
	if ((pd_mode & HRC_PCM_PACED) != 0) {
		if (!play_paced) {
			memset(&play_clock, 0, sizeof play_clock);
			play_paced = true;
		}
	} else {
		play_paced = false;
	}
	playback_amp_update();
}

/* spk_drop: the queue is empty, the FIFO plays out what it holds. Interrupts off */
static void pd_drop(void)
{
	pd_wait = (pd_mode & HRC_PCM_PREFILL) != 0;
	pd_have_first = false;
	pd_playing = false;
}

/* The audio task queued a block. Interrupts off */
static void pd_queued(bool loud)
{
	uint32_t now = hrc_micros();
	if (!pd_have_first) {
		pd_have_first = true;
		pd_first_us = now;
	}
	if (!loud) {
		return;
	}
	uint32_t lat = now - pd_ready_us;
	if (lat < pd.lat_min_us) {
		pd.lat_min_us = lat;
	}
	if (lat > pd.lat_max_us) {
		pd.lat_max_us = lat;
	}
	if (lat >= PD_SLOW_US) {
		pd.slow++;
	}
	pd.loud_blocks++;
}

/* fifo_feed, the route open: false while the prefill waits. Timer context */
static bool pd_feed_ok(void)
{
	if (!pd_wait) {
		return true;
	}
	unsigned queued = (unsigned)((spk_head + SPK_BLOCKS - spk_tail) % SPK_BLOCKS);
	if (queued >= PD_PREFILL_BLOCKS || (queued != 0 && pd_have_first && hrc_micros() - pd_first_us >= PD_PREFILL_US)) {
		pd_wait = false;
		pd.prefills++;
		return true;
	}
	return false;
}

/* A chunk went to the FIFO. Timer context */
static void pd_pushed(bool loud)
{
	uint32_t now = hrc_micros();
	int32_t late = (int32_t)(now - pd_end_us);
	if (loud && pd_playing && late > (int32_t)PD_GAP_US) {
		pd.gaps++;
		if ((uint32_t)late > pd.gap_max_us) {
			pd.gap_max_us = (uint32_t)late;
		}
	}
	if (!pd_playing || late > 0) {
		pd_end_us = now;
	}
	pd_end_us += PD_CHUNK_US;
	pd_playing = true;
}

void hrc_audio_pcm_set_mode(uint8_t mode)
{
	uint32_t s = hrc_irq_save();
	pd_mode = (uint8_t)(mode & (HRC_PCM_PREFILL | HRC_PCM_PACED));
	if (running) {
		pd_pace_apply();
	}
	hrc_irq_restore(s);
}

uint8_t hrc_audio_pcm_mode(void)
{
	return pd_mode;
}

void hrc_audio_pcm_diag(hrc_pcm_diag_t *out)
{
	uint32_t s = hrc_irq_save();
	*out = pd;
	out->mode = pd_mode;
	out->chunks = play_diag[2];
	out->full = play_diag[3];
	out->overruns = overruns;
	hrc_irq_restore(s);
}
#endif
