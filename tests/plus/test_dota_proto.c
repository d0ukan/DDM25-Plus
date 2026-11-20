/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus DOTA Transfer: the protocol (core/dota/dota_proto.c, docs/dota.md 6 and 7) on the host. The frame and
 * manifest codecs and their refusals; then a sender and a receiver joined by a fake link that loses, duplicates and
 * reorders frames and answers, over many seeded runs: every run ends, a sender's "Done" always means the receiver
 * wrote everything exactly once, a receiver never writes part of a transfer, and a sender that gave up without an
 * answer to its END says it can't be sure (never "nothing changed" while the receiver did write). Also: a wrong
 * pairing code is never answered, a stranger's session is ignored, refusals, both sides stopping, the FM rest after a
 * minute of key time and the window's byte limit. Built by tools/test_dota.py with ax25.c and storage.c. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "radio/dota_proto.h"
#include "storage/storage.h"
#include "sim/sim_flash.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

void storage_lock(void) {}
void storage_unlock(void) {}
static band_t bands[] = { { 136000000, 174000000, true }, { 400000000, 480000000, true } };
const board_t board = { .name = "test", .bands = bands, .band_count = 2, .power_count = 3 };

/* --- A pseudo-random source --- */
static uint32_t rng = 1;
static uint32_t rnd(void)
{
	rng = rng * 1103515245u + 12345u;
	return rng >> 8;
}
static bool chance(uint32_t percent)
{
	return rnd() % 100u < percent;
}

/* --- The transfer's records: section s, record k: a length and bytes made from (s, k) --- */
static uint16_t recs[DOTA_SEC_COUNT];
static uint8_t rec_len(uint8_t s, uint16_t k)
{
	return (uint8_t)(6u + (s * 37u + k * 13u) % 35u);
}
static uint8_t rec_byte(uint8_t s, uint16_t k, uint8_t i)
{
	return (uint8_t)(s * 59u + k * 7u + i * 3u);
}
static int fills;
static size_t fill(void *ctx, uint8_t sec, uint16_t first, uint8_t *count, uint8_t *out, size_t max)
{
	(void)ctx;
	fills++;
	size_t n = 0;
	uint8_t c = 0;
	for (uint16_t k = first; k < recs[sec] && (*count == 0 || c < *count); k++) {
		uint8_t l = rec_len(sec, k);
		if (n + 1u + l > max)
			break;
		out[n++] = l;
		for (uint8_t i = 0; i < l; i++)
			out[n++] = rec_byte(sec, k, i);
		c++;
	}
	if (*count != 0 && c != *count)
		return 0;
	*count = c;
	return n;
}

/* The manifest for recs[]: frames per section as fill packs them */
static void plan(dota_manifest_t *m, uint16_t payload)
{
	memset(m, 0, sizeof *m);
	memcpy(m->model, "DM32", 5);
	m->rec_ver = DOTA_REC_VERSION;
	m->layout = DOTA_LAYOUT;
	strcpy(m->fw, "dm32-plus-test");
	uint8_t buf[DOTA_PAYLOAD_MAX];
	for (uint8_t s = 1; s < DOTA_SEC_COUNT; s++) {
		m->recs[s] = recs[s];
		for (uint16_t k = 0; k < recs[s];) {
			uint8_t c = 0;
			m->bytes += fill(NULL, s, k, &c, buf, payload);
			k = (uint16_t)(k + c);
			m->frames[s]++;
		}
	}
}

