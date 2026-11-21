// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Copyright (c) 2026 tylerwatt12
// HR_C7000 air interface provider and receive tap (drivers/modem/hrc7000/hrc7000_air.c; docs/spec/modem-hrc7000.md 10)
// on plain arrays standing in for the modem registers and buffers, with a stub soft modem that records what it is fed:
// the copy set-up and its undoing, every sample once and in order from the interrupt to the tap and the soft modem,
// time stamps, overrun and the receiver's restart at the gap, transmit halves per slot boundary with no stale half,
// idle on underrun, last-chunk padding, the modulation bytes set and put back, abort, the watchdog, suspend / resume,
// open / close / open, the DMR engine on the same memory after close, and a seeded fuzz of interrupt and task order.
#include <stdio.h>
#include <string.h>
#include "modem/hrc7000/hrc7000_air.h"
#include "fuzz.h"
#if CONFIG_HRC7000_NATIVE_RX
#include "air/air_syms.h"
#include "p25/p25.h"
#endif

static int failures, checks;

#define CHECK(cond) do { checks++; if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	failures++; } } while (0)

#define REG(off)	regs[(off) / 4]
#define PHY_RDY		(1u << 12)
#define HALF		HRC7000_FM_RX_SAMPLES
#define DMR_MOD_BIAS	0x01E94A4Au	// a board's DMR value: modulation bytes 0x4A
#define IDLE		0x77u

static uint32_t regs[0x600 / 4];
static uint8_t buf[0xA00];
static hrc7000_t dev;

// A synchronized RX-direction latch models the captured failure: the PHY slot clock runs but no RF TX edge is
// delivered. Quiescing alone failed on hardware; this control requires a completed PHY reset before FM -> PHY ->
// TX master. It tests the reset/IRQ endpoint contract, not proof of the silicon mechanism; RF qualification decides.
enum { DIR_SYNCED, DIR_QUIET, DIR_FM, DIR_PHY, DIR_READY, DIR_BROKEN };
static bool direction_model;
static unsigned direction_phase;
static bool direction_reset_done;
// Existing clock callback models self-release, stuck reset, or a stopped clock (bounded by reads instead).
static unsigned phy_reset_mode, phy_reset_polls, phy_reset_seen;
void hrc7000_air_test_wr(hrc7000_t *d, uint32_t off, uint32_t value)
{
	if (off == C7_SYS_SOFT_RSTN && !(value & 2u)) phy_reset_seen = 0;
	if (direction_model) {
		if (off == C7_LAYER2_CONTROL && value == 0) {
			direction_phase = REG(C7_LAYER2_TXRX_CTRL) == 0 && REG(C7_LAYER2_SLOTON) == 0 ? DIR_QUIET : DIR_BROKEN;
		} else if (off == C7_WORK_MODE && value == C7_WORK_MODE_FM) {
			direction_phase = direction_phase == DIR_QUIET ? DIR_FM : DIR_BROKEN;
		} else if (off == C7_WORK_MODE && value == C7_WORK_MODE_PHY) {
			direction_phase = direction_phase == DIR_FM ? DIR_PHY : DIR_BROKEN;
		} else if (off == C7_LAYER2_CONTROL && value == 0x82u) {
			direction_phase = direction_phase == DIR_PHY && direction_reset_done ? DIR_READY : DIR_BROKEN;
		}
	}
	d->regs[off / 4u] = value;
}

// The board's memory lent to the session, as the glue lays it: the slot engine's queues
typedef struct {
	hrc7000_air_t a;
	hrc7000_air_buf_t b;
} air_mem_t;
static hrc7000_dmr_t eng;
_Static_assert(sizeof(air_mem_t) <= sizeof(eng.evq), "the session no longer fits the engine's event queue");
// The glue (platform/hrc7000/dmr_glue.c) puts the soft modem in the engine's TX queue at its first aligned address and
// checks the fit for the target. Here pointers are 64-bit and the state larger, so it gets its own aligned memory
static air_softmodem_t sm_mem;
#define A	(&((air_mem_t *)(void *)eng.evq)->a)
#define B	(&((air_mem_t *)(void *)eng.evq)->b)
#define SM	(&sm_mem)

// --- ops ---

static uint32_t now;
static int rf_on, rf_off, pa_arms, lock_depth, lock_max, notifies;
static bool rf_state;

static void op_rf(void *ctx, bool tx)
{
	if (tx) {
		rf_on++;
	} else {
		rf_off++;
	}
	rf_state = tx;
}
static void op_pa_arm(void *ctx) { pa_arms++; }
static uint32_t op_now(void *ctx)
{
	if (phy_reset_mode && !(REG(C7_SYS_SOFT_RSTN) & 2u)) {
		phy_reset_polls++;
		phy_reset_seen++;
		if (phy_reset_mode != 3) now++;
		if (phy_reset_mode == 1 && phy_reset_seen >= 3) {
			REG(C7_SYS_SOFT_RSTN) |= 2u;
			if (direction_model) direction_reset_done = true;
		}
	}
	return now;
}
static void op_lock(void *ctx) { if (++lock_depth > lock_max) lock_max = lock_depth; }
static void op_unlock(void *ctx) { lock_depth--; }
// The chip clears SYS_INTERP_LIST when the CPU writes the bits back; an array can't, so the event hook does it
static void op_notify(void *ctx) { notifies++; REG(C7_SYS_INTERP_LIST) = 0; }

static const hrc7000_dmr_ops_t ops = {
	.rf = op_rf, .pa_arm = op_pa_arm, .now_us = op_now, .lock = op_lock, .unlock = op_unlock, .notify = op_notify,
};

// --- The stub soft modem: every sample it is given, and where it was reset ---

#define GOT_MAX	200000
static int16_t got[GOT_MAX];
static int ngot, nresets, sm_opens;
static int reset_at[4096];
static uint32_t sample_t[0x8000];	// the time each sample value was made (values wrap at 0x8000)
static int bad_times;

uint32_t air_softmodem_rx_fit(const air_profile_t *p, const hal_baseband_t *bb)
{
	return p->sym_rate * 5u <= bb->rx_rate_hz * 3u ? AIR_FIT_RX | AIR_FIT_SOFT : 0;
}

bool air_softmodem_rx_open(air_softmodem_t *m, const air_profile_t *p, uint32_t rate_hz, uint16_t counts_per_khz,
			   air_softmodem_out_t out, void *ctx)
{
	memset(m, 0, sizeof(*m));
	m->p = p;
	m->out = out;
	m->ctx = ctx;
	m->rate_hz = rate_hz;
	sm_opens++;
	return rate_hz == HRC7000_AIR_RATE_HZ;
}

void air_softmodem_rx_reset(air_softmodem_t *m)
{
	if (nresets < (int)(sizeof(reset_at) / sizeof(reset_at[0]))) {
		reset_at[nresets] = ngot;
	}
	nresets++;
}

// One chunk out per call: its first symbol's time is the first sample's, so the event path is checked too
void air_softmodem_rx(air_softmodem_t *m, const int16_t *samples, uint16_t n, uint32_t t_us)
{
	CHECK(n > 0 && n <= HALF);
	if (sample_t[(uint16_t)samples[0] & 0x7FFFu] != t_us) {
		bad_times++;
	}
	for (uint16_t i = 0; i < n && ngot < GOT_MAX; i++) {
		got[ngot++] = samples[i];
	}
	static const uint8_t hard[1] = { 0 };
	air_chunk_t c = { .t_us = t_us, .nsyms = 1, .sync_at = -1, .hard = hard };
	m->out(&c, m->ctx);
}

// --- Events ---

static air_event_t evs[4096];
static int nevs;

static void on_event(const air_event_t *ev, void *ctx)
{
	CHECK(lock_depth == 0);	// never called with the interrupts held off
	if (nevs < (int)(sizeof(evs) / sizeof(evs[0]))) {
		evs[nevs] = *ev;
		evs[nevs].chunk = NULL;
	}
	nevs++;
}

static int count_ev(uint8_t kind)
{
	int c = 0;
	for (int i = 0; i < nevs && i < (int)(sizeof(evs) / sizeof(evs[0])); i++) {
		c += evs[i].kind == kind;
	}
	return c;
}

// --- The chip ---

static uint16_t gen;	// the next sample value (15 bits)
static uint8_t next_sw = 1;

// The copy fills a half and raises system interrupt bit 14
static void copy_half(void)
{
	now += 32000;
	uint32_t t0 = now - (HALF - 1u) * 125u;
	uint32_t base = C7_FM_RX_AUDIO + (next_sw - 1u) * HALF * 2u;
	for (uint32_t i = 0; i < HALF; i++) {
		uint16_t v = gen;
		gen = (uint16_t)((gen + 1u) & 0x7FFFu);
		buf[base + 2u * i] = (uint8_t)(v >> 8);
		buf[base + 2u * i + 1u] = (uint8_t)v;
		sample_t[v] = t0 + i * 125u;
	}
	REG(C7_FM_ADDR_SW) = next_sw;
	next_sw = next_sw == 1 ? 2 : 1;
	REG(C7_SYS_INTERP_LIST) = C7_SYS_FM_RX;
	hrc7000_air_isr_sys(A);
	REG(C7_SYS_INTERP_LIST) = 0;
}

