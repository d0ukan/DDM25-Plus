/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus round 15 item 2 (docs/plus.md "Search ranges and speed"): the P25 and DMR search engines (p25search.c,
 * dmrsearch.c) driven as the radio task drives them, a poll every 25 ms (+-1 ms), against a model of the receiver:
 * signals on the band (P25 or a DMR repeater: a carrier all the time; DMR direct mode: 30 ms in every 60, any phase),
 * a neighbour channel 12.5 kHz away 60 dB down, a noise floor with +-1 dB of noise, and a strength reading that follows
 * the tuned frequency's level through a first-order lag (tau 0-15 ms: how slow the receiver's reading may be; the
 * retune itself is instant). Built twice by tools/test_scan.py, CONFIG_SEARCH_FAST=0 (the engines as they were) and 1,
 * and the two compared. Prints, per engine:
 *   - a pass's time over quiet ranges (+-1 MHz: 161 steps; 430-440 MHz: 801 steps; 12.5 kHz) and the time a step;
 *   - per reading lag: how many of 200 signals (random step, level -112..-60 dBm, random phase) the first pass that
 *     reaches them hits on their own frequency, and the hits elsewhere (each costs a probe);
 *   - a busy channel that is no P25 / DMR, probed and skipped: probes in 3 minutes.
 * Lines "MODEL <engine> <what> <numbers>" for the script; it checks the figures. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "radio/p25search.h"
#include "radio/dmrsearch.h"

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

enum { SIG_CONT, SIG_TDMA };
typedef struct {
	uint32_t hz;
	double dbm;
	int kind;
	unsigned phase;
} sig_t;

static sig_t sigs[2];
static int nsig;
static double tau, reading;
static uint32_t now, tuned_hz, filt_ms;
static unsigned seed = 12345, case_seed = 777;

/* The noise and the polls' jitter */
static unsigned rnd(void)
{
	seed = seed * 1103515245u + 12345u;
	return (seed >> 16) & 0x7FFFu;
}

/* The cases (their own sequence: both builds meet the same signals) */
static unsigned crnd(void)
{
	case_seed = case_seed * 1103515245u + 12345u;
	return (case_seed >> 16) & 0x7FFFu;
}

/* The level on the tuned frequency at ms t */
static double level(uint32_t t)
{
	double best = -124.0 + (double)(rnd() % 21u) / 10.0 - 1.0;
	for (int i = 0; i < nsig; i++) {
		const sig_t *s = &sigs[i];
		uint32_t off = tuned_hz > s->hz ? tuned_hz - s->hz : s->hz - tuned_hz;
		double att = off < 3125u ? 0.0 : off < 15625u ? 60.0 : 300.0;
		if (s->kind == SIG_TDMA && (t + s->phase) % 60u >= 30u)
			continue;
		if (s->dbm - att > best)
			best = s->dbm - att;
	}
	return best;
}

/* The reading at the poll: the lag run up to now in 1 ms steps */
static int16_t read_now(void)
{
	double k = tau > 0.0 ? 1.0 - exp(-1.0 / tau) : 1.0;
	for (; filt_ms < now; filt_ms++)
		reading += (level(filt_ms) - reading) * k;
	return (int16_t)lround(reading);
}

static void poll_wait(void)
{
	now += 24u + rnd() % 3u;	/* 25 ms +-1 */
}

/* --- P25 --- */

/* Sweep lo..hi for `ms`; hits: the frequencies hit, in order (a probe: given back at once, P25 or not by `p25_at`) */
static int p25_run(uint32_t lo, uint32_t hi, uint32_t ms, uint32_t *hits, int max_hits, uint32_t p25_at,
		   uint32_t *pass_ms)
{
	p25s_view_t v;
	tuned_hz = p25s_start(lo, hi, 12500u, now);
	int n = 0;
	uint32_t end = now + ms, first_wrap = 0, wraps = 0, last_sweeps = 0;
	while ((int32_t)(end - now) > 0) {
		poll_wait();
		p25s_snapshot(&v);	/* the screen looks */
		bool hit;
		uint32_t hz = p25s_sweep(read_now(), now, &hit);
		if (hit) {
			if (n < max_hits)
				hits[n] = p25s_at_hz();
			n++;
			now += 50u;	/* the hand-over: a probe that ends at once */
			hz = p25s_probe_done(p25s_at_hz() == p25_at, now);
		}
		if (hz != 0)
			tuned_hz = hz;
		p25s_snapshot(&v);
		if (v.sweeps != last_sweeps) {
			last_sweeps = v.sweeps;
			if (wraps++ == 0)
				first_wrap = now;
			else if (pass_ms != NULL)
				*pass_ms = (now - first_wrap) / (wraps - 1u);
		}
	}
	p25s_stop();
	return n;
}

