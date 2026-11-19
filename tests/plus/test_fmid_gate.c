// SPDX-License-Identifier: GPL-3.0-or-later
// DM32 Plus FM caller ID on / off (Settings > Talk & listen, integration/radiofoundry/fmid/radio_fmid.c): the gate
// (radio_fmid_ring_wanted) and the radio task side with a fake receive ring playing DTMF "5" bursts:
//   - on (the default): the ring starts after the settle time, the decoders name the caller ("DTMF 5..."), the pager
//     hears the audio too;
//   - off: the ring stops (FM audio back on the direct path) and stays off, nothing is shown;
//   - off with the Pager / SSTV screen open: the ring runs for the screen, the pager hears the audio, the decoders
//     stay quiet; the screen closed: the ring stops again;
//   - on again: the ring and the decoders come back.
// Built by tools/test_classic.py with core/radio/radio_fmid.c and fmid.c, the audio tests' RTOS stubs.
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "board.h"
#include "radio/radio_fmid.h"
#include "radio/radio_msg.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

void audio_test_enter(void) {}
void audio_test_exit(void) {}
bool aprs_rx_listening(void) { return false; }

// --- the Pager / SSTV screen (radio_msg.c) ---
static bool screen;
static unsigned msg_samples;
bool radio_msg_wants_fm(void) { return screen; }
void radio_msg_audio(const int16_t *s, uint16_t n) { (void)s; msg_samples += n; }

// --- the receive ring: DTMF "5" (770 + 1336 Hz), 80 ms on, 60 ms off, at 8 kHz ---
static bool ring_on;
static unsigned starts, stops;
static uint32_t sample;
static bool rx_start(void) { ring_on = true; starts++; return true; }
static void rx_stop(void) { ring_on = false; stops++; }
static uint16_t rx_read(int16_t *buf, uint16_t max)
{
	if (!ring_on)
		return 0;
	// 200 samples a 25 ms poll, read in blocks
	static uint32_t budget_at;
	uint16_t n = 0;
	while (n < max && sample < budget_at) {
		uint32_t k = sample % 1120u;	// 140 ms
		double t = (double)sample / 8000.0;
		buf[n++] = k < 640u ? (int16_t)(4000.0 * sin(2 * M_PI * 770.0 * t) + 4000.0 * sin(2 * M_PI * 1336.0 * t)) : 0;
		sample++;
	}
	if (n == 0)
		budget_at = sample + 200u;	// the next poll's audio
	return n;
}
static hal_afsk_t afsk = { .rx_rate_hz = 8000, .rx_start = rx_start, .rx_read = rx_read, .rx_stop = rx_stop };
const board_t board = { .name = "fmid gate test", .afsk = &afsk };

static uint32_t now = 1000;
static void run(uint32_t ms, bool fm_rx)
{
	for (uint32_t t = 0; t < ms; t += 25u) {
		now += 25u;
		radio_fmid_poll(fm_rx, 146520000u, now);
	}
}

static uint8_t shown(void)
{
	radio_fmid_id_t id;
	radio_fmid_shown(&id);
	return id.kind;
}

int main(void)
{
	// The gate: the channel must suit; then the setting or the screen
	CHECK(!radio_fmid_ring_wanted(false, true, true));
	CHECK(radio_fmid_ring_wanted(true, true, false));
	CHECK(radio_fmid_ring_wanted(true, false, true));
	CHECK(!radio_fmid_ring_wanted(true, false, false));
	CHECK(radio_fmid_ring_wanted(true, true, true));

	// On (the default, as built): the ring after the settle time, the caller named, the pager hears it too
	run(200, true);
	CHECK(starts == 0);
	run(1500, true);
	CHECK(starts == 1 && ring_on);
	CHECK(shown() == RADIO_FMID_DTMF);
	CHECK(msg_samples > 0);

	// Off: the ring goes (the direct path), nothing shown, and it stays off
	radio_fmid_enable(false);
	run(25, true);
	CHECK(stops == 1 && !ring_on);
	CHECK(shown() == 0);
	run(3000, true);
	CHECK(starts == 1 && !ring_on && shown() == 0);

	// Off, the Pager / SSTV screen open: the ring for the screen, the pager hears it, the decoders don't
	screen = true;
	msg_samples = 0;
	run(1500, true);
	CHECK(starts == 2 && ring_on);
	CHECK(msg_samples > 0);
	CHECK(shown() == 0);
	radio_fmid_id_t logged;
	uint32_t seq = radio_fmid_logged(&logged);
	run(5000, true);
	CHECK(shown() == 0 && radio_fmid_logged(&logged) == seq);
	// ... not on a channel that doesn't suit (TX, AM, another mode)
	run(25, false);
	CHECK(!ring_on && stops == 2);
	run(1000, true);
	CHECK(ring_on && starts == 3);
	// The screen closed: the ring goes again
	screen = false;
	run(25, true);
	CHECK(!ring_on && stops == 3);

	// On again: the ring and the decoders back
	radio_fmid_enable(true);
	run(1500, true);
	CHECK(starts == 4 && ring_on && shown() == RADIO_FMID_DTMF);
	// Switched off while the screen keeps the ring: the decoders stop at once, the ring stays for the screen
	screen = true;
	radio_fmid_enable(false);
	run(25, true);
	CHECK(ring_on && stops == 3 && shown() == 0);
	run(2000, true);
	CHECK(shown() == 0);

	printf("# test_fmid_gate: %d checks, %s\n", checks, fails ? "FAILED" : "PASSED");
	return fails ? 1 : 0;
}