static void phy_rdy(void)
{
	REG(C7_SYS_INTERP_LIST) = PHY_RDY;
	hrc7000_air_isr_sys(A);
	REG(C7_SYS_INTERP_LIST) = 0;
}

static void slot(void)
{
	now += 30000;
	hrc7000_air_isr_timeslot(A);
}

static const volatile uint8_t *ram_half(int h) { return &buf[C7_TX_RAM + (h == 0 ? 0x00u : 0x24u)]; }

// The bytes a boundary writes: all but the last, which waits for the next boundary (half_full)
static bool half_bytes(int h, const air_tx_chunk_t *c, int nbytes)
{
	const volatile uint8_t *ram = ram_half(h);
	for (int i = 0; i < nbytes; i++) {
		uint8_t want = IDLE;
		int n = c == NULL ? 0 : c->nsyms;
		if (4 * i + 4 <= n) {
			want = c->syms[i];
		} else if (4 * i < n) {
			uint8_t keep = (uint8_t)(0xFFu << (8 - 2 * (n - 4 * i)));
			want = (uint8_t)((c->syms[i] & keep) | (IDLE & ~keep));
		}
		if (ram[i] != want) {
			return false;
		}
	}
	return true;
}

static bool half_is(int h, const air_tx_chunk_t *c) { return half_bytes(h, c, HRC7000_AIR_HALF_BYTES - 1); }
static bool half_full(int h, const air_tx_chunk_t *c) { return half_bytes(h, c, HRC7000_AIR_HALF_BYTES); }

// The idle slots after the RF TX interrupt: the queue waits, nothing counted
static void lead(void)
{
	uint32_t under = A->st.tx_underruns;
	for (int i = 0; i < HRC7000_AIR_TX_LEAD_SLOTS; i++) {
		slot();
		CHECK(half_full(0, NULL) && half_full(1, NULL));
	}
	CHECK(A->st.tx_underruns == under);
}

static air_tx_chunk_t chunk(int k, uint16_t nsyms, uint8_t flags)
{
	air_tx_chunk_t c;
	memset(&c, 0, sizeof(c));
	c.nsyms = nsyms;
	c.flags = flags;
	for (int i = 0; i < HRC7000_AIR_HALF_BYTES; i++) {
		c.syms[i] = (uint8_t)(k * 37 + i * 5 + 1);
	}
	return c;
}

static air_profile_t prof;

static void setup(void)
{
	direction_model = false;
	phy_reset_mode = 1; phy_reset_polls = phy_reset_seen = 0;
	memset(regs, 0, sizeof regs);
	memset(buf, 0, sizeof buf);
	memset(&eng, 0x5A, sizeof eng);	// the engine's leftovers: the session must not care
	now = 1000000;
	rf_on = rf_off = pa_arms = lock_depth = lock_max = notifies = 0;
	rf_state = false;
	ngot = nresets = sm_opens = nevs = bad_times = 0;
	gen = 0;
	next_sw = 1;
	hrc7000_attach(&dev, regs, buf);
	// The chip as DMR receive leaves it
	REG(C7_SYS_SOFT_RSTN) = 3;
	REG(C7_SYS_INTERP_MASK) = 0x1234u;
	REG(C7_FM_BANDWIDTH) = 0x40u;
	REG(C7_FM_DEV_COEF) = 0x33500u;
	REG(C7_WORK_MODE) = C7_WORK_MODE_DMR;
	REG(C7_RF_MOD_BIAS) = DMR_MOD_BIAS;
	hrc7000_audio_bits(&dev, C7_AUDIO_FM_PLAY | 1u, 0);
	hrc7000_air_init(A, &dev, &ops, B, SM);

	memset(&prof, 0, sizeof prof);
	prof.sym_rate = 4800;
	prof.levels = 4;
	prof.shape = AIR_SHAPE_C4FM;
	prof.dev_outer_hz = 1800;
	prof.timing = AIR_TIMING_CONTINUOUS;
	prof.idle = IDLE;
	prof.nsyncs = 1;
	prof.sync[0].nsyms = 24;
}

static bool copy_set_up(void)
{
	return REG(C7_WORK_MODE) == C7_WORK_MODE_FM && REG(C7_FM_BANDWIDTH) == 0 &&
	       REG(C7_SYS_INTERP_MASK) == (C7_SYS_FM_RX | PHY_RDY) && (REG(C7_AUDIO_CONTROL) & C7_AUDIO_FM_PLAY) == 0 &&
	       REG(C7_LAYER2_CONTROL) == 0xC0u && REG(C7_LAYER2_TXRX_CTRL) == 0 && REG(C7_FM_PTT) == 0;
}

static bool dmr_set_up(void)
{
	return REG(C7_WORK_MODE) == C7_WORK_MODE_DMR && REG(C7_FM_BANDWIDTH) == 0x40u && REG(C7_FM_DEV_COEF) == 0x33500u &&
	       REG(C7_SYS_INTERP_MASK) == 0x1234u && REG(C7_LAYER2_TXRX_CTRL) == 0;
}

// Samples got in order, one apart, except right at a reset
static bool continuous(int from)
{
	int r = 0;
	for (int i = from + 1; i < ngot; i++) {
		while (r < nresets && reset_at[r] < i) {
			r++;
		}
		bool at_reset = r < nresets && reset_at[r] == i;
		if (!at_reset && got[i] != (int16_t)((got[i - 1] + 1) & 0x7FFF)) {
			printf("  sample %d: %d after %d\n", i, got[i], got[i - 1]);
			return false;
		}
	}
	return true;
}

// --- Tests ---

static void test_pure(void)
{
	air_profile_t p = { .sym_rate = 4800, .levels = 4, .shape = AIR_SHAPE_RRC, .shape_param = 20,
			    .dev_outer_hz = 1944, .timing = AIR_TIMING_CONTINUOUS };
	CHECK(hrc7000_air_tx_fit(&p) == AIR_FIT_TX);
	p.shape = AIR_SHAPE_C4FM;
	CHECK(hrc7000_air_tx_fit(&p) == (AIR_FIT_TX | AIR_FIT_SHAPE_APPROX));
	p.sym_rate = 2400;
	CHECK(hrc7000_air_tx_fit(&p) == (AIR_FIT_TX | AIR_FIT_SHAPE_APPROX | AIR_FIT_RATE_DOUBLED));
	p.sym_rate = 1200;
	CHECK(hrc7000_air_tx_fit(&p) == 0);
	p.sym_rate = 4800;
	p.timing = AIR_TIMING_TDMA;
	CHECK(hrc7000_air_tx_fit(&p) == 0);
	p.timing = AIR_TIMING_CONTINUOUS;
	p.dev_outer_hz = 7000;
	CHECK((hrc7000_air_tx_fit(&p) & AIR_FIT_DEV_APPROX) != 0);

	// Deviation is linear in the modulation bytes: both scaled, the bias half kept
	p.dev_outer_hz = HRC7000_AIR_DMR_DEV_HZ;
	CHECK(hrc7000_air_mod_bias(&p, DMR_MOD_BIAS) == DMR_MOD_BIAS);
	p.dev_outer_hz = 1800;	// 0x4A * 1800 / 1820 = 73.2 -> 73
	CHECK(hrc7000_air_mod_bias(&p, DMR_MOD_BIAS) == 0x01E94949u);
	p.dev_outer_hz = 2880;	// 117.1 -> 117, 0x21: 52.2 -> 52
	CHECK(hrc7000_air_mod_bias(&p, 0x01E94A21u) == (0x01E90000u | 117u << 8 | 52u));
	p.dev_outer_hz = 6000;	// clamped
	CHECK((hrc7000_air_mod_bias(&p, 0x0000FFFFu) & 0xFFFFu) == 0xFFFFu);
	p.dev_outer_hz = 1;
	CHECK((hrc7000_air_mod_bias(&p, DMR_MOD_BIAS) & 0xFFFFu) == 0x0101u);
}

