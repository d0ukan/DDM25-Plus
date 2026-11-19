// SPDX-License-Identifier: GPL-3.0-or-later
// DM32 Plus tone finder and DTMF / SelCall, radio task side (integration/radiofoundry/fmdec/radio_fmdec.c through the
// pager's radio_msg.c and FM caller ID's radio_fmid.c, as on the radio), with a fake receive ring. The ring plays what
// the FD6818 would give: its normal FM path high passes the audio at 300 Hz, the flat path (board_nxdn_flat) doesn't.
//   - Tone finder open on an FM channel: the ring starts, the flat path goes on, CTCSS 88.5 Hz is found; the tone
//     stops: it is let go and kept as the last one; the screen closed: the flat path goes off, the decoder is freed
//     (heap balanced), the last tone stays; without the flat path the tone is never found.
//   - DTMF / SelCall open with FM caller ID turned off: the ring is taken for the screen, the flat path stays off, a
//     ZVEI call is listed with its frequency, the same call again within 3 s refreshes the entry, a DTMF string after
//     it is a second entry; the set picked (CCIR) is passed to the decoder at the next poll.
//   - Another frequency: the decoder starts again.
// Built by tools/test_classic.py with core/radio/radio_fmid.c, fmid.c, radio_msg.c, pocsag.c, acars.c,
// radio_fmdec.c, tonefind.c, selcall.c, tones.c and the audio tests' RTOS stubs; the RTOS heap is malloc
// (-include stdlib.h -DpvPortMalloc=malloc -DvPortFree=free), measured with AddressSanitizer's allocator counter.
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sanitizer/allocator_interface.h>
#include "board.h"
#include "radio/radio_fmid.h"
#include "radio/radio_msg.h"
#include "radio/radio_fmdec.h"
#include "radio/selcall.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

void audio_test_enter(void) {}
void audio_test_exit(void) {}
bool aprs_rx_listening(void) { return false; }

// --- the heap: a decoder is a few hundred bytes; nothing else here allocates once running ---
static size_t heap0;
static int blocks_now(void)
{
	size_t b = __sanitizer_get_current_allocated_bytes();
	return b >= heap0 + 300u ? 1 : b == heap0 ? 0 : -1;
}
#define blocks blocks_now()

// --- the board's flat receive path ---
static bool flat;
static unsigned flat_on_calls;
bool board_nxdn_flat(bool on)
{
	if (on)
		flat_on_calls++;
	flat = on;
	return true;
}

// --- the receive ring ---
enum { SIG_NONE, SIG_CTCSS, SIG_ZVEI, SIG_DTMF };
static int sig;
static bool ring_on;
static unsigned starts, stops;
static uint32_t sample, sig_start;
static double hp_x[4], hp_y[4];
static bool rx_start(void) { ring_on = true; starts++; return true; }
static void rx_stop(void) { ring_on = false; stops++; }

static double source(uint32_t n)
{
	double t = (double)n / 8000.0;
	uint32_t k = n - sig_start;
	double v = 0;
	if (sig == SIG_CTCSS)
		v += 1000.0 * sin(2 * M_PI * 88.5 * t);
	else if (sig == SIG_ZVEI) {
		// ZVEI "12345", 70 ms a tone, every 2 s
		static const double f[5] = { 1060, 1160, 1270, 1400, 1530 };
		uint32_t at = k % 16000u;
		if (at < 5u * 560u)
			v = 4000.0 * sin(2 * M_PI * f[at / 560u] * t);
	} else if (sig == SIG_DTMF) {
		// "7#", 70 ms on, 70 ms off, once
		uint32_t at = k;
		if (at < 560u)
			v = 3000.0 * (sin(2 * M_PI * 852.0 * t) + sin(2 * M_PI * 1209.0 * t));
		else if (at >= 1120u && at < 1680u)
			v = 3000.0 * (sin(2 * M_PI * 941.0 * t) + sin(2 * M_PI * 1477.0 * t));
	}
	return v;
}

static uint16_t rx_read(int16_t *buf, uint16_t max)
{
	if (!ring_on)
		return 0;
	static uint32_t budget_at;
	uint16_t n = 0;
	while (n < max && sample < budget_at) {
		double x = source(sample++);
		if (!flat) {	// the FD6818's FM path: a 300 Hz high pass (4 poles) takes the sub-audible tone out
			for (int k = 0; k < 4; k++) {
				double y = 0.809 * (hp_y[k] + x - hp_x[k]);
				hp_x[k] = x;
				hp_y[k] = y;
				x = y;
			}
		}
		x += 400.0 * ((double)rand() / RAND_MAX - 0.5);	// receiver noise (115 rms)
		buf[n++] = (int16_t)lrint(x);
	}
	if (n == 0)
		budget_at = sample + 200u;
	return n;
}
static hal_afsk_t afsk = { .rx_rate_hz = 8000, .rx_start = rx_start, .rx_read = rx_read, .rx_stop = rx_stop };
const board_t board = { .name = "fmdec glue test", .afsk = &afsk };

static uint32_t now = 1000, rx_hz = 146520000u;
static void run(uint32_t ms)
{
	for (uint32_t t = 0; t < ms; t += 25u) {
		now += 25u;
		radio_fmid_poll(true, rx_hz, now);	// radio_sm.c: FM caller ID first, then the pager's code
		radio_msg_poll(RADIO_MSG_PAGER, rx_hz, now);
	}
}