/* --- The receiver's side: what it keeps --- */
#define RECS_MAX 600
static uint8_t kept[DOTA_SEC_COUNT][RECS_MAX];
static int commits, kept_frames, queue_room, commit_delay;
static bool committed;
static uint8_t hello_answer;
static uint8_t hello(void *ctx, const dota_manifest_t *m, const char *peer)
{
	(void)ctx;
	(void)peer;
	if (m->rec_ver != DOTA_REC_VERSION)
		return DOTA_R_VERSION;
	return hello_answer;
}
static int refuse_every, data_calls;
static bool data(void *ctx, uint8_t sec, uint16_t first, uint8_t count, const uint8_t *p, uint8_t len)
{
	(void)ctx;
	if (queue_room == 0 || (refuse_every && ++data_calls % refuse_every == 0))
		return false;	/* no room in the receive queue now */
	if (queue_room > 0)
		queue_room--;
	size_t n = 0;
	for (uint8_t c = 0; c < count; c++) {
		uint16_t k = (uint16_t)(first + c);
		CHECK(n < len && p[n] == rec_len(sec, k));
		uint8_t l = p[n++];
		for (uint8_t i = 0; i < l; i++)
			CHECK(p[n + i] == rec_byte(sec, k, i));
		n += l;
		CHECK(k < RECS_MAX && kept[sec][k] == 0);	/* never twice */
		kept[sec][k]++;
		/* the sections before it are all in (the barrier) */
		for (uint8_t s = 1; s < sec; s++)
			for (uint16_t j = 0; j < recs[s]; j++)
				CHECK(kept[s][j] == 1);
	}
	CHECK(n == len);
	kept_frames++;
	return true;
}
static void commit(void *ctx)
{
	(void)ctx;
	commits++;
	for (uint8_t s = 1; s < DOTA_SEC_COUNT; s++)
		for (uint16_t j = 0; j < recs[s]; j++)
			CHECK(kept[s][j] == 1);
}

/* --- Links --- */
static const dota_link_t fm = {
	.window = 7, .frame_extra = 19, .payload_max = 128, .window_bytes = 0, .turn_ms = 150, .frame_ms = 1100,
	.ack_ms = 2500, .end_gap_ms = 4000, .idle_ms = 30000, .rest_after_ms = 60000, .rest_ms = 5000, .polls = 4,
	.tries = 8, .end_tries = 5, .call = false, .me = "TA1AAA",
};
static const dota_link_t dmr = {
	.window = 6, .frame_extra = 13, .payload_max = 192, .window_bytes = 0, .turn_ms = 0, .frame_ms = 1200,
	.ack_ms = 3000, .end_gap_ms = 4000, .idle_ms = 30000, .polls = 4, .tries = 8, .end_tries = 5, .call = true,
	.me = "TA1BBB",
};
static dota_link_t rx_link_fm, rx_link_dmr;

typedef struct {
	uint32_t loss, dup, swap, answer_loss;	/* percent */
} chan_t;

static dota_tx_t tx;
static dota_rx_t rx;
static uint32_t now;
static uint32_t tx_frames_sent, answers_sent, max_key_ms, rests;

static void reset_rx(void)
{
	memset(kept, 0, sizeof kept);
	commits = kept_frames = 0;
	queue_room = -1;
	commit_delay = 0;
	committed = false;
	hello_answer = DOTA_R_OK;
}

