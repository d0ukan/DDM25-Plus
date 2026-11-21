// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Copyright (c) 2026 Open25-DM32 Authors
// Next's P25 microphone processing, with its input reference made explicit.
#ifndef P25_MIC_AGC_H
#define P25_MIC_AGC_H
#include <stdint.h>

typedef struct {
    int32_t low_q8;
    uint16_t fade;
    int16_t last;
} classic_p25_condition;

static inline void classic_p25_condition_init(classic_p25_condition *s)
{
    *s=(classic_p25_condition){INT32_MIN,224,0};
}

static inline void classic_p25_condition_block(classic_p25_condition *s,int16_t pcm[160],int keyed)
{
    /* Captured startup settles in 20 ms. RF switching disturbs the containing
     * block and the next 10 ms. Fade in over 8 ms; bridge RF fade-out over 4 ms
     * using the preceding output, so the disturbance cannot enter the bridge. */
    if(keyed)s->fade=304;
    if(s->low_q8==INT32_MIN)s->low_q8=pcm[159]*256;
    for(unsigned i=0;i<160;i++) {
        s->low_q8+=(pcm[i]*256-s->low_q8)>>(s->fade>64?3:6);
        int32_t sample=pcm[i]-(s->low_q8>>8);
        if(sample>INT16_MAX)sample=INT16_MAX;
        if(sample<INT16_MIN)sample=INT16_MIN;
        if(s->fade) {
            if(keyed && i<32)sample=(s->last*(31-(int)i))>>5;
            else if(s->fade>64)sample=0;
            else sample=(sample*(64-s->fade))>>6;
            s->fade--;
        }
        pcm[i]=(int16_t)sample;
    }
    s->last=pcm[159];
}

typedef struct {
    int32_t low_q8;
    uint32_t noise_q8;
    int32_t gain_q12;
    int32_t speech_level;
} classic_p25_filter;


enum {
    BLOCK_SAMPLES = 160,
    DEFAULT_NOISE_Q8 = 96 * 256,
    MIN_NOISE_Q8 = 48 * 256,
    MAX_NOISE_Q8 = 1600 * 256,
    GAIN_QUIET = 1024, GAIN_UNITY = 4096, GAIN_MAX = 3 * GAIN_UNITY,
    SPEECH_TARGET = 2300,
    MAX_OUTPUT = 28000,
};

static inline unsigned classic_p25_magnitude(int32_t sample)
{
    return (unsigned)(sample < 0 ? -sample : sample);
}

/* A one-pole high-pass at about 170 Hz rejects wind, handling rumble and hum. */
static inline int32_t classic_p25_high_pass(int32_t *low_q8, int16_t sample)
{
    *low_q8 += (sample * 256 - *low_q8) >> 3;
    return sample - (*low_q8 >> 8);
}

static inline uint32_t classic_p25_noise_step(uint32_t previous, unsigned level)
{
    uint32_t target = level * 256;
    if (target < MIN_NOISE_Q8) target = MIN_NOISE_Q8;
    if (target > MAX_NOISE_Q8) target = MAX_NOISE_Q8;
    int32_t difference = (int32_t)target - (int32_t)previous;
    /* Follow a quieter room quickly and avoid chasing passing sounds. */
    return (uint32_t)((int32_t)previous + (difference >> (difference < 0 ? 2 : 4)));
}

static inline void classic_p25_filter_init(classic_p25_filter *filter, uint32_t background)
{
    filter->low_q8 = INT32_MIN;
    filter->noise_q8 = background ? background : DEFAULT_NOISE_Q8;
    filter->gain_q12 = GAIN_UNITY;
    filter->speech_level = 0;
}

static inline void classic_p25_filter_block(classic_p25_filter *filter, int16_t pcm[160])
{
    if (filter->low_q8 == INT32_MIN) filter->low_q8 = pcm[0] * 256;
    unsigned total = 0, peak = 0;
    for (unsigned i = 0; i < BLOCK_SAMPLES; i++) {
        int32_t sample = classic_p25_high_pass(&filter->low_q8, pcm[i]);
        if (sample > INT16_MAX) sample = INT16_MAX;
        if (sample < INT16_MIN) sample = INT16_MIN;
        pcm[i] = (int16_t)sample;
        unsigned level = classic_p25_magnitude(sample);
        total += level;
        if (level > peak) peak = level;
    }

    unsigned level = total / BLOCK_SAMPLES;
    unsigned noise = filter->noise_q8 >> 8;
    unsigned quiet = noise + noise / 2, speech = 3 * noise;
    int32_t target;
    if (level <= quiet) {
        target = GAIN_QUIET;
        filter->noise_q8 = classic_p25_noise_step(filter->noise_q8, level);
    } else {
        /* Track clear speech only. Hold that estimate through pauses so the
         * compressor cannot turn up room noise as the speaker trails off. */
        if (level >= speech) {
            int32_t change = (int32_t)level - filter->speech_level;
            if (!filter->speech_level) filter->speech_level = (int32_t)level;
            else filter->speech_level += change / (change > 0 ? 2 : 8);
        }
        int32_t speech_level = filter->speech_level ? filter->speech_level : (int32_t)speech;
        /* Blend unity gain with level normalization to retain some natural
         * dynamics, with a ceiling on gain for quiet or off-axis speech. */
        target = (GAIN_UNITY + SPEECH_TARGET * GAIN_UNITY / speech_level) / 2;
        if (target > GAIN_MAX) target = GAIN_MAX;
        if (level < speech) {
            target = GAIN_QUIET + (target - GAIN_QUIET) *
                        (int32_t)(level - quiet) / (int32_t)(speech - quiet);
        }
    }

    /* Bound the gain by the entire block's peak. Ease upward into speech
     * and downward into pauses so changes in gain don't add clicks. */
    int32_t ceiling = GAIN_MAX;
    if (peak > MAX_OUTPUT / 3) ceiling = MAX_OUTPUT * GAIN_UNITY / (int32_t)peak;
    if (target > ceiling) target = ceiling;
    int32_t gain = filter->gain_q12;
    if (gain > ceiling) gain = ceiling;
    for (unsigned i = 0; i < BLOCK_SAMPLES; i++) {
        if (gain < target) gain += (target - gain + 15) >> 4;
        else if (gain > target) gain -= (gain - target + 127) >> 7;
        pcm[i] = (int16_t)((pcm[i] * gain) >> 12);
    }
    filter->gain_q12 = gain;
}

typedef struct {
 classic_p25_condition condition;
 classic_p25_filter filter;
} p25_mic_agc_t;
static inline void p25_mic_agc_init(p25_mic_agc_t *s)
{
 classic_p25_condition_init(&s->condition);
 classic_p25_filter_init(&s->filter,0);
}
static inline void p25_mic_agc_block(p25_mic_agc_t *s,int16_t pcm[160])
{
 /* Next captures with 10 dB more ADC gain. Put the native PCM in the same
  * units before using its speech/noise thresholds and level target. This
  * calibration belongs to the optional automatic path; fixed gain is unchanged. */
 for(unsigned i=0;i<160;i++){
  int32_t v=(int32_t)pcm[i]*810/256;
  if(v>INT16_MAX)v=INT16_MAX;
  if(v<INT16_MIN)v=INT16_MIN;
  pcm[i]=(int16_t)v;
 }
 classic_p25_condition_block(&s->condition,pcm,0);
 classic_p25_filter_block(&s->filter,pcm);
}
#endif
