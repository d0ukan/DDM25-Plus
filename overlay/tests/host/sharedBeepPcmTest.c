// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Copyright (c) 2026 Open25-DM32 Authors
/* Real beep/AMBE callbacks and PCM producer/queue; only hardware and vocoder are doubled. */
#define PLATFORM_HRC7000_H
#define HRC_DMR_GLUE_H
#define pdFALSE 0
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY 0xffffffffu
#define pdMS_TO_TICKS(x) (x)
#define TIMER_PCM 6
typedef void *TaskHandle_t;
typedef int BaseType_t;
typedef unsigned TickType_t;
#define portYIELD_FROM_ISR(x) ((void)(x))
#define IRQ_PCM_WR 28
#define IRQ_TIMER6 6
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "modem/hrc7000/hrc7000.h"
#include "dmr/ambe/ambeVoice.h"
#include "dmr/ambe/ambeFec.h"
#include "radio/beep.h"

#define CAPTURE_SAMPLES 16000u
static uint32_t now_us, fifo_ready_us, timer_at;
static bool timer_armed;
static int16_t captured[CAPTURE_SAMPLES];
static unsigned captured_count, decoded_frames, failures, checks;
static unsigned tx_encoded, tx_encoded_routed, tx_input_errors;
static bool check_tx_input, board_route;
static bool one_task_block, tx_active;
static unsigned task_waits;
static jmp_buf task_done;
static uint32_t regs[1024];
static uint8_t buffers[4096];
static hrc7000_t chip = { .regs = regs, .buf = buffers };

#define CHECK(test, ...) do { checks++; if (!(test)) { failures++; \
    printf("FAIL shared beep: "); printf(__VA_ARGS__); putchar('\n'); } } while (0)

static void hrc_timer_oneshot(uint8_t n, uint32_t us)
{ assert(n == TIMER_PCM && us > 0); timer_at = now_us + us; timer_armed = true; }
static void hrc_timer_stop(uint8_t n) { assert(n == TIMER_PCM); timer_armed = false; }
static uint32_t hrc_micros(void) { return now_us; }
void hal_delay_us(uint32_t us) { assert(us == 200); now_us += us; }
static uint32_t hrc_irq_save(void) { return 0; }
static void hrc_irq_restore(uint32_t s) { (void)s; }
static void hrc_irq_enable(unsigned irq, bool on) { (void)irq; (void)on; }
static void hrc_irq_register(uint8_t irq, void (*fn)(void)) { (void)irq; (void)fn; }
static void hrc_barrier(void) { }
static bool hrc_dmr_tx_active(void) { return tx_active; }

void hrc7000_set_crit(hrc7000_t *m, uint32_t (*enter)(void), void (*leave)(uint32_t))
{ m->crit_enter = enter; m->crit_exit = leave; }
void hrc7000_audio_bits(hrc7000_t *m, uint32_t set, uint32_t clear)
{ m->audio_ctl = (m->audio_ctl & ~clear) | set; }
void vTaskNotifyGiveFromISR(TaskHandle_t t, BaseType_t *w) { (void)t; (void)w; }
BaseType_t xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *ctx,
                      unsigned priority, TaskHandle_t *handle)
{ (void)fn; (void)name; (void)stack; (void)ctx; (void)priority; *handle = (void *)1; return pdPASS; }
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait)
{
    (void)clear; (void)wait;
    if (one_task_block && task_waits++) longjmp(task_done, 1);
    return 1;
}
void xTaskNotifyGive(TaskHandle_t t) { (void)t; }

/* Observe accepted writes after the actual byte writer, and model one 4 ms FIFO
 * chunk at a time. The production queue/generation checks decide what reaches it. */
static bool capture_push(hrc7000_t *m, const int16_t *pcm)
{
    if (!hrc7000_prompt_push(m, pcm)) return false;
    assert(captured_count + C7_PROMPT_CHUNK <= CAPTURE_SAMPLES);
    for (unsigned i = 0; i < C7_PROMPT_CHUNK; i++) {
        uint16_t raw = (uint16_t)m->buf[C7_PROMPT_FIFO + 2u * i] |
                       (uint16_t)((uint16_t)m->buf[C7_PROMPT_FIFO + 2u * i + 1u] << 8);
        assert((int16_t)raw == pcm[i]);
        captured[captured_count++] = (int16_t)raw;
    }
    regs[C7_AUDIO_CONTROL / 4] |= C7_AUDIO_RING_FULL;
    /* The hardware half switch keeps its 4 ms cadence during the 200 us settled write. */
    fifo_ready_us = now_us + 4000u - 200u;
    return true;
}
#define hrc7000_prompt_push capture_push
#ifndef AUDIO_PCM_SOURCE
#define AUDIO_PCM_SOURCE "../../platform/hrc7000/audio_pcm.c"
#endif
#include AUDIO_PCM_SOURCE
#undef hrc7000_prompt_push