/* One transfer through the fake link; returns the sender's state */
static uint8_t run(const dota_link_t *l, const dota_link_t *rl, const chan_t *c, uint16_t code_tx, uint16_t code_rx)
{
	dota_manifest_t m;
	uint8_t scratch[DOTA_PAYLOAD_MAX], buf[DOTA_FRAME_MAX], win[DOTA_WINDOW_MAX + 2][DOTA_FRAME_MAX];
	size_t wlen[DOTA_WINDOW_MAX + 2];
	plan(&m, l->payload_max);
	reset_rx();
	now = 1000;
	tx_frames_sent = answers_sent = max_key_ms = 0;
	memset(rx.fw, 0, sizeof rx.fw);
	strcpy(rx.fw, "dm32-plus-test");
	dota_rx_arm(&rx, rl, code_rx, hello, data, commit, NULL, now);
	dota_tx_start(&tx, l, &m, code_tx, (uint16_t)(1 + rnd() % 60000u), fill, NULL);
	int commit_wait = -1;
	for (int guard = 0; guard < 20000; guard++) {
		uint8_t n = dota_tx_window(&tx, now, scratch);
		if (n != 0) {
			uint32_t key = 400;
			for (uint8_t i = 0; i < n; i++) {
				wlen[i] = dota_tx_frame(&tx, i, win[i], sizeof win[i]);
				CHECK(wlen[i] != 0);
				key += (uint32_t)(wlen[i] + l->frame_extra) * 7u;
			}
			if (key > max_key_ms)
				max_key_ms = key;
			/* the frames, in order but for the odd swap, some lost or twice */
			uint32_t t = now;
			for (uint8_t i = 0; i < n; i++) {
				uint8_t j = i;
				if (i + 1u < n && chance(c->swap)) {
					size_t tl = wlen[i];
					memcpy(buf, win[i], tl);
					memcpy(win[i], win[i + 1], wlen[i + 1]);
					memcpy(win[i + 1], buf, tl);
					wlen[i] = wlen[i + 1];
					wlen[i + 1] = tl;
				}
				t += (uint32_t)(wlen[j] + l->frame_extra) * 7u;
				tx_frames_sent++;
				if (!chance(c->loss)) {
					dota_rx_heard(&rx, win[j], wlen[j], l->call ? NULL : l->me, t);
					if (chance(c->dup))
						dota_rx_heard(&rx, win[j], wlen[j], l->call ? NULL : l->me, t);
				}
			}
			now = t;
			dota_tx_sent(&tx, key, now);
		}
		if (tx.state == DOTA_TX_DONE || tx.state == DOTA_TX_FAILED)
			break;
		now += 50;
		dota_rx_tick(&rx, now);
		if (rx.state == DOTA_RX_COMMIT && commit_wait < 0)
			commit_wait = commit_delay;
		if (commit_wait == 0 && rx.state == DOTA_RX_COMMIT) {
			committed = true;
			dota_rx_commit_done(&rx, true);
		}
		if (commit_wait > 0)
			commit_wait--;
		size_t a = dota_rx_answer(&rx, now, true, buf, sizeof buf);
		if (a != 0) {
			answers_sent++;
			now += 400u + (uint32_t)a * 7u;
			if (!chance(c->answer_loss))
				dota_tx_heard(&tx, buf, a, rl->call ? NULL : rl->me, now);
		}
		dota_tx_tick(&tx, now);
		if (tx.state == DOTA_TX_REST)
			rests++;
	}
	/* the receiver is left on its own a while: it finishes or gives up */
	for (int k = 0; k < 1000 && (rx.state == DOTA_RX_RECEIVING || rx.state == DOTA_RX_COMMIT); k++) {
		now += 100;
		dota_rx_tick(&rx, now);
		if (rx.state == DOTA_RX_COMMIT) {
			committed = true;
			dota_rx_commit_done(&rx, true);
		}
	}
	return tx.state;
}

/* The outcome's rules, whatever the link did */
static void outcome_rules(void)
{
	CHECK(tx.state == DOTA_TX_DONE || tx.state == DOTA_TX_FAILED);
	CHECK(commits <= 1);
	if (tx.state == DOTA_TX_DONE) {
		CHECK(rx.state == DOTA_RX_DONE && committed && commits == 1);
		for (uint8_t s = 1; s < DOTA_SEC_COUNT; s++)
			for (uint16_t j = 0; j < recs[s]; j++)
				CHECK(kept[s][j] == 1);
	}
	if (rx.state == DOTA_RX_DONE && tx.state != DOTA_TX_DONE)
		CHECK(tx.fail == DOTA_TF_UNSURE);
	if (tx.state == DOTA_TX_FAILED && tx.fail != DOTA_TF_UNSURE)
		CHECK(rx.state != DOTA_RX_DONE && !committed);
}

static void set_recs(uint16_t c, uint16_t l, uint16_t ch, uint16_t z)
{
	recs[DOTA_SEC_CONTACTS] = c;
	recs[DOTA_SEC_LISTS] = l;
	recs[DOTA_SEC_CHANNELS] = ch;
	recs[DOTA_SEC_ZONES] = z;
}