static void test_tap(void)
{
	setup();
	CHECK(hrc7000_air_tap_start(A));
	CHECK(copy_set_up() && lock_depth == 0);
	CHECK(!hrc7000_air_tap_start(A));
	CHECK(!hrc7000_air_open(A, &prof, AIR_FIT_RX, on_event, NULL));	// one at a time

	// Halves read back in pieces of every size, each sample once, timed
	int16_t out[600];
	uint16_t want = 0;
	bool ok = true, times = true;
	for (int k = 0; k < 40; k++) {
		copy_half();
		uint16_t sizes[3] = { (uint16_t)(1 + k % 7), (uint16_t)(100 + k), 600 };
		for (int s = 0; s < 3; s++) {
			uint32_t t = hrc7000_air_tap_time(A);
			uint16_t n = hrc7000_air_tap_read(A, out, sizes[s]);
			if (n > 0 && t != sample_t[(uint16_t)out[0]]) {
				times = false;
			}
			for (uint16_t i = 0; i < n; i++) {
				ok &= out[i] == (int16_t)want;
				want = (uint16_t)((want + 1u) & 0x7FFFu);
			}
		}
	}
	CHECK(ok && times && want == (uint16_t)(40u * HALF));
	CHECK(A->st.halves == 40 && A->st.overruns == 0);

	// Not read: the ring takes two halves, the third is dropped, and the read after shows the jump by its time
	copy_half();
	copy_half();
	copy_half();
	CHECK(A->st.overruns == 1);
	CHECK(hrc7000_air_tap_read(A, out, 600) == 2 * HALF);
	CHECK(out[0] == (int16_t)want && out[2 * HALF - 1] == (int16_t)((want + 2 * HALF - 1) & 0x7FFF));
	copy_half();
	uint32_t t = hrc7000_air_tap_time(A);
	CHECK(hrc7000_air_tap_read(A, out, 600) == HALF);
	CHECK(out[0] == (int16_t)((want + 3 * HALF) & 0x7FFF) && t == sample_t[(uint16_t)out[0]]);

	// FM_ADDR_SW naming neither half: nothing taken
	REG(C7_FM_ADDR_SW) = 0;
	REG(C7_SYS_INTERP_LIST) = C7_SYS_FM_RX;
	hrc7000_air_isr_sys(A);
	REG(C7_SYS_INTERP_LIST) = 0;
	CHECK(A->st.no_half == 1 && hrc7000_air_tap_read(A, out, 600) == 0);

	hrc7000_air_tap_stop(A);
	CHECK(dmr_set_up() && lock_depth == 0 && rf_on == 0);
	CHECK((REG(C7_AUDIO_CONTROL) & C7_AUDIO_FM_PLAY) == 0);	// the board's DMR entry sets its own audio paths
	copy_half();
	CHECK(hrc7000_air_tap_read(A, out, 600) == 0);
}

static void test_rx_session(void)
{
	setup();
	CHECK(!hrc7000_air_open(A, &prof, 0, on_event, NULL));
	CHECK(hrc7000_air_open(A, &prof, AIR_FIT_RX | AIR_FIT_TX, on_event, NULL));
	CHECK(copy_set_up() && sm_opens == 1 && lock_depth == 0);

	for (int k = 0; k < 30; k++) {
		copy_half();
		if (k % 3 != 2) {
			hrc7000_air_poll(A);
		}
	}
	hrc7000_air_poll(A);
	CHECK(ngot == 30 * HALF && got[0] == 0 && continuous(0) && bad_times == 0);
	CHECK(nresets == 1 && reset_at[0] == 0);	// the first half follows a new set-up
	CHECK(count_ev(AIR_EV_RX) > 0 && count_ev(AIR_EV_RX) == (int)A->st.chunks);

	// The task behind: the ring holds two halves, the third is lost, and the receiver restarts exactly where the
	// samples jump
	copy_half();
	copy_half();
	copy_half();
	hrc7000_air_poll(A);
	CHECK(A->st.overruns == 1 && ngot == 32 * HALF && nresets == 1 && continuous(0));
	copy_half();
	hrc7000_air_poll(A);
	CHECK(ngot == 33 * HALF && nresets == 2 && reset_at[1] == 32 * HALF);
	CHECK(got[32 * HALF] == (int16_t)((33 * HALF) & 0x7FFF) && continuous(0));

	// A restart drops what the ring holds and starts the receiver over at the next half
	copy_half();
	hrc7000_air_rx_restart(A);
	copy_half();
	hrc7000_air_poll(A);
	CHECK(ngot == 34 * HALF && nresets == 3 && reset_at[2] == 33 * HALF && continuous(0));
	CHECK(got[33 * HALF] == (int16_t)((35 * HALF) & 0x7FFF));
	CHECK(bad_times == 0 && lock_max == 1);

	hrc7000_air_close(A, true);
	CHECK(dmr_set_up());
	int before = nevs;
	copy_half();
	hrc7000_air_poll(A);
	CHECK(nevs == before);
}

// Four chunks, the last short: each boundary writes only the half the next slot reads
static void test_tx(void)
{
	setup();
	CHECK(hrc7000_air_open(A, &prof, AIR_FIT_RX | AIR_FIT_TX, on_event, NULL));
	air_tx_chunk_t c[4] = { chunk(0, 144, 0), chunk(1, 144, 0), chunk(2, 144, 0), chunk(3, 50, AIR_TX_LAST) };
	CHECK(!hrc7000_air_tx_begin(A));	// nothing queued
	CHECK(hrc7000_air_tx_space(A) == HRC7000_AIR_TXQ);
	for (int i = 0; i < 4; i++) {
		CHECK(hrc7000_air_tx_queue(A, &c[i]));
	}
	CHECK(hrc7000_air_tx_space(A) == 0 && !hrc7000_air_tx_queue(A, &c[0]));

	CHECK(hrc7000_air_tx_begin(A));
	CHECK(REG(C7_WORK_MODE) == C7_WORK_MODE_PHY && REG(C7_LAYER2_CONTROL) == 0x82u && REG(C7_AUDIO_BUFFER_CLR) == 3);
	CHECK(REG(C7_RF_MOD_BIAS) == 0x01E94949u);
	CHECK(half_is(0, NULL) && half_is(1, NULL));
	CHECK(!hrc7000_air_tx_begin(A) && hrc7000_air_tx_active(A));
	uint32_t t_begin = now;

	// Halves while the chip sends: ignored, not counted lost
	copy_half();
	CHECK(A->st.halves == 0 && A->st.overruns == 0);

	// A boundary before the RF TX interrupt: idle, the queue waits
	REG(C7_LAYER2_TXRX_CTRL) = 0;
	now = t_begin + 10000;
	hrc7000_air_isr_timeslot(A);
	CHECK(half_full(0, NULL) && half_full(1, NULL) && REG(C7_LAYER2_TXRX_CTRL) == 0x80u);
	CHECK(hrc7000_air_tx_space(A) == 0);

	// The RF TX interrupt keys the RF once, then idle slots while the RF chain comes up
	hrc7000_air_isr_rftx(A);
	hrc7000_air_isr_rftx(A);
	CHECK(rf_on == 1 && pa_arms == 1 && rf_state);
	lead();
	CHECK(hrc7000_air_tx_space(A) == 0);
	uint32_t slots0 = A->st.tx_slots;
	int h = A->half, g = h ^ 1;	// the idle slots moved which half comes first

	// Each boundary writes the half the next slot reads, its last byte at the boundary after
	slot();
	uint32_t b1 = now;
	CHECK(half_is(h, &c[0]) && !half_full(h, &c[0]) && half_full(g, NULL));
	// The same boundary's PHY_RDY: nothing more
	now += 800;
	phy_rdy();
	CHECK(A->st.tx_slots == slots0 + 1);

	slot();
	CHECK(half_is(g, &c[1]) && half_full(h, &c[0]));
	slot();
	CHECK(half_is(h, &c[2]) && half_full(g, &c[1]));
	slot();
	CHECK(half_is(g, &c[3]) && half_full(h, &c[2]));	// the last one padded with idle
	CHECK(rf_state && rf_off == 0);
	slot();
	CHECK(half_is(h, NULL) && half_full(g, &c[3]) && rf_state);	// the tail: idle while the last chunk leaves the air
	CHECK(A->st.tx_slots == slots0 + 5 && A->st.tx_underruns == 0);
	slot();
	CHECK(!rf_state && rf_off == 1 && !hrc7000_air_tx_active(A));
	CHECK(REG(C7_RF_MOD_BIAS) == DMR_MOD_BIAS && copy_set_up());

	hrc7000_air_poll(A);
	CHECK(count_ev(AIR_EV_TX_STARTED) == 1 && count_ev(AIR_EV_TX_SPACE) == 1 && count_ev(AIR_EV_TX_DONE) == 1);	// in a row: one
	CHECK(count_ev(AIR_EV_TX_UNDERRUN) == 0);
	CHECK(evs[0].kind == AIR_EV_TX_STARTED && evs[0].t_us == b1 + 5000u + 104u);
	CHECK(evs[nevs - 1].kind == AIR_EV_TX_DONE);

	// Receiving again, the receiver restarted
	int resets = nresets;
	copy_half();
	hrc7000_air_poll(A);
	CHECK(ngot == HALF && nresets == resets + 1 && lock_depth == 0);
	hrc7000_air_close(A, true);
	CHECK(dmr_set_up() && REG(C7_RF_MOD_BIAS) == DMR_MOD_BIAS);
}