static void route(bool on) { board_route = on; hrc_audio_pcm_ring(&chip, on); }
static bool vocoder_present(void) { return true; }
static void vocoder_reset(void) { }
static void vocoder_encode(const int16_t *pcm, uint8_t *bits)
{
    if (check_tx_input) {
        tx_encoded++;
        tx_encoded_routed += board_route;
        for (unsigned i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
            tx_input_errors += pcm[i] != (i & 1u ? 300 : -300);
    }
    memset(bits, 0, VOCODER_BITS);
}
/* A silent decoded call isolates the overlaid tone while exercising real RX
 * priming, frame consumption, end/drain and shared-stream ownership. */
static void vocoder_decode(const uint8_t *bits, int16_t *pcm)
{ (void)bits; memset(pcm, 0, AUDIO_BLOCK_SAMPLES * sizeof *pcm); decoded_frames++; }
static const hal_vocoder_t vocoder = {
    .present = vocoder_present, .reset = vocoder_reset,
    .encode = vocoder_encode, .decode = vocoder_decode,
};

static void step(bool shared)
{
    if (running) {
        mic_ready = 0;
        task_waits = 0;
        one_task_block = true;
        if (!setjmp(task_done)) audio_task(NULL);
        one_task_block = false;
    }
    uint32_t until = now_us + 20000u;
    while (timer_armed && timer_at < until) {
        assert(timer_at >= now_us);
        now_us = timer_at;
        if (now_us >= fifo_ready_us) regs[C7_AUDIO_CONTROL / 4] &= ~C7_AUDIO_RING_FULL;
        fifo_tick();
    }
    now_us = until;
    beep_service(shared, now_us / 1000u);
}

static void settle(bool shared)
{ for (unsigned i = 0; i < 20; i++) step(shared); }

static unsigned trimmed(const int16_t **start)
{
    unsigned first = 0, last = captured_count;
    while (first < last && captured[first] == 0) first++;
    while (last > first && captured[last - 1] == 0) last--;
    *start = captured + first;
    return last - first;
}

typedef struct { int16_t pcm[1600]; unsigned count; } reference_t;

static void make_reference(beep_t pattern, reference_t *ref)
{
    captured_count = 0;
    beep_play(pattern, 50, false);
    settle(false);
    const int16_t *pcm;
    ref->count = trimmed(&pcm);
    assert(ref->count <= sizeof ref->pcm / sizeof *ref->pcm);
    memcpy(ref->pcm, pcm, ref->count * sizeof *pcm);
    CHECK(!running && !spk_routed, "own stream releases after reference");
}

#if CONFIG_SIG_TONES
static void custom_beep(bool shared)
{
    beep_seg_t steps[] = { { 730, 35 }, { 0, 25 }, { 1770, 50 } };
    beep_play_custom(steps, 3, 50, shared);
    memset(steps, 0, sizeof steps); /* The request owns a copy, not the caller's array. */
}

static void make_custom_reference(reference_t *ref)
{
    captured_count = 0;
    custom_beep(false);
    settle(false);
    const int16_t *pcm;
    ref->count = trimmed(&pcm);
    assert(ref->count <= sizeof ref->pcm / sizeof *ref->pcm);
    memcpy(ref->pcm, pcm, ref->count * sizeof *pcm);
    CHECK(!running && !spk_routed, "own custom tone releases after reference");
}
#endif

static void check_reference(const char *label, const reference_t *ref)
{
    const int16_t *pcm;
    unsigned count = trimmed(&pcm);
    CHECK(count == ref->count && !memcmp(pcm, ref->pcm,
          (count < ref->count ? count : ref->count) * sizeof *pcm),
          "%s delivered %u samples; own stream delivered %u", label, count, ref->count);
    CHECK(ambeVoiceStreaming() && running, "%s retains the DMR stream", label);
    CHECK(!spk_routed, "%s releases the speaker route after the tail", label);
    printf("Shared beep %s: delivered=%u reference=%u\n", label, count, ref->count);
}

static void check_own_reference(const char *label, const reference_t *ref)
{
    const int16_t *pcm;
    unsigned count = trimmed(&pcm);
    CHECK(count == ref->count && !memcmp(pcm, ref->pcm,
          (count < ref->count ? count : ref->count) * sizeof *pcm),
          "%s delivered %u samples; own stream delivered %u", label, count, ref->count);
    CHECK(!running && !spk_routed, "%s releases the own stream after the tail", label);
    printf("Own beep %s: delivered=%u reference=%u\n", label, count, ref->count);
}

static void transmit_then_beep(const reference_t *ref)
{
    const dmr_voice_source_t *source = ambeVoiceSource();
    uint8_t frame[DMR_AMBE_FRAME_BYTES];
    ambe_voice_stats_t before, after;
    ambeVoiceGetStats(&before);
    /* Exercise the real source and PCM TX gate lifecycle without an RF engine.
     * Every encoded frame is consumed before end(), as on an active call. */
    tx_active = true;
    hrc_audio_pcm_tx_gate();
    source->begin(source->ctx);
    for (unsigned i = 0; i < 4; i++) {
        step(true);
        CHECK(source->next(frame, source->ctx), "TX source supplies an encoded frame");
    }
    source->end(source->ctx);
    tx_active = false;
    settle(true);
    CHECK(!source->next(frame, source->ctx), "ended TX source has no pending frames");
    ambeVoiceGetStats(&after);
    CHECK(after.tx_frames == before.tx_frames + 4, "TX source encoded every PCM block");
    CHECK(ambeVoiceStreaming() && !spk_routed && !tx_gated,
          "TX ends with an idle shared stream and an open speaker gate");
    captured_count = 0;
    beep_play(BEEP_KEY, 50, true);
    settle(true);
    check_reference("after DMR TX", ref);
}

static void begin_call(void)
{
    const dmr_voice_sink_t *sink = ambeVoiceSink();
    uint8_t bits[VOCODER_BITS] = {0}, frame[DMR_AMBE_FRAME_BYTES];
    ambeFecEncode(bits, frame);
    sink->begin(NULL, sink->ctx);
    for (unsigned i = 0; i < 3; i++) sink->frame(frame, sink->ctx);
    sink->end(sink->ctx);
}

static void cold_permit_transmit(void)
{
    const dmr_voice_source_t *source = ambeVoiceSource();
    uint8_t frame[DMR_AMBE_FRAME_BYTES];
    beep_play(BEEP_TX_PERMIT_SHORT, 50, false);
    uint32_t permit_at = now_us;
    while ((beep_busy() || hrc_audio_pcm_spk_pending()) && now_us - permit_at < 400000u) step(false);
    CHECK(!beep_busy() && !hrc_audio_pcm_spk_pending(), "cold permit drains before TX admission");
    CHECK(!ambeVoiceStreaming() && running && spk_routed && board_route,
          "cold permit hands its remaining silent route hold to the first voice owner");
    tx_active = true;
    hrc_audio_pcm_tx_gate();
    source->begin(source->ctx);
    check_tx_input = true;
    for (unsigned block = 0; block < 12; block++) {
        for (unsigned i = 0; i < AUDIO_BLOCK_SAMPLES; i++) mic_blk[0][i] = i & 1u ? 300 : -300;
        step(true);
        CHECK(source->next(frame, source->ctx), "cold TX supplies frame %u", block);
        if (block >= 4) CHECK(!spk_routed && !board_route,
            "cold TX block %u releases both actual PCM and board speaker routes after the permit tail", block);
    }
    check_tx_input = false;
    CHECK(tx_encoded == 12 && tx_input_errors == 0, "cold TX forwards every microphone block unchanged");
    CHECK(tx_encoded_routed <= 4,
          "cold TX encodes after the silent permit route hold releases (routed %u of 12 blocks)", tx_encoded_routed);
    source->end(source->ctx);
    tx_active = false;
    settle(true);
    ambeVoiceStop();
}

static void receive_to_transmit(void)
{
    const dmr_voice_source_t *source = ambeVoiceSource();
    uint8_t frame[DMR_AMBE_FRAME_BYTES];
    begin_call();
    step(true);
    step(true);
    CHECK(!beep_busy() && spk_routed && board_route, "queued receive voice owns the route before PTT");
    tx_active = true;
    hrc_audio_pcm_tx_gate();
    source->begin(source->ctx);
    step(true);
    CHECK(source->next(frame, source->ctx), "RX to TX handoff encodes its first microphone frame");
    CHECK(!spk_routed && !board_route,
          "TX releases both speaker routes while the earlier RX queue drains silently");
    source->end(source->ctx);
    tx_active = false;
    settle(true);
    ambeVoiceStop();
}

int main(void)
{
    reference_t key, error, permit;
    hrc_audio_pcm_attach(&chip, route);
    beep_init(&hrc_audio_pcm);
    ambeVoiceInit(&hrc_audio_pcm, &vocoder);
    ambeVoiceSetMixer(beep_mix);
    cold_permit_transmit();
    make_reference(BEEP_KEY, &key);
    make_reference(BEEP_ERROR, &error);
    make_reference(BEEP_TX_PERMIT_SHORT, &permit);
#if CONFIG_SIG_TONES
    reference_t custom;
    make_custom_reference(&custom);
#endif
    CHECK(key.count >= 190 && key.count <= 200, "own 25 ms key has full duration (%u)", key.count);
    CHECK(error.count >= 1190 && error.count <= 1200, "own 150 ms error has full duration (%u)", error.count);

    /* A pre-key permit must reach the FIFO whole before the TX gate closes. */
    begin_call();
    settle(true);
    captured_count = 0;
    beep_play(BEEP_TX_PERMIT_SHORT, 50, true);
    uint32_t permit_at = now_us;
    while ((beep_busy() || hrc_audio_pcm_spk_pending()) && now_us - permit_at < 400000u) step(true);
    CHECK(!beep_busy() && !hrc_audio_pcm_spk_pending() && now_us - permit_at < 400000u,
          "80 ms permit and buffered tail drain before the 400 ms admission bound");
    unsigned saved = captured_count;
    tx_active = true; hrc_audio_pcm_tx_gate();
    for (unsigned i = 0; i < 4; i++) step(true);
    CHECK(captured_count == saved && !(chip.audio_ctl & C7_AUDIO_RING_PLAY), "TX gate rejects beep PCM on air");
    tx_active = false; settle(true);
    check_reference("pre-key permit", &permit);
    ambeVoiceStop();

    transmit_then_beep(&key);
    transmit_then_beep(&key);
    ambeVoiceStop();

    begin_call();
    settle(true);
    CHECK(decoded_frames == 6 && ambeVoiceStreaming() && !spk_routed,
          "a received call ends while its audio stream remains alive");
    for (unsigned repeat = 0; repeat < 3; repeat++) {
        captured_count = 0;
        beep_play(BEEP_KEY, 50, true);
        settle(true);
        check_reference("after DMR", &key);
    }
    captured_count = 0;
    beep_play(BEEP_ERROR, 50, true);
    settle(true);
    check_reference("long error after DMR", &error);
#if CONFIG_SIG_TONES
    captured_count = 0;
    custom_beep(true);
    settle(true);
    check_reference("custom chain after DMR", &custom);
#endif

    /* A new request while the previous speaker hold is still up must retain a
     * full pattern; it must not inherit a consumed lead/tail from the old one. */
    beep_play(BEEP_KEY, 50, true);
    for (unsigned i = 0; i < 4; i++) step(true);
    captured_count = 0;
    beep_play(BEEP_KEY, 50, true);
    settle(true);
    check_reference("restart during hold", &key);

    /* Start a beep on the final decoded voice frame. The next block ends RX;
     * deciding the speaker route twice in that callback must not flush the beep. */
    begin_call();
    step(true);
    step(true);
    CHECK(spk_routed, "received voice has the speaker route before overlap");
    captured_count = 0;
    beep_play(BEEP_KEY, 50, true);
    settle(true);
    check_reference("voice end overlap", &key);
    CHECK(decoded_frames == 9, "overlap call drains all decoded frames");

    /* Leaving DMR during a shared lead-in discards that stream's buffers.
     * Its successor must settle a fresh route before generating any tone. */
    beep_play(BEEP_KEY, 50, true);
    step(true);
    ambeVoiceStop();
    captured_count = 0;
    beep_play(BEEP_KEY, 50, false);
    settle(false);
    check_own_reference("restart during shared lead", &key);

    /* A queued shared request must also survive losing its stream without another key press. */
    begin_call();
    settle(true);
    beep_play(BEEP_KEY, 50, true);
    step(true);
    ambeVoiceStop();
    captured_count = 0;
    beep_service(false, now_us / 1000u);
    settle(false);
    check_own_reference("pending shared request handoff", &key);

    /* Every own-stream start resets the PCM queue, even if the previous beep
     * still holds its route. A replacement must retain its complete waveform. */
    beep_play(BEEP_KEY, 50, false);
    for (unsigned i = 0; i < 4; i++) step(false);
    captured_count = 0;
    beep_play(BEEP_KEY, 50, false);
    settle(false);
    check_own_reference("repeated own request", &key);
    ambeVoiceStop();
    receive_to_transmit();
    CHECK(!running && !spk_routed, "leaving DMR releases the stream and route");
    printf("Shared beep/AMBE/real PCM queue: %u checks, %s\n", checks, failures ? "FAILED" : "PASS");
    return failures ? 1 : 0;
}