static void p25_model(const char *eng)
{
	/* Quiet passes */
	uint32_t pass1 = 0, pass2 = 0, hits[4];
	nsig = 0;
	tau = 5.0;
	p25_run(433000000u, 435000000u, 60000u, hits, 4, 0, &pass1);
	p25_run(430000000u, 440000000u, 120000u, hits, 4, 0, &pass2);
	printf("MODEL %s p25-pass %u %u %.1f\n", eng, pass1, pass2, pass2 / 801.0);
	/* Detection: one P25 carrier at a random step of +-1 MHz, the hits of one pass and a bit */
	static const double taus[4] = { 0.0, 5.0, 10.0, 15.0 };
	for (int ti = 0; ti < 4; ti++) {
		tau = taus[ti];
		int found = 0, wrong = 0;
		for (int k = 0; k < 200; k++) {
			nsig = 2;
			sigs[0] = (sig_t){ 433000000u + 12500u * (crnd() % 161u), -112.0 + (double)(crnd() % 53u), SIG_CONT, 0 };
			sigs[1] = (sig_t){ 0, -200.0, SIG_CONT, 0 };
			uint32_t h[8];
			int n = p25_run(433000000u, 435000000u, 161u * 52u, h, 8, sigs[0].hz, NULL);
			bool got = false;
			for (int i = 0; i < n && i < 8; i++) {
				if (h[i] == sigs[0].hz)
					got = true;
				else
					wrong++;
			}
			found += got;
		}
		printf("MODEL %s p25-detect %.0f %d %d\n", eng, tau, found, wrong);
	}
	/* A strong FM carrier in the range, probed and skipped (no P25), for 3 minutes */
	tau = 5.0;
	nsig = 1;
	sigs[0] = (sig_t){ 433500000u, -70.0, SIG_CONT, 0 };
	uint32_t h[64];
	int n = p25_run(433000000u, 435000000u, 180000u, h, 64, 0, NULL);
	printf("MODEL %s p25-busy %d\n", eng, n);
}

/* --- DMR --- */

static int dmr_run(uint32_t lo, uint32_t hi, uint32_t ms, uint32_t *hits, int max_hits, uint32_t dmr_at,
		   uint32_t *pass_ms)
{
	dmrs_view_t v;
	tuned_hz = dmrs_start(lo, hi, 12500u, now);
	int n = 0;
	uint32_t end = now + ms, first_wrap = 0, wraps = 0, last_sweeps = 0;
	while ((int32_t)(end - now) > 0) {
		poll_wait();
		dmrs_snapshot(&v);
		uint32_t hz;
		if (dmrs_phase() == DMRS_SWEEP) {
			bool hit;
			hz = dmrs_sweep(read_now(), now, &hit);
			if (hit) {
				if (n < max_hits)
					hits[n] = dmrs_at_hz();
				n++;
			}
		} else {
			/* The probe: bursts on the DMR frequency, none elsewhere (it ends after DMRS_PROBE_MS / LATE_MS) */
			dmrs_heard_t heard;
			memset(&heard, 0, sizeof heard);
			heard.cc = DMRS_CC_NONE;
			if (dmrs_at_hz() == dmr_at)
				heard.bursts = 2;
			hz = dmrs_probe(&heard, now);
		}
		if (hz != 0)
			tuned_hz = hz;
		dmrs_snapshot(&v);
		if (v.sweeps != last_sweeps) {
			last_sweeps = v.sweeps;
			if (wraps++ == 0)
				first_wrap = now;
			else if (pass_ms != NULL)
				*pass_ms = (now - first_wrap) / (wraps - 1u);
		}
	}
	dmrs_stop();
	return n;
}

static void dmr_model(const char *eng)
{
	uint32_t pass1 = 0, pass2 = 0, hits[4];
	nsig = 0;
	tau = 5.0;
	dmr_run(433000000u, 435000000u, 60000u, hits, 4, 0, &pass1);
	dmr_run(430000000u, 440000000u, 240000u, hits, 4, 0, &pass2);
	printf("MODEL %s dmr-pass %u %u %.1f\n", eng, pass1, pass2, pass2 / 801.0);
	static const double taus[4] = { 0.0, 5.0, 10.0, 15.0 };
	for (int kind = SIG_CONT; kind <= SIG_TDMA; kind++) {
		for (int ti = 0; ti < 4; ti++) {
			tau = taus[ti];
			int found = 0, wrong = 0;
			for (int k = 0; k < 200; k++) {
				nsig = 1;
				sigs[0] = (sig_t){ 433000000u + 12500u * (crnd() % 161u), -112.0 + (double)(crnd() % 53u), kind,
						   crnd() % 60u };
				uint32_t h[8];
				/* one pass, and the probes' time */
				int n = dmr_run(433000000u, 435000000u, 161u * 105u + 3000u, h, 8, sigs[0].hz, NULL);
				bool got = false;
				for (int i = 0; i < n && i < 8; i++) {
					if (h[i] == sigs[0].hz)
						got = true;
					else
						wrong++;
				}
				found += got;
			}
			printf("MODEL %s dmr-detect-%s %.0f %d %d\n", eng, kind == SIG_TDMA ? "direct" : "repeater", tau,
			       found, wrong);
		}
	}
	tau = 5.0;
	nsig = 1;
	sigs[0] = (sig_t){ 433500000u, -70.0, SIG_CONT, 0 };
	uint32_t h[64];
	int n = dmr_run(433000000u, 435000000u, 180000u, h, 64, 0, NULL);
	printf("MODEL %s dmr-busy %d\n", eng, n);
}

int main(void)
{
	const char *eng = CONFIG_SEARCH_FAST ? "fast" : "before";
	now = 1000u;
	p25_model(eng);
	dmr_model(eng);
	CHECK(failures == 0);
	return failures != 0;
}