static void test_underrun_and_end(void)
{
	setup();
	CHECK(hrc7000_air_open(A, &prof, AIR_FIT_TX, on_event, NULL));	// transmit only: no copy set-up
	CHECK(REG(C7_WORK_MODE) == C7_WORK_MODE_DMR && (REG(C7_AUDIO_CONTROL) & C7_AUDIO_FM_PLAY) == 0);
	air_tx_chunk_t c0 = chunk(0, 144, 0), c1 = chunk(1, 144, 0);
	CHECK(hrc7000_air_tx_queue(A, &c0) && hrc7000_air_tx_begin(A));
	hrc7000_air_isr_rftx(A);
	lead();
	slot();
	CHECK(half_is(0, &c0));
	slot();
	CHECK(half_is(1, NULL) && half_full(0, &c0) && A->st.tx_underruns == 1);
	slot();
	CHECK(half_is(0, NULL) && A->st.tx_underruns == 2);
	CHECK(hrc7000_air_tx_queue(A, &c1));
	slot();
	CHECK(half_is(1, &c1));
	hrc7000_air_tx_end(A);	// nothing queued: ends with what went
	CHECK(hrc7000_air_tx_space(A) == 0 && !hrc7000_air_tx_queue(A, &c0));
	slot();
	CHECK(half_is(0, NULL) && rf_state);
	slot();
	CHECK(!rf_state && !hrc7000_air_tx_active(A));
	CHECK(REG(C7_WORK_MODE) == C7_WORK_MODE_FM && REG(C7_LAYER2_CONTROL) == 0xC0u);	// no copy: out of the PHY
	hrc7000_air_poll(A);
	CHECK(count_ev(AIR_EV_TX_UNDERRUN) == 1 && count_ev(AIR_EV_TX_SPACE) == 2 && count_ev(AIR_EV_TX_DONE) == 1);
	int order[5] = { AIR_EV_TX_STARTED, AIR_EV_TX_SPACE, AIR_EV_TX_UNDERRUN, AIR_EV_TX_SPACE, AIR_EV_TX_DONE };
	bool in_order = nevs == 5;
	for (int i = 0; i < 5 && in_order; i++) {
		in_order = evs[i].kind == order[i];
	}
	CHECK(in_order);

	// tx_end with a chunk queued makes it the last
	nevs = 0;
	CHECK(hrc7000_air_tx_queue(A, &c0) && hrc7000_air_tx_queue(A, &c1));
	hrc7000_air_tx_end(A);
	CHECK(hrc7000_air_tx_begin(A));
	hrc7000_air_isr_rftx(A);
	lead();
	slot();
	slot();
	CHECK(half_is(1, &c1));
	slot();
	slot();
	CHECK(!rf_state);
	hrc7000_air_poll(A);
	CHECK(count_ev(AIR_EV_TX_DONE) == 1 && count_ev(AIR_EV_TX_UNDERRUN) == 0);
	hrc7000_air_close(A, true);
}

static void test_abort_watchdog_suspend(void)
{
	setup();
	CHECK(hrc7000_air_open(A, &prof, AIR_FIT_RX | AIR_FIT_TX, on_event, NULL));
	air_tx_chunk_t c0 = chunk(0, 144, 0);

	// Abort mid-transmission: the RF off now, the bytes back, TX_DONE
	CHECK(hrc7000_air_tx_queue(A, &c0) && hrc7000_air_tx_queue(A, &c0) && hrc7000_air_tx_begin(A));
	hrc7000_air_isr_rftx(A);
	slot();
	hrc7000_air_tx_abort(A);
	CHECK(!rf_state && !hrc7000_air_tx_active(A) && REG(C7_RF_MOD_BIAS) == DMR_MOD_BIAS && copy_set_up());
	CHECK(hrc7000_air_tx_space(A) == HRC7000_AIR_TXQ);
	hrc7000_air_poll(A);
	CHECK(count_ev(AIR_EV_TX_DONE) == 1);

	// Never keyed (no RF TX interrupt): dropped after 200 ms
	CHECK(hrc7000_air_tx_queue(A, &c0) && hrc7000_air_tx_begin(A));
	slot();
	hrc7000_air_poll(A);
	CHECK(hrc7000_air_tx_active(A));
	now += 200000;
	hrc7000_air_poll(A);
	CHECK(!hrc7000_air_tx_active(A) && A->st.stalls == 1 && rf_on == 1 && count_ev(AIR_EV_TX_DONE) == 2);

	// Keyed, then the slot clock stops: dropped after 200 ms
	CHECK(hrc7000_air_tx_queue(A, &c0) && hrc7000_air_tx_begin(A));
	hrc7000_air_isr_rftx(A);
	slot();
	now += 199000;
	hrc7000_air_poll(A);
	CHECK(rf_state);
	now += 2000;
	hrc7000_air_poll(A);
	CHECK(!rf_state && A->st.stalls == 2 && REG(C7_RF_MOD_BIAS) == DMR_MOD_BIAS);

	// The board leaves DMR under the session: the transmission ends, nothing taken until it is back
	CHECK(hrc7000_air_tx_queue(A, &c0) && hrc7000_air_tx_begin(A));
	hrc7000_air_isr_rftx(A);
	slot();
	hrc7000_air_suspend(A);
	CHECK(!rf_state && !hrc7000_air_tx_active(A));
	CHECK(!hrc7000_air_tx_queue(A, &c0) || !hrc7000_air_tx_begin(A));
	hrc7000_air_tx_abort(A);
	REG(C7_WORK_MODE) = 0x55;	// the board's FM set-up
	uint32_t halves = A->st.halves;
	copy_half();
	CHECK(A->st.halves == halves);
	hrc7000_air_resume(A);
	CHECK(copy_set_up());
	int resets = nresets;
	copy_half();
	hrc7000_air_poll(A);
	CHECK(A->st.halves == halves + 1 && nresets == resets + 1);
	hrc7000_air_close(A, true);
	CHECK(dmr_set_up() && lock_depth == 0);
}

// Close mid-transmission, open again, then the DMR engine on the same memory
static void test_open_close_dmr(void)
{
	setup();
	CHECK(hrc7000_air_open(A, &prof, AIR_FIT_RX | AIR_FIT_TX, on_event, NULL));
	air_tx_chunk_t c0 = chunk(0, 144, 0);
	CHECK(hrc7000_air_tx_queue(A, &c0) && hrc7000_air_tx_begin(A));
	hrc7000_air_isr_rftx(A);
	slot();
	hrc7000_air_close(A, true);
	CHECK(!rf_state && dmr_set_up() && REG(C7_RF_MOD_BIAS) == DMR_MOD_BIAS);
	hrc7000_air_poll(A);
	CHECK(count_ev(AIR_EV_TX_DONE) == 0);	// the session is over: nothing goes out
	slot();
	CHECK(A->st.tx_slots == 1);

	CHECK(hrc7000_air_open(A, &prof, AIR_FIT_RX, on_event, NULL));
	CHECK(copy_set_up() && hrc7000_air_tx_queue(A, &c0));
	CHECK(!hrc7000_air_tx_begin(A));	// receive-only fit
	hrc7000_air_tx_abort(A);
	int from = ngot;
	copy_half();
	hrc7000_air_poll(A);
	CHECK(ngot == from + (int)HALF);
	hrc7000_air_close(A, true);
	CHECK(dmr_set_up());

	// The board gives the queues back: the engine runs and receives as before
	eng.evq_head = eng.evq_tail = 0;
	eng.txq_head = eng.txq_tail = 0;
	hrc7000_dmr_init(&eng, &dev, &ops);
	hrc7000_dmr_run(&eng);
	CHECK(REG(C7_LAYER2_CONTROL) == 0xC0u && REG(C7_LAYER2_SLOTON) == 0x05u);
	for (int i = 0; i < 12; i++) {
		buf[C7_RX_RAM + i] = (uint8_t)(0x10 + i);
	}
	now += 30000;
	hrc7000_dmr_isr_timeslot(&eng, false);
	// A voice LC header: burst type 1, CC 3, MS sync, chip slot 1
	REG(C7_RX_TYPE_INFO) = 1u << 8 | 3u << 4 | 2u | 1u << 19;
	REG(C7_SYS_INTERP_LIST) = 1u << 3;
	hrc7000_dmr_isr_sys(&eng);
	REG(C7_SYS_INTERP_LIST) = 0;
	dmr_event_t ev;
	dmr_burst_t b;
	int bursts = 0;
	while (hrc7000_dmr_pop(&eng, &ev, &b)) {
		if (ev.kind == DMR_EV_BURST_RX) {
			bursts++;
			CHECK(b.kind == DMR_BURST_VOICE_LC_HEADER && b.cc == 3 && b.lc[0] == 0x10);
		}
	}
	if (bursts != 1) {
		printf("  dmr: rx_lc_header %u unusable %u crc %u ticks %u\n", (unsigned)eng.st.rx_lc_header,
		       (unsigned)eng.st.rx_unusable, (unsigned)eng.st.rx_crc_err, (unsigned)eng.st.ticks);
	}
	CHECK(bursts == 1);
	hrc7000_dmr_halt(&eng);
}

