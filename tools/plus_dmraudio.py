#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""DDM32 Plus DMR receive audio diagnostics (CONFIG_DMR_AUDIO_DIAG, docs/plus.md "DMR receive audio: diagnostics").
Called last from plus_features.prepare(); a module of its own so the shared prepare list gains one line. Our code is
integration/radiofoundry/dmraudio/pcm_diag.inc (installed next to platform/hrc7000/audio_pcm.c); upstream files
change only through the hunks below.

The owner hears DMR receive audio crackly and artificial on v1.0.0-beta.3 while our transmit is clean. Nothing that
changed between beta.2 and beta.3 runs in the receive audio path during a call, so the cause has to be found on the
radio. This gives one session the evidence, with no cable and no second image:

  Tools > Diagnostics, first page, five lines (debug text, not translated, like the page's other lines):
    dv m<mode> in<AMBE frames in> pl<played>                         the AMBE voice queue (ambeVoice.c counters)
    dv cc<concealed> un<underruns> q<queue max> pk<peak sample>
    dv bd<frames FEC could not fix> ls<lost> be<bit errors fixed>
    sp gp<FIFO ran dry>/<longest, ms> fu<FIFO full refusals> dr<blocks dropped>   the prompt FIFO feed (pcm_diag.inc)
    au bl<block latency min>-<max, ms> sl<blocks 15 ms late> ov<mic blocks lost>  the audio task's timing
  On that page, with a channel shown: 0 steps the FIFO feed mode (m0 as before, m1 prefill, m2 paced, m3 both),
  9 zeroes the counters. RAM only: every power-on starts at m0, the shipped behavior.
"""

DMRAUDIO = 'integration/radiofoundry/dmraudio/'
ON = '#if CONFIG_DMR_AUDIO_DIAG\n'
ONB = '#if CONFIG_DMR_AUDIO_DIAG && CONFIG_DMR && CONFIG_DIAG\n'	# the page's pieces: the page and the driver both built

# The lines dm32_diag_line shows first (diag_dm32.c); the formatter is a function of its own so the host test can
# compile it alone (tools/test_dmraudio.py)
DIAG_LINES = r'''#if CONFIG_DMR_AUDIO_DIAG && CONFIG_DMR && CONFIG_DIAG
// DDM32 Plus DMR receive audio (tools/plus_dmraudio.py): the voice queue, the prompt FIFO feed, the audio task's timing.
// At most 26 characters a line (the page's 8-pixel font inside the 240-pixel panel's insets): counters stop at a cap
#define DMRAUDIO_LINES	5u

static void put_c(out_t *o, uint32_t v, uint32_t cap)
{
	put_u(o, v < cap ? v : cap);
}

static void put_ms10(out_t *o, uint32_t us)
{
	uint32_t t = us < 99900u ? (us + 50u) / 100u : 999u;	// tenths of a millisecond, at most 99.9
	put_u(o, t / 10u);
	put(o, ".");
	put_u(o, t % 10u);
}

static bool dmraudio_line(uint8_t i, char *buf, uint8_t len)
{
	out_t o = { buf, buf + len - 1 };
	ambe_voice_stats_t a;
	hrc_pcm_diag_t p;
	ambeVoiceGetStats(&a);
	hrc_audio_pcm_diag(&p);
	switch (i) {
	case 0:		// the FIFO feed mode; AMBE frames from the air, frames played
		put(&o, "dv m");
		put_c(&o, p.mode, 9u);
		put(&o, " in");
		put_c(&o, a.rx_in, 99999u);
		put(&o, " pl");
		put_c(&o, a.rx_frames, 99999u);
		break;
	case 1:		// blocks concealed (a frame repeated), queue underruns, the queue's most, the call's peak sample
		put(&o, "dv cc");
		put_c(&o, a.rx_concealed, 999u);
		put(&o, " un");
		put_c(&o, a.rx_underruns, 999u);
		put(&o, " q");
		put_c(&o, a.rx_queue_max, 99u);
		put(&o, " pk");
		put_c(&o, a.rx_peak, 99999u);
		break;
	case 2:		// frames FEC could not fix, frames lost, bit errors fixed
		put(&o, "dv bd");
		put_c(&o, a.rx_bad, 9999u);
		put(&o, " ls");
		put_c(&o, a.rx_lost, 9999u);
		put(&o, " be");
		put_c(&o, a.rx_bit_errors, 99999u);
		break;
	case 3:		// the FIFO ran dry / the longest (ms), FIFO full refusals, blocks dropped
		put(&o, "sp gp");
		put_c(&o, p.gaps, 999u);
		put(&o, "/");
		put_ms10(&o, p.gap_max_us);
		put(&o, " fu");
		put_c(&o, p.full, 9999u);
		put(&o, " dr");
		put_c(&o, p.dropped, 999u);
		break;
	case 4:		// block latency min-max (ms), blocks 15 ms late or more, microphone blocks lost
		put(&o, "au bl");
		if (p.loud_blocks == 0) {
			put(&o, "-");
		} else {
			put_ms10(&o, p.lat_min_us);
			put(&o, "-");
			put_ms10(&o, p.lat_max_us);
		}
		put(&o, " sl");
		put_c(&o, p.slow, 999u);
		put(&o, " ov");
		put_c(&o, p.overruns, 999u);
		break;
	default:
		return false;
	}
	*o.p = '\0';
	return true;
}

// On the diagnostics page with a channel shown (core/ui/ui_app.c ui_diag_key): 0 steps the FIFO feed mode, 9 zeroes
// the counters behind the page
bool dm32_diag_digit(uint8_t d)
{
	if (d == 0u) {
		hrc_audio_pcm_set_mode((uint8_t)((hrc_audio_pcm_mode() + 1u) & 3u));
		return true;
	}
	if (d == 9u) {
		dm32_diag_reset();
		ambeVoiceResetStats();
		return true;
	}
	return false;
}
#endif

'''

PCM_HEADER = '''#if CONFIG_DMR_AUDIO_DIAG
// DDM32 Plus DMR receive audio diagnostics (platform/hrc7000/pcm_diag.inc, tools/plus_dmraudio.py)
#define HRC_PCM_PREFILL	0x01u	// after the queue empties, the FIFO waits for one block of margin
#define HRC_PCM_PACED	0x02u	// one chunk per 4 ms by the CPU clock (the P25 path's pacing) for every stream
typedef struct {
	uint32_t mode;		// HRC_PCM_* now
	uint32_t gaps;		// loud chunks that found the FIFO empty (by the 8 kHz play model), the longest gap
	uint32_t gap_max_us;
	uint32_t lat_min_us;	// microphone block complete -> speaker block queued, loud blocks only
	uint32_t lat_max_us;
	uint32_t slow;		// loud blocks queued 15 ms or more after their microphone block
	uint32_t loud_blocks;
	uint32_t prefills;	// prefills that ended (the FIFO started)
	uint32_t chunks;	// chunks pushed, FIFO full refusals, blocks dropped (queue full, stale after a gate edge)
	uint32_t full;
	uint32_t dropped;
	uint32_t overruns;	// microphone blocks the audio task missed (a 20 ms gap)
} hrc_pcm_diag_t;
void hrc_audio_pcm_diag(hrc_pcm_diag_t *out);
// A mode (HRC_PCM_* bits; RAM only, 0 at power-on), applied to the stream running now and every later one
void hrc_audio_pcm_set_mode(uint8_t mode);
uint8_t hrc_audio_pcm_mode(void);
#endif
'''


def prepare_dmraudio(transform, install):
    install('platform/hrc7000/pcm_diag.inc', DMRAUDIO + 'pcm_diag.inc')

    transform('config/features.mk', [
        ('CONFIG_DATAPACK ?= n\nFEATURES += DATAPACK\n',
         'CONFIG_DATAPACK ?= n\nFEATURES += DATAPACK\n'
         'CONFIG_DMR_AUDIO_DIAG ?= n\nFEATURES += DMR_AUDIO_DIAG\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_DATAPACK\n',
         '#ifndef CONFIG_DMR_AUDIO_DIAG\n#define CONFIG_DMR_AUDIO_DIAG 0\t/* DDM32 Plus DMR receive audio diagnostics */\n'
         '#endif\n#ifndef CONFIG_DATAPACK\n'),
    ])

    # The driver: the counters, the prefill gate, the paced mode for every stream
    transform('platform/hrc7000/audio_pcm.h', [
        ('void hrc_audio_pcm_reset_stats(void);\n', 'void hrc_audio_pcm_reset_stats(void);\n' + PCM_HEADER),
    ])
    transform('platform/hrc7000/audio_pcm.c', [
        ('static volatile int16_t *const pcm_mic = (volatile int16_t *)C7_PCM_MIC_ADDR;\n',
         'static volatile int16_t *const pcm_mic = (volatile int16_t *)C7_PCM_MIC_ADDR;\n'
         + ON + 'static volatile uint32_t pd_ready_us;\t// DDM32 Plus: the last microphone block completed (pcm_diag.inc)\n'
         '#endif\n'),
        ('\tmic_ready = (int8_t)mic_fill;\n',
         '\tmic_ready = (int8_t)mic_fill;\n' + ON + '\tpd_ready_us = hrc_micros();\n#endif\n'),
        ('#include "pcm_playback.inc"\n',
         '#include "pcm_playback.inc"\n#include "pcm_diag.inc"\t// DDM32 Plus DMR receive audio diagnostics\n'),
        ('\tif (!spk_routed || fifo_held || tx_gated) {\n\t\treturn;\n\t}\n\twhile (spk_tail',
         '\tif (!spk_routed || fifo_held || tx_gated) {\n\t\treturn;\n\t}\n'
         + ON + '\tif (!pd_feed_ok()) {\n\t\treturn;\t// DDM32 Plus: the prefill waits for its margin\n\t}\n#endif\n'
         '\twhile (spk_tail'),
        ('\t\tif (spk_loud[spk_tail]) {\n\t\t\tloud_pushed = hrc_micros();',
         ON + '\t\tpd_pushed(spk_loud[spk_tail]);\n#endif\n'
         '\t\tif (spk_loud[spk_tail]) {\n\t\t\tloud_pushed = hrc_micros();'),
        ('\t\t\tspk_loud[spk_head] = loud;\n\t\t\tspk_head = next;\n',
         '\t\t\tspk_loud[spk_head] = loud;\n\t\t\tspk_head = next;\n' + ON + '\t\t\tpd_queued(loud);\n#endif\n'),
        ('\tspeaker_pcm_cb = speaker_pcm;\n',
         '\tspeaker_pcm_cb = speaker_pcm;\n' + ON + '\tpd_clear();\n#endif\n'),
        ('static void spk_drop(void)\n{\n\tspk_head = spk_tail = 0;\n',
         'static void spk_drop(void)\n{\n\tspk_head = spk_tail = 0;\n' + ON + '\tpd_drop();\n#endif\n'),
        ('\thrc_barrier();\n\trunning = true;\n',
         '\thrc_barrier();\n\trunning = true;\n'
         + ON + '\tif ((pd_mode & HRC_PCM_PACED) != 0) {\n\t\tpd_pace_apply();\t// DDM32 Plus: paced for every stream\n'
         '\t}\n#endif\n'),
        ('\tblocks = overruns = underruns = 0;\n',
         '\tblocks = overruns = underruns = 0;\n'
         + ON + '\tuint32_t s = hrc_irq_save();\n\tpd_clear();\n\tmemset(play_diag, 0, sizeof play_diag);\n'
         '\tplay_diag[5] = UINT32_MAX;\t// the shortest push gap: none yet\n\thrc_irq_restore(s);\n#endif\n'),
    ])

    # The page: five lines first, the digits 0 and 9
    transform('hal/board.h', [
        ('\tvoid (*diag_reset)(void);\n',
         '\tvoid (*diag_reset)(void);\n'
         '\t// DDM32 Plus (CONFIG_DMR_AUDIO_DIAG): a digit pressed on the diagnostics page with a channel shown; true =\n'
         '\t// taken. May be NULL\n'
         '\tbool (*diag_digit)(uint8_t digit);\n'),
    ])
    transform('boards/dm32/dm32.h', [
        ('void dm32_diag_reset(void);\n',
         'void dm32_diag_reset(void);\n' + ONB + 'bool dm32_diag_digit(uint8_t d);\t// DDM32 Plus (diag_dm32.c)\n#endif\n'),
    ])
    transform('boards/dm32/board.c', [
        ('\t.diag_reset = dm32_diag_reset,\n',
         '\t.diag_reset = dm32_diag_reset,\n' + ONB + '\t.diag_digit = dm32_diag_digit,\n#endif\n'),
    ])
    transform('boards/dm32/diag_dm32.c', [
        ('#include "platform.h"\n',
         '#include "platform.h"\n' + ONB + '#include "audio_pcm.h"\n#include "dmr/ambe/ambeVoice.h"\n#endif\n'),
        ('bool dm32_diag_line(uint8_t i, char *buf, uint8_t len)\n{\n\tif (len == 0) {\n\t\treturn false;\n\t}\n',
         DIAG_LINES +
         'bool dm32_diag_line(uint8_t i, char *buf, uint8_t len)\n{\n\tif (len == 0) {\n\t\treturn false;\n\t}\n'
         + ONB + '\tif (i < DMRAUDIO_LINES) {\n\t\treturn dmraudio_line(i, buf, len);\t// DDM32 Plus: first, on page one\n'
         '\t}\n\ti -= DMRAUDIO_LINES;\n#endif\n'),
    ])
    transform('core/ui/ui_app.c', [
        ('\tif (ev->action >= ACT_DIGIT_0 && ev->action <= ACT_DIGIT_9 && !view->vfo)\n\t\treturn false;\t// no Find from here\n',
         ONB +
         '\t// DDM32 Plus DMR receive audio (tools/plus_dmraudio.py): the board takes 0 (the FIFO feed mode) and 9 (zero\n'
         '\t// the counters); the page shows the change at its next draw\n'
         '\tif (ev->gesture == GESTURE_PRESS && ev->action >= ACT_DIGIT_0 && ev->action <= ACT_DIGIT_9 && !view->vfo &&\n'
         '\t    board.diag_digit != NULL && board.diag_digit((uint8_t)(ev->action - ACT_DIGIT_0)))\n\t\treturn true;\n'
         '#endif\n'
         '\tif (ev->action >= ACT_DIGIT_0 && ev->action <= ACT_DIGIT_9 && !view->vfo)\n\t\treturn false;\t// no Find from here\n'),
    ])