static void test_codecs(void)
{
	uint8_t buf[DOTA_FRAME_MAX], pay[DOTA_PAYLOAD_MAX];
	for (unsigned i = 0; i < DOTA_PAYLOAD_MAX; i++)
		pay[i] = (uint8_t)(i * 7);
	dota_frame_t f = { .type = DOTA_T_DATA, .flags = DOTA_F_CALL | DOTA_F_POLL, .sec = DOTA_SEC_ZONES, .left = 5,
			   .code = 9999, .session = 0xBEEF, .seq = 511, .first = 1023, .count = 9, .len = DOTA_PAYLOAD_MAX,
			   .call = "TA2DOU", .payload = pay }, g;
	size_t n = dota_frame_put(&f, buf, sizeof buf);
	CHECK(n == DOTA_FRAME_MAX);
	CHECK(dota_frame_get(buf, n, &g));
	CHECK(g.type == f.type && g.flags == f.flags && g.sec == f.sec && g.left == f.left && g.code == f.code &&
	      g.session == f.session && g.seq == f.seq && g.first == f.first && g.count == f.count && g.len == f.len &&
	      strcmp(g.call, "TA2DOU") == 0 && memcmp(g.payload, pay, f.len) == 0);
	CHECK(dota_frame_put(&f, buf, n - 1) == 0);		/* no room */
	/* a short callsign is padded, and comes back without the padding */
	strcpy(f.call, "K1A");
	f.len = 3;
	n = dota_frame_put(&f, buf, sizeof buf);
	CHECK(dota_frame_get(buf, n, &g) && strcmp(g.call, "K1A") == 0 && buf[DOTA_HDR + 3] == ' ');
	/* any bit flipped is refused (CRC), as are other lengths, versions and codes */
	for (size_t i = 0; i < n * 8; i++) {
		buf[i / 8] ^= (uint8_t)(1u << (i % 8));
		CHECK(!dota_frame_get(buf, n, &g));
		buf[i / 8] ^= (uint8_t)(1u << (i % 8));
	}
	CHECK(dota_frame_get(buf, n, &g));
	CHECK(!dota_frame_get(buf, n - 1, &g) && !dota_frame_get(buf, n + 1, &g) && !dota_frame_get(buf, 5, &g));
	f.code = 10000;
	n = dota_frame_put(&f, buf, sizeof buf);
	CHECK(!dota_frame_get(buf, n, &g));
	f.code = 1;
	f.flags = 0;
	f.len = 0;
	n = dota_frame_put(&f, buf, sizeof buf);
	CHECK(n == DOTA_HDR + 2 && dota_frame_get(buf, n, &g) && g.call[0] == '\0' && g.len == 0);
	/* random bytes never pass (and never crash) */
	for (int k = 0; k < 20000; k++) {
		size_t l = rnd() % sizeof buf;
		for (size_t i = 0; i < l; i++)
			buf[i] = (uint8_t)rnd();
		(void)dota_frame_get(buf, l, &g);
	}
	/* The manifest */
	dota_manifest_t m, b;
	memset(&m, 0, sizeof m);
	memcpy(m.model, "DM32", 5);
	m.rec_ver = 1;
	m.layout = 1;
	m.modes = 0x3F;
	m.kind = 3;
	m.recs[1] = 150;
	m.recs[2] = 76;
	m.recs[3] = 1024;
	m.recs[4] = 68;
	m.frames[1] = 17;
	m.frames[2] = 9;
	m.frames[3] = 205;
	m.frames[4] = 14;
	m.bytes = 0x01ABCD;
	strcpy(m.fw, "dm32-plus-0.1.0-dev-2026");
	n = dota_manifest_put(&m, buf, sizeof buf);
	CHECK(n == 28 + 24 && n <= DOTA_MANIFEST_MAX);
	CHECK(dota_manifest_get(buf, n, &b) && memcmp(&m, &b, sizeof m) == 0 && dota_manifest_total(&b) == 245);
	CHECK(!dota_manifest_get(buf, n - 1, &b));
	m.frames[3] = 600;	/* more than DOTA_FRAMES_MAX */
	n = dota_manifest_put(&m, buf, sizeof buf);
	CHECK(!dota_manifest_get(buf, n, &b));
	CHECK(dota_manifest_put(&m, buf, 20) == 0);
}