// The halted DMR engine cannot clear an air session's lost system request. A still-high request line must fall
// before this PIC can see the next interrupt, even though the software receive ring has never overrun.
static void test_sys_recovery(void)
{
	setup();
	CHECK(hrc7000_air_open(A, &prof, AIR_FIT_RX | AIR_FIT_TX, on_event, NULL));
	eng.running = false; // As in the glue: the air provider, not the normal DMR engine, owns these requests.
	copy_half(); hrc7000_air_poll(A);
	int resets = nresets;
	REG(C7_SYS_INTERP_LIST) = C7_SYS_FM_RX;
	REG(C7_SYS_INTERP_CLEAR) = 0;
	now += 2000000u; hrc7000_air_poll(A); // A late first poll starts the wait; it does not discard a fresh cause.
	CHECK(REG(C7_SYS_INTERP_CLEAR) == 0);
	now += 50000u; hrc7000_air_poll(A);
	CHECK(REG(C7_SYS_INTERP_CLEAR) == 0);
	now++; hrc7000_air_poll(A);
	CHECK(REG(C7_SYS_INTERP_CLEAR) == C7_SYS_FM_RX && !rf_state);
	REG(C7_SYS_INTERP_LIST) = 0; // Model the hardware write-one-to-clear side effect.
	copy_half(); hrc7000_air_poll(A);
	CHECK(ngot == 2 * HALF && nresets == resets + 1); // The lost input is a gap, not spliced into the next half.

	// A masked source is not a lost enabled request.
	REG(C7_SYS_INTERP_LIST) = 1u << 13;
	REG(C7_SYS_INTERP_CLEAR) = 0;
	hrc7000_air_poll(A); now += 100000u; hrc7000_air_poll(A);
	CHECK(REG(C7_SYS_INTERP_CLEAR) == 0);
	REG(C7_SYS_INTERP_LIST) = 0;

	// An ISR that ran since the observation renews progress, even if another cause is already pending.
	REG(C7_SYS_INTERP_MASK) |= 1u << 16;
	REG(C7_SYS_INTERP_LIST) = 1u << 16;
	hrc7000_air_poll(A); now += 50001u;
	hrc7000_air_isr_sys(A); REG(C7_SYS_INTERP_CLEAR) = 0;
	hrc7000_air_poll(A);
	CHECK(REG(C7_SYS_INTERP_CLEAR) == 0);
	now += 50001u; hrc7000_air_poll(A); // First pending observation after that ISR, not its deadline.
	CHECK(REG(C7_SYS_INTERP_CLEAR) == 0);
	REG(C7_SYS_INTERP_LIST) = 0; hrc7000_air_poll(A);

	// Retune and resume clear old observations: a request arriving under the new setup gets its own 50 ms.
	REG(C7_SYS_INTERP_LIST) = C7_SYS_FM_RX; hrc7000_air_poll(A); now += 100000u;
	hrc7000_air_rx_restart(A); REG(C7_SYS_INTERP_CLEAR) = 0;
	hrc7000_air_poll(A); CHECK(REG(C7_SYS_INTERP_CLEAR) == 0);
	hrc7000_air_suspend(A); now += 100000u; hrc7000_air_resume(A);
	REG(C7_SYS_INTERP_LIST) = C7_SYS_FM_RX; REG(C7_SYS_INTERP_CLEAR) = 0;
	hrc7000_air_poll(A); CHECK(REG(C7_SYS_INTERP_CLEAR) == 0);

	// TX changes the request owner too. Do not carry an expired RX observation into a fresh PHY request.
	air_tx_chunk_t c = chunk(9, 144, AIR_TX_LAST);
	now += 100000u;
	CHECK(hrc7000_air_tx_queue(A, &c) && hrc7000_air_tx_begin(A));
	REG(C7_SYS_INTERP_LIST) = PHY_RDY; REG(C7_SYS_INTERP_CLEAR) = 0;
	hrc7000_air_poll(A); CHECK(REG(C7_SYS_INTERP_CLEAR) == 0);
	REG(C7_SYS_INTERP_LIST) = 0; hrc7000_air_tx_abort(A); hrc7000_air_poll(A);

	// The deadline works across the microsecond clock's wrap, and never services an inactive owner.
	now = UINT32_MAX - 20000u;
	REG(C7_SYS_INTERP_LIST) = C7_SYS_FM_RX; REG(C7_SYS_INTERP_CLEAR) = 0;
	hrc7000_air_poll(A); now += 50001u; hrc7000_air_poll(A);
	CHECK(REG(C7_SYS_INTERP_CLEAR) == C7_SYS_FM_RX);
	REG(C7_SYS_INTERP_LIST) = 0; hrc7000_air_close(A, true);
	REG(C7_SYS_INTERP_LIST) = PHY_RDY; REG(C7_SYS_INTERP_CLEAR) = 0;
	hrc7000_air_poll(A); now += 100000u; hrc7000_air_poll(A);
	CHECK(REG(C7_SYS_INTERP_CLEAR) == 0 && lock_depth == 0);
}

// Model the PIC edge contract and the modem's write-one-to-clear requests. A source already high cannot deliver
// a new interrupt. Keep RF RX and both PCM requests pending: an air transition must never acknowledge them.
static void pending_clears(uint32_t *pending)
{
	*pending &= ~REG(C7_INTERP_CLEAR);
	REG(C7_INTERP_CLEAR) = 0;
	REG(C7_SYS_INTERP_LIST) &= ~REG(C7_SYS_INTERP_CLEAR);
	REG(C7_SYS_INTERP_CLEAR) = 0;
}

static bool pending_edge(uint32_t *pending, uint32_t bit)
{
	if ((*pending & bit) != 0) return false;
	*pending |= bit;
	if (bit == C7_INT_RF_TX) hrc7000_air_isr_rftx(A);
	else hrc7000_air_isr_timeslot(A);
	pending_clears(pending);
	return true;
}


static void tx_reset_refusal(bool native)
{
	const uint16_t owned[] = { C7_WORK_MODE, C7_LAYER2_CONTROL, C7_LAYER2_SLOTON, C7_LAYER2_TXRX_CTRL,
		C7_SYS_SOFT_RSTN, C7_SYS_INTERP_MASK, C7_RF_MODE, C7_RF_MOD_BIAS, C7_RECV_MS_SYNC_H, C7_RECV_MS_SYNC_L };
	air_tx_chunk_t c = chunk(13, 144, AIR_TX_LAST);
	for (unsigned scenario = 0; scenario < 3; scenario++) {
		uint32_t saved[sizeof owned / sizeof owned[0]];
		for (unsigned i = 0; i < sizeof owned / sizeof owned[0]; i++) saved[i] = REG(owned[i]);
		bool copying = A->copy;
		uint32_t head = A->head, tail = A->tail, done = A->st.tx_done, blocks = phy_reset_polls;
		int keys = rf_on, arms = pa_arms, events = nevs;
		CHECK(!hrc7000_air_tx_begin(A) && phy_reset_polls == blocks); // No reset for an unprimed request.
		phy_reset_mode = scenario == 1 ? 3 : 2; // stopped clock, or reset stuck with the clock advancing
		if (scenario == 2) now = UINT32_MAX - 50u;
		uint32_t t0 = now;
		CHECK(hrc7000_air_tx_queue(A, &c));
		bool started = hrc7000_air_tx_begin(A);
		CHECK(!started);
		CHECK(phy_reset_polls - blocks == (scenario == 1 ? 1024u : 100u));
		CHECK((uint32_t)(now - t0) == (scenario == 1 ? 0u : 100u)); // Includes unsigned clock wrap.
		CHECK(A->tx == HRC7000_AIR_RX && A->copy == copying && !A->pa && !A->mod_saved);
		CHECK(A->head == head && A->tail == tail && A->st.tx_done == done && rf_on == keys && pa_arms == arms);
		CHECK(hrc7000_air_tx_space(A) == HRC7000_AIR_TXQ && !A->end_req && nevs == events && lock_depth == 0);
		for (unsigned i = 0; i < sizeof owned / sizeof owned[0]; i++) CHECK(REG(owned[i]) == saved[i]);
#if CONFIG_HRC7000_NATIVE_RX
		CHECK(!native || (A->native_on && A->native_saved && A->native_rx));
#else
		(void)native;
#endif
		if (started) { hrc7000_air_tx_abort(A); hrc7000_air_poll(A); } // Keep the old-source negative control bounded.
		phy_reset_mode = 1;
		blocks = phy_reset_polls;
		CHECK(hrc7000_air_tx_queue(A, &c) && hrc7000_air_tx_begin(A));
		CHECK(phy_reset_polls == blocks + 3);
		hrc7000_air_isr_rftx(A); slot();
		hrc7000_air_tx_abort(A); hrc7000_air_poll(A);
		CHECK(!rf_state && A->tx == HRC7000_AIR_RX && A->copy == copying && phy_reset_polls == blocks + 3);
	}
}

static void test_tx_reset(void)
{
	setup();
	CHECK(hrc7000_air_open(A, &prof, AIR_FIT_RX | AIR_FIT_TX, on_event, NULL));
	tx_reset_refusal(false);
	copy_half(); hrc7000_air_poll(A);
	CHECK(ngot == HALF && !bad_times); // The configured copy path is still usable after refusal and retry.
	hrc7000_air_close(A, true);
}