static void play(int s)
{
	sig = s;
	sig_start = sample;
}

static radio_tonef_info_t info(void)
{
	radio_tonef_info_t i;
	radio_tonef_info(&i);
	return i;
}

static void tone_finder(void)
{
	play(SIG_CTCSS);
	radio_msg_want(RADIO_MSG_TONEF);
	run(500);
	CHECK(ring_on && flat && flat_on_calls > 0);
	CHECK(blocks == 1);	// the decoder (the ring is the board's here)
	run(2500);
	radio_tonef_info_t i = info();
	CHECK(i.state == RADIO_TONEF_FOUND && i.type == TONE_CTCSS && i.value == 885);
	CHECK(radio_msg_status() == RADIO_MSG_ST_ON);
	// the tone stops: let go, kept as the last
	play(SIG_NONE);
	run(1500);
	i = info();
	CHECK(i.state != RADIO_TONEF_FOUND && i.last_type == TONE_CTCSS && i.last_value == 885);
	run(3000);
	CHECK(info().state == RADIO_TONEF_NONE);
	// another frequency: the decoder from scratch (searching again)
	play(SIG_CTCSS);
	rx_hz = 146540000u;
	run(100);
	CHECK(info().state == RADIO_TONEF_SEARCH || info().state == RADIO_TONEF_OFF);
	run(2500);
	CHECK(info().state == RADIO_TONEF_FOUND && info().value == 885);
	// the screen closed: flat path off, decoder freed, the last tone kept
	radio_msg_want(0);
	run(100);
	CHECK(!flat && blocks == 0);
	i = info();
	CHECK(i.state == RADIO_TONEF_OFF && i.last_value == 885);
	printf("tone finder: found 88.5 Hz on the flat path, kept as the last one, flat path off and decoder freed after\n");
}

static void no_flat(void)
{
	// the same tone through the FM path's high pass (what the screen would hear without the flat path): no tone
	bool found = false;
	play(SIG_CTCSS);
	radio_msg_want(RADIO_MSG_TONEF);
	for (int k = 0; k < 160; k++) {
		run(25);
		flat = false;	// the board refuses (as on a radio without NXDN's recipe)
		radio_tonef_info_t i = info();
		if (i.state == RADIO_TONEF_FOUND)
			found = true;
	}
	CHECK(!found);
	radio_msg_want(0);
	run(100);
	CHECK(blocks == 0);
	printf("without the flat path the 300 Hz high pass leaves no tone to find\n");
}

static void selcall(void)
{
	radio_fmid_enable(false);	// FM caller ID off in Settings: the screen still gets the ring
	unsigned flat0 = flat_on_calls;
	radio_msg_want(RADIO_MSG_SELCALL);
	run(100);	// the decoder starts at the next poll
	play(SIG_ZVEI);
	run(1500);
	CHECK(ring_on && flat_on_calls == flat0 && !flat);	// the ring FM caller ID had, kept for the screen
	CHECK(blocks == 1);
	radio_selcall_t e;
	CHECK(radio_selcall_count() == 1 && radio_selcall_get(0, &e));
	CHECK(e.set == SELCALL_ZVEI && strcmp(e.text, "12345") == 0 && e.rx_hz == rx_hz);
	uint32_t first = e.ms;
	run(2000);	// the call again 2 s later: the same entry, refreshed
	CHECK(radio_selcall_count() == 1 && radio_selcall_get(0, &e) && e.ms > first);
	play(SIG_DTMF);
	run(2500);
	CHECK(radio_selcall_count() == 2 && radio_selcall_get(0, &e) && e.set == SELCALL_DTMF && strcmp(e.text, "7#") == 0);
	CHECK(radio_selcall_get(1, &e) && e.set == SELCALL_ZVEI);
	CHECK(!radio_selcall_get(2, &e));
	// the set picked reaches the decoder: CCIR only, so the ZVEI call is not heard
	radio_selcall_set(SELCALL_CCIR);
	CHECK(radio_selcall_only() == SELCALL_CCIR);
	play(SIG_ZVEI);
	run(2500);
	CHECK(radio_selcall_count() == 2);
	radio_selcall_set(SELCALL_ANY);
	run(2100);
	CHECK(radio_selcall_count() == 3);
	// the screen closed: the ring goes (FM caller ID is off), the decoder is freed, the list stays
	radio_msg_want(0);
	run(100);
	CHECK(!ring_on && blocks == 0 && radio_selcall_count() == 3);
	radio_fmid_enable(true);
	printf("DTMF / SelCall: ZVEI 12345 and DTMF 7# listed with the frequency, repeats merged, set picked, list kept\n");
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);	// no stdio buffer allocated while the heap is measured
	run(500);	// FM caller ID's ring (the board's, not from this heap)
	CHECK(ring_on);
	heap0 = __sanitizer_get_current_allocated_bytes();
	tone_finder();
	no_flat();
	selcall();
	printf("# test_fmdec_glue: %d checks, %s\n", checks, fails ? "FAILED" : "PASSED");
	return fails ? 1 : 0;
}