/* Many runs over a lossy link; at most `fail_ok` percent may fail */
static void lossy(const char *name, const dota_link_t *l, const dota_link_t *rl, const chan_t *c, int runs, int must_pass)
{
	int done = 0, unsure = 0;
	uint32_t windows_max = 0;
	for (int r = 0; r < runs; r++) {
		rng = (uint32_t)(r * 7919 + 17);
		set_recs((uint16_t)(rnd() % 20), (uint16_t)(rnd() % 4), (uint16_t)(1 + rnd() % 200), (uint16_t)(rnd() % 8));
		uint8_t st = run(l, rl, c, 4271, 4271);
		outcome_rules();
		done += st == DOTA_TX_DONE;
		unsure += st == DOTA_TX_FAILED && tx.fail == DOTA_TF_UNSURE;
		if (tx_frames_sent > windows_max)
			windows_max = tx_frames_sent;
	}
	printf("  %-34s %3d/%d done, %d unsure, most frames sent %u\n", name, done, runs, unsure, windows_max);
	CHECK(done >= must_pass);
}

static void test_transfers(void)
{
	static const chan_t clean = { 0, 0, 0, 0 }, light = { 1, 1, 1, 1 }, ten = { 10, 5, 5, 10 },
			    heavy = { 30, 10, 10, 30 };
	/* A clean link: no resends, the windows as section 8 counts them */
	rng = 5;
	set_recs(0, 0, 16, 1);
	CHECK(run(&fm, &rx_link_fm, &clean, 1234, 1234) == DOTA_TX_DONE);
	outcome_rules();
	CHECK(tx.resent == 0 && answers_sent == 1);	/* one window, one answer: HELLO, data, END */
	set_recs(10, 2, 80, 1);
	CHECK(run(&dmr, &rx_link_dmr, &clean, 1234, 1234) == DOTA_TX_DONE);
	outcome_rules();
	CHECK(tx.resent == 0 && strcmp(tx.peer, "TA1BBB") == 0);
	lossy("FM clean", &fm, &rx_link_fm, &clean, 40, 40);
	lossy("DMR clean", &dmr, &rx_link_dmr, &clean, 40, 40);
	lossy("FM 1 %", &fm, &rx_link_fm, &light, 80, 80);
	lossy("DMR 1 %", &dmr, &rx_link_dmr, &light, 80, 80);
	lossy("FM 10 % (dups, swaps)", &fm, &rx_link_fm, &ten, 80, 76);
	lossy("DMR 10 % (dups, swaps)", &dmr, &rx_link_dmr, &ten, 80, 76);
	lossy("FM 30 %", &fm, &rx_link_fm, &heavy, 60, 0);
	lossy("DMR 30 %", &dmr, &rx_link_dmr, &heavy, 60, 0);
	/* Every answer lost: the sender gives up, the receiver writes nothing (it never got END) or the sender says it
	 * can't be sure */
	static const chan_t deaf = { 0, 0, 0, 100 };
	set_recs(0, 0, 40, 1);
	CHECK(run(&fm, &rx_link_fm, &deaf, 1, 1) == DOTA_TX_FAILED);
	outcome_rules();
	CHECK(tx.fail == DOTA_TF_UNSURE || tx.fail == DOTA_TF_NO_ANSWER);
	/* The FM rest: a long transfer rests 5 s after each minute of key time */
	rests = 0;
	set_recs(0, 0, 600, 0);
	CHECK(run(&fm, &rx_link_fm, &clean, 7, 7) == DOTA_TX_DONE);
	CHECK(rests > 0);
}