static void test_tx_pending_requests(void)
{
	setup();
	prof.shape_param = 20;
	prof.sync_every_syms = 864;
	const uint8_t sync[6] = { 0x55, 0x75, 0xF5, 0xFF, 0x77, 0xFF };
	memcpy(prof.sync[0].syms, sync, sizeof sync);
#if CONFIG_HRC7000_NATIVE_RX
	phy_reset_mode = 1; phy_reset_polls = 0; REG(C7_SYS_SOFT_RSTN) = 3;
#endif
	CHECK(hrc7000_air_open(A, &prof, AIR_FIT_RX | AIR_FIT_TX, on_event, NULL));
	const uint32_t owned = C7_INT_TS_TX | C7_INT_TS_RX | C7_INT_RF_TX;
	const uint32_t others = (1u << 3) | C7_INT_PCM_WR | C7_INT_PCM_RD;
	uint32_t pending = owned | others;
	REG(C7_INTERP_CLEAR) = REG(C7_SYS_INTERP_CLEAR) = 0;
	REG(C7_SYS_INTERP_LIST) = PHY_RDY;
	air_tx_chunk_t c = chunk(10, 144, AIR_TX_LAST);
	CHECK(hrc7000_air_tx_queue(A, &c) && hrc7000_air_tx_begin(A));
	pending_clears(&pending);
	CHECK(pending == others && REG(C7_SYS_INTERP_LIST) == 0);
	CHECK(pending_edge(&pending, C7_INT_RF_TX) && rf_state && pa_arms == 1);
	for (unsigned i = 0; i < HRC7000_AIR_TX_LEAD_SLOTS + 3u; i++) {
		now += 30000u;
		CHECK(pending_edge(&pending, C7_INT_TS_TX));
	}
	hrc7000_air_poll(A); pending_clears(&pending);
	CHECK(!hrc7000_air_tx_active(A) && !rf_state && A->st.stalls == 0);
	CHECK(A->st.tx_done == 1 && A->st.tx_underruns == 0 && pending == others);
	hrc7000_air_tx_abort(A); // Harmless after completion; permits cleanup of the deliberately broken baseline.
	pending_clears(&pending);

	// A later lost-clock abort must also clear owned requests, including for a session using the sample receiver.
	pending = others; REG(C7_SYS_INTERP_LIST) = 0;
	CHECK(hrc7000_air_tx_queue(A, &c) && hrc7000_air_tx_begin(A));
	pending_clears(&pending);
	pending |= owned; REG(C7_SYS_INTERP_LIST) = PHY_RDY;
	now += 200001u; hrc7000_air_poll(A); pending_clears(&pending);
	CHECK(!hrc7000_air_tx_active(A) && !rf_state && A->st.stalls == 1 && pending == others);
	CHECK(REG(C7_SYS_INTERP_LIST) == 0);
	CHECK(hrc7000_air_tx_queue(A, &c) && hrc7000_air_tx_begin(A));
	pending_clears(&pending);
	CHECK(pending_edge(&pending, C7_INT_RF_TX) && rf_state);
	hrc7000_air_tx_abort(A); hrc7000_air_poll(A); pending_clears(&pending);
	CHECK(!rf_state && pending == others && lock_depth == 0);
	hrc7000_air_close(A, true);
#if CONFIG_HRC7000_NATIVE_RX
	phy_reset_mode = 0;
#endif
}

// Interrupts and task calls in any order: every sample at most once and in order, a jump only at a receiver restart,
// the RF never keyed outside a transmission, every transmission ending in one TX_DONE, the lock always let go
static void test_fuzz(void)
{
	fuzz_t f;
	fuzz_init(&f, 0xA1C7);
	long iters = fuzz_iters(200);
	int begun = 0;
	bool rf_ok = true, locks_ok = true;

	for (long it = 0; it < iters; it++) {
		setup();
		begun = 0;
		CHECK(hrc7000_air_open(A, &prof, AIR_FIT_RX | AIR_FIT_TX, on_event, NULL));
		for (int step = 0; step < 400; step++) {
			uint32_t op = fuzz_below(&f, 12);
			air_tx_chunk_t c = chunk((int)step, (uint16_t)(1 + fuzz_below(&f, 144)), 0);
			switch (op) {
			case 0: case 1: case 2:
				copy_half();
				break;
			case 3:
				hrc7000_air_poll(A);
				break;
			case 4:
				now += fuzz_below(&f, 40000);
				hrc7000_air_isr_timeslot(A);
				break;
			case 5:
				now += fuzz_below(&f, 2000);
				phy_rdy();
				break;
			case 6:
				hrc7000_air_isr_rftx(A);
				break;
			case 7:
				if (fuzz_below(&f, 8) == 0) {
					c.flags = AIR_TX_LAST;
				}
				hrc7000_air_tx_queue(A, &c);
				break;
			case 8:
				begun += hrc7000_air_tx_begin(A);
				break;
			case 9:
				if (fuzz_below(&f, 4) == 0) {
					hrc7000_air_tx_end(A);
				}
				break;
			case 10:
				if (fuzz_below(&f, 16) == 0) {
					hrc7000_air_tx_abort(A);
				} else if (fuzz_below(&f, 16) == 0) {
					hrc7000_air_rx_restart(A);
				}
				break;
			default:
				now += fuzz_below(&f, 100000);
				break;
			}
			rf_ok &= !rf_state || hrc7000_air_tx_active(A);
			locks_ok &= lock_depth == 0;
		}
		hrc7000_air_tx_abort(A);
		hrc7000_air_poll(A);
		CHECK(continuous(0) && bad_times == 0);
		CHECK(count_ev(AIR_EV_TX_DONE) == begun && count_ev(AIR_EV_TX_STARTED) <= begun);
		CHECK(!rf_state && REG(C7_RF_MOD_BIAS) == DMR_MOD_BIAS);
		hrc7000_air_close(A, true);
		CHECK(dmr_set_up());
		if (failures != 0) {
			printf("  fuzz iteration %ld\n", it);
			break;
		}
	}
	CHECK(rf_ok && locks_ok);
}

// These receive profiles keep the existing sample path: the two NXDN symbol rates have short recurring syncs.
// With the switch off, the P25-shaped profile does too. Their real decoder is covered by airSoftmodemTest.
static void test_rx_fallback_profiles(void)
{
	for (unsigned k = 0; k < 3; k++) {
		setup();
		prof.sym_rate = k == 0 ? 2400 : 4800;
		prof.shape = k == 2 ? AIR_SHAPE_C4FM : AIR_SHAPE_RRC;
		prof.shape_param = 20;
		prof.dev_outer_hz = k == 0 ? 1050 : k == 1 ? 2400 : 1800;
		prof.sync[0].nsyms = k == 2 ? 24 : 10;
		prof.sync_every_syms = k == 2 ? 864 : 0;
#if CONFIG_HRC7000_NATIVE_RX
		if (k == 2) prof.sync[0].flags = AIR_SYNC_START_ONLY; // Not a recurring correlator pattern.
#endif
		memcpy(prof.sync[0].syms, (uint8_t[]){0x55,0x75,0xF5,0xFF,0x77,0xFF}, 6);
		CHECK(!(hrc7000_air_native_rx_fit(&prof) & AIR_FIT_RX_IDLE_REARM));
		CHECK(hrc7000_air_open(A, &prof, AIR_FIT_RX | AIR_FIT_TX | AIR_FIT_SOFT, on_event, NULL));
		CHECK(copy_set_up() && sm_opens == 1);
		copy_half(); hrc7000_air_poll(A);
		CHECK(ngot == HALF && got[0] == 0 && got[HALF-1] == HALF-1 && !bad_times);
		hrc7000_air_close(A, true); CHECK(dmr_set_up());
	}
}

static void test_idle_rearm_fit(void)
{
	setup();
	prof.shape_param = 20;
	prof.sync_every_syms = 864;
	memcpy(prof.sync[0].syms, (uint8_t[]){0x55,0x75,0xF5,0xFF,0x77,0xFF}, 6);
#if CONFIG_HRC7000_NATIVE_RX
	CHECK(hrc7000_air_native_rx_fit(&prof) == (AIR_FIT_RX | AIR_FIT_NEEDS_SYNC | AIR_FIT_RX_IDLE_REARM));
#else
	CHECK(hrc7000_air_native_rx_fit(&prof) == 0);
#endif
}

#if CONFIG_HRC7000_NATIVE_RX
// The published r13 PHY record layout, inverse-mapped by bit spans in this fixture. No soft-modem decisions are
// substituted: the actual driver's ISR copies raw chip RAM, then its poll hands hard chunks to the native framer.
static air_chunk_t native_chunks[64];
static uint8_t native_hard[64][36];
static unsigned native_n, native_isr, native_polls;
static air_framer_t *native_framer;
static uint32_t native_rf, native_sync_regs[10];

static void native_event(const air_event_t *ev, void *ctx)
{
	CHECK(!native_isr && lock_depth == 0);
	if (ev->kind == AIR_EV_RX_POLL) { native_polls++; CHECK(ev->chunk == NULL); return; }
	if (ev->kind != AIR_EV_RX) return;
	const air_chunk_t *c = ev->chunk;
	CHECK(c && c->nsyms == 144 && c->hard && !c->soft);
	if (!c || !c->hard || c->nsyms != 144) return;
	if (native_n < 64) {
		native_chunks[native_n] = *c;
		memcpy(native_hard[native_n], c->hard, 36);
		native_chunks[native_n].hard = native_hard[native_n];
	}
	native_n++;
	if (native_framer) air_framer_rx(native_framer, c);
}

static void native_setup(void)
{
	setup();
	prof = p25_air_mode.profile;
	native_n = native_isr = native_polls = 0;
	phy_reset_mode = 1;
	phy_reset_polls = 0;
	REG(C7_SYS_SOFT_RSTN) = 0xA5A50003u;
	native_framer = NULL;
	native_rf = REG(C7_RF_MODE) = 0xA567B890u;
	for (unsigned i = 0; i < 10; i++)
		native_sync_regs[i] = REG(C7_RECV_MS_SYNC_H + i * 4) = 0x80301000u + i;
}

static bool native_open(void)
{
	return hrc7000_air_open(A, &prof, AIR_FIT_RX | AIR_FIT_TX | AIR_FIT_NEEDS_SYNC | AIR_FIT_RX_IDLE_REARM, native_event, NULL);
}

static void test_native_idle_poll(void)
{
	native_setup();
	CHECK(native_open());
	hrc7000_air_poll(A);
	CHECK(native_polls == 1 && native_n == 0 && A->st.chunks == 0 && lock_depth == 0);
	hrc7000_air_suspend(A);
	hrc7000_air_poll(A);
	CHECK(native_polls == 1);
	hrc7000_air_resume(A);
	hrc7000_air_poll(A);
	CHECK(native_polls == 2 && native_n == 0);
	hrc7000_air_close(A, true);
	hrc7000_air_poll(A);
	CHECK(native_polls == 2);
}

static bool native_set_up(void)
{
	return REG(C7_WORK_MODE) == 0x02 && REG(C7_LAYER2_CONTROL) == 0x40 &&
	       REG(C7_LAYER2_TXRX_CTRL) == 0x41 && !(REG(C7_RF_MODE) & (1u << 11)) &&
	       REG(C7_RECV_MS_SYNC_H) == 0x805575F5u && REG(C7_RECV_MS_SYNC_L) == 0xFF77FFu &&
	       (REG(C7_SYS_INTERP_MASK) & (1u << 11)) && !(REG(C7_AUDIO_CONTROL) & C7_AUDIO_FM_PLAY);
}

static bool native_restored(void)
{
	if (REG(C7_RF_MODE) != native_rf) return false;
	for (unsigned i = 0; i < 10; i++)
		if (REG(C7_RECV_MS_SYNC_H + i * 4) != native_sync_regs[i]) return false;
	return true;
}

static void native_record(const uint8_t air[36], unsigned kind, uint32_t delta)
{
	// Each air bit has exactly one destination in the raw record. The sync layout puts its24-symbol comparator
	// pattern at air symbol66; types0/1 have the plain data layout, type3 is deliberately unsupported.
	static const uint16_t plain[][2] = { {0,24}, {72,108}, {24,48}, {180,108} };
	static const uint16_t sync[][2] = { {0,24}, {92,98}, {24,10}, {44,48}, {34,10}, {190,98} };
	const uint16_t (*runs)[2] = kind == 2 ? sync : plain;
	unsigned nruns = kind == 2 ? 6 : 4, at = 0;
	uint8_t raw[36] = {0};
	for (unsigned k = 0; k < nruns; k++) for (unsigned j = 0; j < runs[k][1]; j++, at++) {
		unsigned to = runs[k][0] + j;
		raw[to / 8] |= (uint8_t)(((air[at / 8] >> (7 - at % 8)) & 1u) << (7 - to % 8));
	}
	memcpy(buf + C7_RX_RAM + 0x30, raw, 36);
	REG(C7_RX_TYPE_INFO) = kind | 0x123400u;
	now += delta;
	REG(C7_SYS_INTERP_LIST) = 1u << 11;
	native_isr++;
	hrc7000_air_isr_sys(A);
	native_isr--;
	REG(C7_SYS_INTERP_LIST) = 0;
}

static void test_native_records(void)
{
	uint8_t air[36];
	for (unsigned i = 0; i < 36; i++) air[i] = (uint8_t)(i * 37 + 19);
	native_setup(); CHECK(native_open()); CHECK(native_set_up() && sm_opens == 0);
	CHECK(REG(C7_INTERP_CLEAR) == (C7_INT_TS_TX | C7_INT_TS_RX | C7_INT_RF_TX));
	CHECK(phy_reset_polls == 3 && REG(C7_SYS_SOFT_RSTN) == 0xA5A50003u);
	for (unsigned k = 0; k < 3; k++) {
		native_record(air, k, 30000);
		CHECK(native_n == k); // Deferred, and RAM must already belong to the record rather than the chip buffer.
		memset(buf + C7_RX_RAM + 0x30, 0xDB, 36);
		hrc7000_air_poll(A);
		CHECK(native_n == k + 1);
		if (native_n > k) {
			CHECK(!memcmp(native_hard[k], air, 36));
			CHECK(native_chunks[k].sync_at == (k == 2 ? 66 : -1));
			CHECK(!!(native_chunks[k].flags & AIR_CHUNK_GAP) == (k == 0));
			if (k) CHECK(native_chunks[k].t_us - native_chunks[k-1].t_us == 30000);
		}
	}
	unsigned stalled = native_n;
	REG(C7_SYS_INTERP_LIST) = 1u << 11; REG(C7_SYS_INTERP_CLEAR) = 0;
	hrc7000_air_poll(A); now += 50001u; hrc7000_air_poll(A);
	CHECK(REG(C7_SYS_INTERP_CLEAR) == (1u << 11) && native_n == stalled && A->st.overruns == 0);
	REG(C7_SYS_INTERP_LIST) = 0;
	native_record(air, 2, 30000); hrc7000_air_poll(A);
	CHECK(native_n == stalled + 1 && (native_chunks[stalled].flags & AIR_CHUNK_GAP));
	CHECK(!memcmp(native_hard[stalled], air, 36) && phy_reset_polls == 3);
	unsigned before = native_n;
	native_record(air, 3, 30000); hrc7000_air_poll(A);
	CHECK(native_n == before);
	native_record(air, 0, 30000); hrc7000_air_poll(A);
	CHECK(native_n == before + 1 && (native_chunks[before].flags & AIR_CHUNK_GAP));
	before = native_n;
	native_record(air, 0, 60000); hrc7000_air_poll(A);
	CHECK(native_n == before + 1 && (native_chunks[before].flags & AIR_CHUNK_GAP));
	// The continuous PHY raises a slot-format warning during valid reception; r13 acknowledges it without a gap.
	REG(C7_SYS_INTERP_LIST) = 1u << 16;
	hrc7000_air_isr_sys(A); REG(C7_SYS_INTERP_LIST) = 0;
	before = native_n;
	native_record(air, 0, 30000); hrc7000_air_poll(A);
	CHECK(native_n == before + 1 && !(native_chunks[before].flags & AIR_CHUNK_GAP));
	CHECK(ngot == 0 && sm_opens == 0);
	hrc7000_air_close(A, true);
	CHECK(dmr_set_up() && native_restored());
}

static void test_native_lifecycle(void)
{
	uint8_t air[36] = {0x19};
	native_setup(); CHECK(native_open());
	// One beyond the bounded eight-record queue is lost; later records explicitly expose the hole.
	for (unsigned i = 0; i < 9; i++) { air[0] = (uint8_t)i; native_record(air, 0, 30000); }
	hrc7000_air_poll(A);
	CHECK(A->st.overruns == 1 && native_n == 8);
	for (unsigned i = 0; i < native_n && i < 8; i++) CHECK(native_hard[i][0] == i);
	native_record(air, 0, 30000); hrc7000_air_poll(A);
	CHECK(native_n == 9 && (native_chunks[8].flags & AIR_CHUNK_GAP));
	native_record(air, 0, 30000);
	hrc7000_air_rx_restart(A); // Discard pre-retune records and release receive synchronization.
	unsigned before = native_n;
	native_record(air, 0, 30000); hrc7000_air_poll(A);
	CHECK(native_n == before + 1 && (native_chunks[before].flags & AIR_CHUNK_GAP));
	air_tx_chunk_t c = chunk(3, 144, AIR_TX_LAST);
	CHECK(hrc7000_air_tx_queue(A, &c) && hrc7000_air_tx_begin(A));
	CHECK(native_restored() && REG(C7_WORK_MODE) == C7_WORK_MODE_PHY);
	before = native_n;
	native_record(air, 0, 30000); hrc7000_air_poll(A);
	CHECK(native_n == before); // RX records do not enter the framer during TX.
	hrc7000_air_tx_abort(A); hrc7000_air_poll(A);
	CHECK(native_set_up() && REG(C7_RF_MOD_BIAS) == DMR_MOD_BIAS);
	CHECK(phy_reset_polls == 6); // One initial open and one task TX begin; no reset in abort/RX re-entry.
	native_record(air, 0, 30000); hrc7000_air_poll(A);
	CHECK(native_n == before + 1 && (native_chunks[before].flags & AIR_CHUNK_GAP));
	hrc7000_air_suspend(A); CHECK(native_restored());
	before = native_n;
	native_record(air, 0, 30000); hrc7000_air_poll(A); CHECK(native_n == before);
	hrc7000_air_resume(A); CHECK(native_set_up() && phy_reset_polls == 6);
	native_record(air, 0, 30000); hrc7000_air_poll(A);
	CHECK(native_n == before + 1 && (native_chunks[before].flags & AIR_CHUNK_GAP));
	hrc7000_air_close(A, true); CHECK(dmr_set_up() && native_restored());
	// The same provider object may next be a sample tap; a closed hardware choice cannot survive that handoff.
	CHECK(hrc7000_air_tap_start(A) && copy_set_up());
	hrc7000_air_suspend(A); hrc7000_air_resume(A);
	CHECK(copy_set_up());
	copy_half();
	int16_t samples[HALF];
	CHECK(hrc7000_air_tap_read(A, samples, HALF) == HALF && samples[0] == 0);
	hrc7000_air_tap_stop(A); CHECK(dmr_set_up());
}

static void test_native_reset_stuck(void)
{
	native_setup(); phy_reset_mode = 2;
	CHECK(!native_open());
	CHECK(phy_reset_polls > 0 && phy_reset_polls <= 1024);
	CHECK(REG(C7_SYS_SOFT_RSTN) == 0xA5A50003u);
	CHECK(!A->open && !rf_state && !sm_opens && lock_depth == 0);
	CHECK(dmr_set_up() && native_restored());
	phy_reset_mode = 0;
}