static void test_refusals(void)
{
	static const chan_t clean = { 0, 0, 0, 0 };
	uint8_t scratch[DOTA_PAYLOAD_MAX], buf[DOTA_FRAME_MAX];
	/* A wrong code: nothing is ever answered, the sender gives up, nothing written */
	set_recs(2, 1, 60, 1);
	CHECK(run(&fm, &rx_link_fm, &clean, 1111, 2222) == DOTA_TX_FAILED);
	CHECK(tx.fail == DOTA_TF_NO_ANSWER && answers_sent == 0 && kept_frames == 0 && rx.state == DOTA_RX_ARMED);
	/* ... and when the whole transfer fit the first window (END went out), the sender can't be sure */
	set_recs(0, 0, 10, 1);
	CHECK(run(&fm, &rx_link_fm, &clean, 1111, 2222) == DOTA_TX_FAILED);
	CHECK(tx.fail == DOTA_TF_UNSURE && !tx.hello_ok && answers_sent == 0 && kept_frames == 0);
	set_recs(2, 1, 60, 1);
	/* The receiver refuses (no room): the sender learns why and what the receiver has. (Enough records that END is
	 * not in the first window, for the stops below) */
	set_recs(2, 1, 60, 1);
	dota_manifest_t m;
	plan(&m, fm.payload_max);
	reset_rx();
	hello_answer = DOTA_R_CHANNELS;
	strcpy(rx.fw, "dm32-plus-other");
	dota_rx_arm(&rx, &rx_link_fm, 42, hello, data, commit, NULL, 0);
	rx.free_ch = 3;
	dota_tx_start(&tx, &fm, &m, 42, 77, fill, NULL);
	uint8_t n = dota_tx_window(&tx, 0, scratch);
	CHECK(n >= 2);
	for (uint8_t i = 0; i < n; i++) {
		size_t l = dota_tx_frame(&tx, i, buf, sizeof buf);
		dota_rx_heard(&rx, buf, l, "TA1AAA", 10);
	}
	dota_tx_sent(&tx, 3000, 3000);
	CHECK(kept_frames == 0 && rx.state == DOTA_RX_ARMED);
	size_t a = dota_rx_answer(&rx, 5000, true, buf, sizeof buf);
	CHECK(a != 0);
	dota_tx_heard(&tx, buf, a, "TA1RRR", 5100);
	CHECK(tx.state == DOTA_TX_FAILED && tx.fail == DOTA_TF_REFUSED && tx.status == DOTA_R_CHANNELS &&
	      tx.free_ch == 3 && strcmp(tx.peer_fw, "dm32-plus-other") == 0 && strcmp(tx.peer, "TA1RRR") == 0);
	/* Another sender's session after ours is bound: ignored */
	hello_answer = DOTA_R_OK;
	dota_rx_arm(&rx, &rx_link_fm, 42, hello, data, commit, NULL, 0);
	dota_tx_start(&tx, &fm, &m, 42, 77, fill, NULL);
	n = dota_tx_window(&tx, 0, scratch);
	for (uint8_t i = 0; i < n; i++) {
		size_t l = dota_tx_frame(&tx, i, buf, sizeof buf);
		dota_rx_heard(&rx, buf, l, "TA1AAA", 10);
	}
	int before = kept_frames;
	dota_tx_t other;
	dota_tx_start(&other, &fm, &m, 42, 78, fill, NULL);
	uint8_t on = dota_tx_window(&other, 0, scratch);
	for (uint8_t i = 0; i < on; i++) {
		size_t l = dota_tx_frame(&other, i, buf, sizeof buf);
		dota_rx_heard(&rx, buf, l, "TA9XXX", 20);
	}
	CHECK(kept_frames == before && rx.session == 77 && strcmp(rx.peer, "TA1AAA") == 0);
	/* The receiver stops: an ABORT the sender takes as "stopped by the receiver" */
	dota_tx_sent(&tx, 3000, 3000);
	a = dota_rx_stop(&rx, DOTA_A_USER, buf, sizeof buf);
	CHECK(a != 0 && rx.state == DOTA_RX_FAILED && rx.fail == DOTA_RF_STOPPED && commits == 0);
	dota_tx_heard(&tx, buf, a, NULL, 3100);
	CHECK(tx.state == DOTA_TX_FAILED && tx.fail == DOTA_TF_RX_STOPPED);
	/* The sender stops: an ABORT, the receiver drops everything (never commits) */
	reset_rx();
	dota_rx_arm(&rx, &rx_link_fm, 42, hello, data, commit, NULL, 0);
	dota_tx_start(&tx, &fm, &m, 42, 79, fill, NULL);
	n = dota_tx_window(&tx, 0, scratch);
	for (uint8_t i = 0; i < n; i++) {
		size_t l = dota_tx_frame(&tx, i, buf, sizeof buf);
		dota_rx_heard(&rx, buf, l, "TA1AAA", 10);
	}
	a = dota_tx_stop(&tx, DOTA_A_USER, buf, sizeof buf);
	CHECK(a != 0 && tx.state == DOTA_TX_FAILED && tx.fail == DOTA_TF_STOPPED);
	dota_rx_heard(&rx, buf, a, "TA1AAA", 20);
	CHECK(rx.state == DOTA_RX_FAILED && rx.fail == DOTA_RF_TX_STOPPED && commits == 0);
	CHECK(dota_tx_stop(&tx, DOTA_A_USER, buf, sizeof buf) == 0);	/* already over: nothing more to send */
	/* The receiver gives up after its idle time */
	reset_rx();
	dota_rx_arm(&rx, &rx_link_fm, 42, hello, data, commit, NULL, 0);
	dota_tx_start(&tx, &fm, &m, 42, 80, fill, NULL);
	n = dota_tx_window(&tx, 0, scratch);
	size_t l = dota_tx_frame(&tx, 0, buf, sizeof buf);
	dota_rx_heard(&rx, buf, l, "TA1AAA", 10);
	CHECK(rx.state == DOTA_RX_RECEIVING);
	dota_rx_tick(&rx, 10 + fm.idle_ms - 1);
	CHECK(rx.state == DOTA_RX_RECEIVING);
	dota_rx_tick(&rx, 10 + fm.idle_ms);
	CHECK(rx.state == DOTA_RX_FAILED && rx.fail == DOTA_RF_IDLE && commits == 0);
	/* A full receive queue (every third frame finds no room): not acknowledged, sent again, nothing lost */
	set_recs(3, 1, 60, 2);
	rng = 99;
	refuse_every = 3;
	CHECK(run(&fm, &rx_link_fm, &clean, 5, 5) == DOTA_TX_DONE);
	outcome_rules();
	CHECK(tx.resent > 0);
	refuse_every = 0;
}

static void test_window_limit(void)
{
	/* A window keeps under its byte limit (the key time rule): at least one frame, never more bytes */
	dota_link_t l = fm;
	l.window_bytes = 700;
	static const chan_t clean = { 0, 0, 0, 0 };
	rng = 3;
	set_recs(0, 0, 200, 2);
	CHECK(run(&l, &rx_link_fm, &clean, 9, 9) == DOTA_TX_DONE);
	outcome_rules();
	CHECK(max_key_ms <= 400u + 700u * 7u + 2u * (DOTA_FRAME_MAX + 19u) * 7u);
}

int main(void)
{
	rx_link_fm = fm;
	strcpy(rx_link_fm.me, "TA1RRR");
	rx_link_dmr = dmr;
	strcpy(rx_link_dmr.me, "TA1BBB");
	test_codecs();
	test_transfers();
	test_refusals();
	test_window_limit();
	printf("dota protocol: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