static void test_native_tx_reset(void)
{
	native_setup(); CHECK(native_open());
	tx_reset_refusal(true);
	uint8_t air[36] = {0x19};
	native_record(air, 2, 30000); hrc7000_air_poll(A);
	CHECK(native_n == 1 && !memcmp(native_hard[0], air, sizeof air));
	hrc7000_air_close(A, true);
	CHECK(native_restored() && dmr_set_up());
}

static unsigned native_units;
static uint8_t native_duid[8], native_unit_air[8][P25_UNIT_BYTES_MAX];
static void native_unit(const p25_rx_unit_t *u, void *ctx)
{
	if (native_units < 8) {
		native_duid[native_units] = u->duid;
		memcpy(native_unit_air[native_units], u->air, (u->nsyms + 3u) / 4u);
	}
	native_units++;
}

// A normal completed transmission, including its idle tail, must re-enter raw RX from the interrupt path. Keep
// this separate from abort: a missing re-entry here makes the next physical P25 call inaudible until a retune.
static void native_completed_tx(void)
{
	air_tx_chunk_t first = chunk(7, 144, 0), last = chunk(8, 144, AIR_TX_LAST);
	uint32_t resets = phy_reset_polls, done = A->st.tx_done, sent = A->st.tx_slots;
	CHECK(hrc7000_air_tx_queue(A, &first) && hrc7000_air_tx_queue(A, &last));
	CHECK(hrc7000_air_tx_begin(A) && native_restored());
	CHECK(REG(C7_WORK_MODE) == C7_WORK_MODE_PHY && REG(C7_LAYER2_CONTROL) == 0x82u);
	native_isr++;
	hrc7000_air_isr_rftx(A);
	for (unsigned i = 0; i < HRC7000_AIR_TX_LEAD_SLOTS + 4u; i++) {
		if (i < HRC7000_AIR_TX_LEAD_SLOTS + 3u) CHECK(rf_state);
		slot();
	}
	native_isr--;
	CHECK(!hrc7000_air_tx_active(A) && !rf_state && native_set_up());
	CHECK(A->st.tx_done == done + 1 && A->st.tx_slots == sent + HRC7000_AIR_TX_LEAD_SLOTS + 3u);
	CHECK(A->st.tx_underruns == 0 && REG(C7_RF_MOD_BIAS) == DMR_MOD_BIAS);
	CHECK(phy_reset_polls == resets + 3); // Only task begin reset: none inside the TX-end interrupt.
	hrc7000_air_poll(A);
}

static void native_direction_tx(void)
{
	air_tx_chunk_t c = chunk(12, 144, AIR_TX_LAST);
	uint32_t stalls = A->st.stalls;
	direction_model = true;
	direction_phase = DIR_SYNCED;
	direction_reset_done = false;
	CHECK(native_set_up() && hrc7000_air_tx_queue(A, &c) && hrc7000_air_tx_begin(A));
	bool ready = direction_phase == DIR_READY;
	slot();
	CHECK(!rf_state); // A boundary alone must never replace the RF TX interrupt's key authority.
	if (ready) hrc7000_air_isr_rftx(A);
	CHECK(ready && rf_state);
	if (!ready) {
		// Match the captured endpoint before cleanup: fresh clock, no RF key, unchanged TX registers.
		for (unsigned i = 0; i < 6; i++) slot();
		now += 75u;
		CHECK(A->tx == HRC7000_AIR_START && !A->pa && A->tick_any && A->lead == 2 && now - A->t_tick == 75u);
		CHECK(REG(C7_WORK_MODE) == 0x22u && REG(C7_LAYER2_CONTROL) == 0x82u && REG(C7_LAYER2_TXRX_CTRL) == 0x80u);
		CHECK(hrc7000_air_poll(A) && !rf_state && A->st.stalls == stalls + 1);
	} else {
		for (unsigned i = 0; i < HRC7000_AIR_TX_LEAD_SLOTS + 3u; i++) slot();
		CHECK(!hrc7000_air_poll(A) && !rf_state && !hrc7000_air_tx_active(A) && A->st.stalls == stalls);
	}
	direction_model = false;
	CHECK(native_set_up() && lock_depth == 0);
}

static void test_native_p25(void)
{
	uint8_t stream[36 * 20] = {0}, unit[P25_UNIT_BYTES_MAX], hdr[P25_HDR_BYTES] = {0};
	uint8_t lcraw[P25_LC_BYTES], es[P25_ES_BYTES] = {0}, voice[2][9][P25_IMBE_BYTES], decoded[16];
	p25_lc_t lc = { .lcf = P25_LCF_GROUP, .tgid = 1, .src = 101 };
	air_framer_t fr; p25_rx_t rx;
	uint8_t fbuf[P25_UNIT_BYTES_MAX]; int8_t fsoft[P25_UNIT_SYMS_MAX];
	unsigned pos = 66; // First sync at the native correlator's documented symbol offset.
	p25_lc_build(&lc, lcraw); hdr[10] = 0x80; es[9] = 0x80;
	for (unsigned k = 0; k < 4; k++) {
		unsigned duid = k == 0 ? P25_DUID_HDU : k == 1 ? P25_DUID_LDU1 : k == 2 ? P25_DUID_LDU2 : P25_DUID_TDU;
		unsigned n = k == 0 ? P25_HDU_SYMS : k == 3 ? P25_TDU_SYMS : P25_LDU_SYMS;
		p25_unit_begin(unit, n, 0x293, duid, P25_SS_UNKNOWN);
		if (k == 0) p25_hdu_put(unit, hdr);
		if (k == 1 || k == 2) {
			for (unsigned v = 0; v < 9; v++) {
				for (unsigned i = 0; i < P25_IMBE_BYTES; i++) voice[k-1][v][i] = (uint8_t)(k*53 + v*13 + i);
				p25_ldu_voice_put(unit, v, voice[k-1][v]);
			}
			if (k == 1) p25_ldu_lc_put(unit, lcraw); else p25_ldu_es_put(unit, es);
		}
		for (unsigned i = 0; i < n; i++) air_sym_put(stream, (uint16_t)pos++, air_sym_get(unit, i));
	}
	native_setup(); CHECK(native_open()); native_units = 0;
	p25_rx_init(&rx, 0x293, native_unit, NULL, NULL);
	air_framer_init(&fr, &p25_air_mode, &rx, fbuf, fsoft, P25_UNIT_SYMS_MAX);
	native_framer = &fr;
	for (unsigned call = 0; call < 2; call++) {
		if (call) {
			now += 2000000u; hrc7000_air_poll(A); // Idle RX, own TX, idle RX: the provider stays open throughout.
			native_completed_tx();
			now += 2000000u; hrc7000_air_poll(A);
		}
		for (unsigned at = 0; at < pos; at += 144) {
			native_record(stream + at/4, at == 0 ? 2 : (at/144)%2, 30000);
			hrc7000_air_poll(A);
		}
		unsigned base = call * 4;
		CHECK(native_units == base + 4);
		if (native_units == base + 4) {
			CHECK(native_duid[base] == P25_DUID_HDU && native_duid[base+1] == P25_DUID_LDU1 &&
			      native_duid[base+2] == P25_DUID_LDU2 && native_duid[base+3] == P25_DUID_TDU);
			CHECK(p25_hdu_get(native_unit_air[base], decoded) == 0 && !memcmp(hdr, decoded, P25_HDR_BYTES));
			CHECK(p25_ldu_lc_get(native_unit_air[base+1], decoded) == 0 && !memcmp(lcraw, decoded, P25_LC_BYTES));
			CHECK(p25_ldu_es_get(native_unit_air[base+2], decoded) == 0 && !memcmp(es, decoded, P25_ES_BYTES));
			for (unsigned k = 0; k < 2; k++) for (unsigned v = 0; v < 9; v++)
				CHECK(p25_ldu_voice_get(native_unit_air[base+k+1], v, decoded) == 0 && !memcmp(voice[k][v], decoded, P25_IMBE_BYTES));
		}
	}
	// Both preceding complete calls decoded HDU/LDU/NID/voice through the actual driver/framer. Exercise the
	// proposed direction-latch contract after that sync history, and after own TX re-entered the raw receiver.
	native_framer = NULL;
	native_direction_tx();
	native_record(stream, 2, 30000); hrc7000_air_poll(A);
	native_direction_tx();
	CHECK(!sm_opens && !ngot);
	hrc7000_air_close(A, true); CHECK(native_restored() && dmr_set_up());
}
#endif

int main(void)
{
	test_pure();
	test_tap();
	test_rx_session();
	test_tx();
	test_underrun_and_end();
	test_abort_watchdog_suspend();
	test_open_close_dmr();
	test_sys_recovery();
	test_tx_pending_requests();
	test_tx_reset();
	test_fuzz();
	test_rx_fallback_profiles();
	test_idle_rearm_fit();
#if CONFIG_HRC7000_NATIVE_RX
	test_native_records();
	test_native_idle_poll();
	test_native_lifecycle();
	test_native_reset_stuck();
	test_native_tx_reset();
	test_native_p25();
#endif
	printf("hrc7000AirTest: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
