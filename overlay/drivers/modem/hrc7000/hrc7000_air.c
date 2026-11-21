// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Copyright (c) 2026 tylerwatt12
// The HR_C7000 as an air interface provider and receive tap (see hrc7000_air.h; docs/spec/modem-hrc7000.md 10)
#include <stddef.h>
#include <string.h>
#include "hrc7000_air.h"

#if CONFIG_AIR	// the whole file

#define WORK_MODE_PHY		C7_WORK_MODE_PHY	// timeslot mode: on air back to back with every slot sent (10.2)
#define L2_PHY_TX_MASTER	0x82u	// txen, tx_master_mode: own TX slot timing
#define L2_PASSIVE		0xC0u
#define TXRX_NONE		0x00u
#define TXRX_SEND		0x80u	// txnextsloten
#define TXRX_SYNC_RELEASE	0xC00000u	// b23, b22: release the receive and transmit slot syncs
#define SYS_PHY_RDY		(1u << 12)
#define SYS_PHY_RX		(1u << 11)
#define SYS_PHY_ABNORMAL		(1u << 16)
#define BW_COPY			0x00u	// FM_BANDWIDTH: band-pass off (b6), 12.5 kHz receive, no pre-emphasis
#define DEV_COEF_NARROW		0x40A25u	// FM_DEV_COEF for 12.5 kHz receive (8.2)
#define RAM_HALF0		0x00u
#define RAM_HALF1		0x24u
#define SYS_LOOPS		4u

#define SLOT_US			30000u
#define SAMPLE_US		125u	// 8 kHz
#define HALF			HRC7000_FM_RX_SAMPLES	// samples per copy half
#define HALF_SYM_US		104u	// half a symbol at 4800 symbols/s: t_us is a symbol's center
#define TX_AIR_DELAY_US		5000u	// a slot set up at a boundary goes on air about 5 ms later (2.10)
#define STALL_US		200000u
#define NO_KEY_US		200000u	// transmitting this long without an RF TX interrupt: dropped
#define SYS_STUCK_US		50000u	// the shared PIC needs a stuck request to fall before it can rise again

_Static_assert((HRC7000_AIR_RING % HALF) == 0 && HRC7000_AIR_RING / HALF == 2, "the ring is two copy halves");

// The interrupt path runs from RAM on the SoC: flash busy time would add jitter to the 5 ms a slot boundary leaves for
// the next half (modem-hrc7000.md 9 #15). Host builds have no such section
#if defined(__csky__)
#define AIR_ISR		__attribute__((section(".ramfunc"), noinline))
#else
#define AIR_ISR
#endif

static inline void w(hrc7000_air_t *a, uint32_t off, uint32_t v) { hrc7000_wr(a->dev, off, v); }
static inline uint32_t r(hrc7000_air_t *a, uint32_t off) { return hrc7000_rd(a->dev, off); }
static inline uint32_t now(const hrc7000_air_t *a) { return a->ops->now_us(a->ops->ctx); }
static void lock(hrc7000_air_t *a) { if (a->ops->lock != NULL) a->ops->lock(a->ops->ctx); }
static void unlock(hrc7000_air_t *a) { if (a->ops->unlock != NULL) a->ops->unlock(a->ops->ctx); }
static AIR_ISR void notify(hrc7000_air_t *a) { if (a->ops->notify != NULL) a->ops->notify(a->ops->ctx); }

// --- Pure ---

#if CONFIG_HRC7000_NATIVE_RX
// Select only the receive profile checked by the released hardware-record path. A different waveform continues
// through the sample copy; matching a correlator pattern alone does not establish the chip's receive bandwidth.
static int native_sync(const air_profile_t *p)
{
	if (p == NULL || p->sym_rate != 4800u || p->levels != 4 || p->timing != AIR_TIMING_CONTINUOUS
	    || p->shape != AIR_SHAPE_C4FM || p->shape_param != 20 || p->dev_outer_hz != 1800u
	    || p->sync_every_syms == 0 || p->sync_every_syms > 864u || p->nsyncs > AIR_SYNCS_MAX) {
		return -1;
	}
	for (uint8_t i = 0; i < p->nsyncs; i++) {
		const air_sync_t *s = &p->sync[i];
		if (s->nsyms != AIR_SYNC_SYMS_MAX || (s->flags & AIR_SYNC_START_ONLY) != 0) {
			continue;
		}
		bool outer = true;
		for (uint8_t j = 0; j < s->nsyms; j++) {
			if ((s->syms[j / 4] & (1u << (6u - 2u * (j % 4)))) == 0) {
				outer = false;
			}
		}
		if (outer) {
			return i;
		}
	}
	return -1;
}
#endif

uint32_t hrc7000_air_native_rx_fit(const air_profile_t *p)
{
#if CONFIG_HRC7000_NATIVE_RX
	return native_sync(p) >= 0 ? AIR_FIT_RX | AIR_FIT_NEEDS_SYNC | AIR_FIT_RX_IDLE_REARM : 0;
#else
	(void)p;
	return 0;
#endif
}

uint32_t hrc7000_air_tx_fit(const air_profile_t *p)
{
	if (p->timing != AIR_TIMING_CONTINUOUS || (p->levels != 2 && p->levels != 4)) {
		return 0;	// the chip keys slot by slot only as the DMR engine; its own slot clock is not the profile's
	}
	uint32_t f = AIR_FIT_TX;
	if (p->sym_rate == 2400u) {
		f |= AIR_FIT_RATE_DOUBLED | AIR_FIT_SHAPE_APPROX;	// the pulse filter is the 4800 one
	} else if (p->sym_rate != 4800u) {
		return 0;
	}
	if (p->shape != AIR_SHAPE_RRC || p->shape_param != 20u) {
		f |= AIR_FIT_SHAPE_APPROX;	// the chip's own DMR pulse: root raised cosine, roll-off 0.2
	}
	// The modulation bytes scale from a byte calibrated near 0x4A at 1944 Hz: 1 to 255 covers about 26 Hz to 6.7 kHz
	if (p->dev_outer_hz < 100u || p->dev_outer_hz > 6000u) {
		f |= AIR_FIT_DEV_APPROX;
	}
	return f;
}

static uint32_t scale_byte(uint32_t b, uint16_t dev)
{
	uint32_t v = (b * dev + HRC7000_AIR_DMR_DEV_HZ / 2u) / HRC7000_AIR_DMR_DEV_HZ;
	return v < 1u ? 1u : v > 255u ? 255u : v;
}

uint32_t hrc7000_air_mod_bias(const air_profile_t *p, uint32_t dmr_mod_bias)
{
	uint32_t mod2 = scale_byte((dmr_mod_bias >> 8) & 0xFFu, p->dev_outer_hz);
	uint32_t mod1 = scale_byte(dmr_mod_bias & 0xFFu, p->dev_outer_hz);
	return (dmr_mod_bias & 0xFFFF0000u) | mod2 << 8 | mod1;
}

// --- Queues (free-running indexes: a power of two) ---

// TX_SPACE events in a row are one (the count is tx_space's), and the last place is kept for TX_DONE: the core's TX
// off waits on it
static AIR_ISR void ev_push(hrc7000_air_t *a, uint8_t kind, uint32_t t)
{
	uint8_t used = (uint8_t)(a->ev_head - a->ev_tail);
	if (kind == AIR_EV_TX_SPACE && used > 0 && a->evq[(uint8_t)(a->ev_head - 1u) % HRC7000_AIR_EVQ].kind == kind) {
		return;
	}
	if (used >= HRC7000_AIR_EVQ - (kind == AIR_EV_TX_DONE ? 0u : 1u)) {
		a->st.ev_overflow++;
		return;
	}
	hrc7000_air_ev_t *e = &a->evq[a->ev_head % HRC7000_AIR_EVQ];
	e->kind = kind;
	e->t_us = t;
	__atomic_thread_fence(__ATOMIC_RELEASE);
	a->ev_head++;
}

static uint8_t txq_count(const hrc7000_air_t *a)
{
	return (uint8_t)(a->txq_head - a->txq_tail);
}

static bool last_queued(const hrc7000_air_t *a)
{
	return txq_count(a) > 0 && (a->buf->txq[(uint8_t)(a->txq_head - 1u) % HRC7000_AIR_TXQ].flags & AIR_TX_LAST) != 0;
}

// --- The receive copy (10.1) ---

// A request left high cannot give this PIC another rising edge. At an ownership transition discard old modem
// requests before enabling new timing; RF RX and the PCM pair belong to other drivers (normal engine, 3.3).
static AIR_ISR void clear_requests(hrc7000_air_t *a)
{
	w(a, C7_INTERP_CLEAR, C7_INT_TS_TX | C7_INT_TS_RX | C7_INT_RF_TX);
	w(a, C7_SYS_INTERP_CLEAR, r(a, C7_SYS_INTERP_LIST));
	a->sys_waiting = false;
}

// The time of sample i: the halves are whole in the ring, each stamped when its interrupt came
static uint32_t sample_time(const hrc7000_air_t *a, uint32_t i)
{
	return a->t_half[(i / HALF) % 2u] + (i % HALF) * SAMPLE_US;
}

// The chip from DMR to the copy set-up: the FM receive path on the RF chip's DMR output, band-pass and playback off
static AIR_ISR void copy_on(hrc7000_air_t *a)
{
	a->sys_waiting = false;
	if (!a->chip_saved) {
		a->saved_mask = r(a, C7_SYS_INTERP_MASK);
		a->saved_bw = r(a, C7_FM_BANDWIDTH);
		a->saved_coef = r(a, C7_FM_DEV_COEF);
		a->chip_saved = true;
	}
	w(a, C7_LAYER2_TXRX_CTRL, TXRX_NONE);
	w(a, C7_LAYER2_SLOTON, 0);
	w(a, C7_LAYER2_CONTROL, L2_PASSIVE);
	w(a, C7_FM_PTT, 0);
	// Rule 1 (hal_air.h): the copy goes to the codec DAC too while FM playback is on. Off before the mode changes
	hrc7000_audio_bits(a->dev, 0, C7_AUDIO_FM_PLAY);
	w(a, C7_FM_BANDWIDTH, BW_COPY);
	w(a, C7_FM_DEV_COEF, DEV_COEF_NARROW);
	w(a, C7_WORK_MODE, C7_WORK_MODE_FM);
	w(a, C7_SYS_INTERP_CLEAR, C7_SYS_FM_RX | SYS_PHY_RDY);
	w(a, C7_SYS_INTERP_MASK, C7_SYS_FM_RX | SYS_PHY_RDY);
	a->gap = true;		// the next half does not follow what the ring holds
	__atomic_thread_fence(__ATOMIC_RELEASE);
	a->copy = true;
}

// The interrupt mask as it was before the session. Only power-on sets it otherwise, so it goes back whatever mode the
// chip is in now; the FM bits only in DMR (an FM entry writes its own)
static void restore(hrc7000_air_t *a, bool fm_bits)
{
	if (a->chip_saved) {
		w(a, C7_SYS_INTERP_MASK, a->saved_mask);
		if (fm_bits) {
			w(a, C7_FM_BANDWIDTH, a->saved_bw);
			w(a, C7_FM_DEV_COEF, a->saved_coef);
		}
		a->chip_saved = false;
	}
}

// Back to the DMR receive set-up the slot engine starts from (it restarts its receiver after)
static void copy_off(hrc7000_air_t *a)
{
	a->copy = false;
	restore(a, true);
	w(a, C7_SYS_INTERP_CLEAR, C7_SYS_FM_RX | SYS_PHY_RDY);
	w(a, C7_LAYER2_TXRX_CTRL, TXRX_NONE);
	w(a, C7_WORK_MODE, C7_WORK_MODE_FM);	// through FM, as every DMR entry: resets the state machine (5.1)
	w(a, C7_WORK_MODE, C7_WORK_MODE_DMR);
	w(a, C7_LAYER2_CONTROL, L2_PASSIVE);
	w(a, C7_LAYER2_TXRX_CTRL, TXRX_SYNC_RELEASE);
	w(a, C7_LAYER2_TXRX_CTRL, TXRX_NONE);
}

// Task-level PHY entry (initial raw RX and every TX): the released path resets this sub-block before changing
// direction. Bound both elapsed time and reads; on failure restore its entry configuration. Never called by an ISR.
static bool phy_reset(hrc7000_air_t *a)
{
	uint32_t mode = r(a, C7_WORK_MODE), ctl = r(a, C7_LAYER2_CONTROL);
	uint32_t slots = r(a, C7_LAYER2_SLOTON), txrx = r(a, C7_LAYER2_TXRX_CTRL);
	uint32_t reset = r(a, C7_SYS_SOFT_RSTN);
	w(a, C7_LAYER2_TXRX_CTRL, TXRX_NONE);
	w(a, C7_LAYER2_SLOTON, 0);
	w(a, C7_LAYER2_CONTROL, 0);
	w(a, C7_WORK_MODE, C7_WORK_MODE_FM);
	uint32_t t0 = now(a);
	w(a, C7_SYS_SOFT_RSTN, reset & ~2u);
	for (uint16_t i = 0; i < 1024u; i++) {
		if ((r(a, C7_SYS_SOFT_RSTN) & 2u) != 0) {
			return true;
		}
		if ((uint32_t)(now(a) - t0) >= 100u) {
			break;
		}
	}
	w(a, C7_SYS_SOFT_RSTN, reset);
	w(a, C7_LAYER2_CONTROL, ctl);
	w(a, C7_LAYER2_SLOTON, slots);
	w(a, C7_LAYER2_TXRX_CTRL, txrx & ~TXRX_SEND);
	w(a, C7_WORK_MODE, mode);
	return false;
}

#if CONFIG_HRC7000_NATIVE_RX
static const uint16_t NATIVE_SYNC_REGS[10] = {
	C7_RECV_MS_SYNC_H, C7_RECV_MS_SYNC_L, C7_RECV_BS_SYNC_H, C7_RECV_BS_SYNC_L,
	C7_RECV_TDMA1_SYNC_H, C7_RECV_TDMA1_SYNC_L, C7_RECV_TDMA2_SYNC_H, C7_RECV_TDMA2_SYNC_L,
	C7_RECV_RC_SYNC_H, C7_RECV_RC_SYNC_L,
};

// The released continuous hardware receiver's register sequence. Keep the board's RF calibration and filters;
// only the second DMR receive channel and sync comparators belong to this receiver, and are restored before TX.
static AIR_ISR void native_on(hrc7000_air_t *a)
{
	a->sys_waiting = false;
	if (!a->chip_saved) {
		a->saved_mask = r(a, C7_SYS_INTERP_MASK);
		a->saved_bw = r(a, C7_FM_BANDWIDTH);
		a->saved_coef = r(a, C7_FM_DEV_COEF);
		a->chip_saved = true;
	}
	if (!a->native_saved) {
		a->saved_rf_mode = r(a, C7_RF_MODE);
		for (uint8_t i = 0; i < 10; i++) {
			a->saved_sync[i] = r(a, NATIVE_SYNC_REGS[i]);
		}
		a->native_saved = true;
	}
	w(a, C7_LAYER2_TXRX_CTRL, TXRX_NONE);
	w(a, C7_LAYER2_SLOTON, 0);
	w(a, C7_WORK_MODE, C7_WORK_MODE_FM);
	w(a, C7_WORK_MODE, 0x02u);
	w(a, C7_RF_MODE, a->saved_rf_mode & ~(1u << 11));
	const uint8_t *s = a->p->sync[a->native_sync].syms;
	uint32_t hi = (uint32_t)s[0] << 16 | (uint32_t)s[1] << 8 | s[2];
	uint32_t lo = (uint32_t)s[3] << 16 | (uint32_t)s[4] << 8 | s[5];
	w(a, C7_RECV_MS_SYNC_H, 0x80000000u | hi);
	w(a, C7_RECV_MS_SYNC_L, lo);
	w(a, C7_RECV_BS_SYNC_H, hi);
	w(a, C7_RECV_BS_SYNC_L, lo);
	for (uint8_t i = 4; i < 10; i += 2) {
		w(a, NATIVE_SYNC_REGS[i], a->saved_sync[i] & ~0x80000000u);
	}
	hrc7000_audio_bits(a->dev, 0, C7_AUDIO_FM_PLAY);
	// Drop old slot/RF-TX requests at this RX entry, as the normal engine does. RF RX and PCM have other owners.
	w(a, C7_INTERP_CLEAR, C7_INT_TS_TX | C7_INT_TS_RX | C7_INT_RF_TX);
	w(a, C7_SYS_INTERP_CLEAR, SYS_PHY_RX | SYS_PHY_RDY | SYS_PHY_ABNORMAL);
	w(a, C7_SYS_INTERP_MASK, SYS_PHY_RX | SYS_PHY_RDY | SYS_PHY_ABNORMAL);
	w(a, C7_LAYER2_CONTROL, 0x40u);
	w(a, C7_LAYER2_TXRX_CTRL, 1u << 23);
	w(a, C7_LAYER2_TXRX_CTRL, TXRX_NONE);
	a->tail = a->head;
	a->native_any = false;
	a->gap = true;
	__atomic_thread_fence(__ATOMIC_RELEASE);
	a->native_on = true;
	w(a, C7_LAYER2_TXRX_CTRL, 0x41u);
}

// Only task-level TX begin, suspend and close release this register ownership; the interrupt only re-enters RX.
static void native_off(hrc7000_air_t *a, bool chip)
{
	a->native_on = false;
	if (a->native_saved && chip) {
		w(a, C7_LAYER2_TXRX_CTRL, TXRX_NONE);
		w(a, C7_RF_MODE, a->saved_rf_mode);
		for (uint8_t i = 0; i < 10; i++) {
			w(a, NATIVE_SYNC_REGS[i], a->saved_sync[i]);
		}
		w(a, C7_SYS_INTERP_MASK, a->saved_mask);
	}
	a->native_saved = false;
}

static AIR_ISR void take_record(hrc7000_air_t *a, uint32_t t)
{
	uint8_t kind = (uint8_t)(r(a, C7_RX_TYPE_INFO) & 3u);
	a->st.records++;
	if (kind == 2) {
		a->st.synced++;
	}
	if (a->native_any && (t - a->native_last < SLOT_US / 2u || t - a->native_last > SLOT_US * 3u / 2u)) {
		a->gap = true;
	}
	a->native_last = t;
	a->native_any = true;
	if (kind == 3 || a->head - a->tail >= HRC7000_AIR_RXQ) {
		if (kind != 3) {
			a->st.overruns++;
		}
		a->gap = true;
		return;
	}
	hrc7000_air_record_t *q = &a->buf->records[a->head % HRC7000_AIR_RXQ];
	q->t_us = t - SLOT_US + HALF_SYM_US;
	q->kind = kind;
	q->gap = a->gap;
	for (uint8_t i = 0; i < HRC7000_AIR_HALF_BYTES; i++) {
		q->raw[i] = a->dev->buf[C7_RX_RAM + 0x30u + i];
	}
	a->gap = false;
	__atomic_thread_fence(__ATOMIC_RELEASE);
	a->head++;
}
#endif

static AIR_ISR void rx_on(hrc7000_air_t *a)
{
#if CONFIG_HRC7000_NATIVE_RX
	if (a->native_rx) {
		native_on(a);
		return;
	}
#endif
	copy_on(a);
}

// A half is full (system interrupt bit 14): into the ring, or dropped when the task has not made room
static AIR_ISR void take_half(hrc7000_air_t *a, uint32_t t)
{
	uint32_t h = a->head;
	if (h - a->tail > HRC7000_AIR_RING - HALF) {
		a->st.overruns++;
		a->gap = true;		// the samples after the ring's are not the next ones
		return;
	}
	if (hrc7000_fm_rx_audio(a->dev, &a->buf->ring[h % HRC7000_AIR_RING]) == 0) {
		a->st.no_half++;
		return;
	}
	a->t_half[(h / HALF) % 2u] = t - (HALF - 1u) * SAMPLE_US;
	a->gap_half[(h / HALF) % 2u] = a->gap;	// the task's receiver starts over at this half
	a->gap = false;
	__atomic_thread_fence(__ATOMIC_RELEASE);
	a->head = h + HALF;
	a->st.halves++;
}

// --- Transmit (10.2) ---

static inline volatile uint8_t *half_ram(hrc7000_air_t *a, uint8_t h)
{
	return &a->dev->buf[C7_TX_RAM + (h == 0 ? RAM_HALF0 : RAM_HALF1)];
}

// The last byte a boundary held back, written at the next one: the chip reads a half's last byte after the boundary
// that rewrites the half, so written with the rest it went out with the chunk two slots later (10.2)
static AIR_ISR void last_byte_out(hrc7000_air_t *a)
{
	if (a->last_due) {
		half_ram(a, a->last_half)[HRC7000_AIR_HALF_BYTES - 1] = a->last_byte;
		a->last_due = false;
	}
}

// The TX RAM half the next slot reads: the chunk, idle after its last symbol; NULL = all idle. late: the last byte
// waits for the next boundary (last_byte_out)
static AIR_ISR void fill_half(hrc7000_air_t *a, const air_tx_chunk_t *c, bool late)
{
	volatile uint8_t *ram = half_ram(a, a->half);
	uint16_t n = c == NULL ? 0 : c->nsyms > HRC7000_AIR_SYMS ? HRC7000_AIR_SYMS : c->nsyms;
	uint8_t idle = a->p->idle;

	for (uint16_t i = 0; i < HRC7000_AIR_HALF_BYTES; i++) {
		uint8_t b = idle;
		if (4u * i + 4u <= n) {
			b = c->syms[i];
		} else if (4u * i < n) {
			// The chunk ends inside this byte: idle after its last symbol
			uint8_t keep = (uint8_t)(0xFFu << (8u - 2u * (n - 4u * i)));
			b = (uint8_t)((c->syms[i] & keep) | (idle & (uint8_t)~keep));
		}
		if (late && i == HRC7000_AIR_HALF_BYTES - 1) {
			a->last_byte = b;
			a->last_half = a->half;
			a->last_due = true;
		} else {
			ram[i] = b;
		}
	}
	a->half ^= 1u;		// only the half the next slot reads: writing both corrupted the end of the slot on air
}

static AIR_ISR void write_half(hrc7000_air_t *a, const air_tx_chunk_t *c)
{
	fill_half(a, c, true);
	w(a, C7_LAYER2_TXRX_CTRL, TXRX_SEND);
	a->st.tx_slots++;
}

// The transmission ends now: PA off, the queue dropped, the modulation bytes back, receiving (chip: the registers may
// be written)
static AIR_ISR void tx_stop(hrc7000_air_t *a, uint32_t t, bool chip)
{
	if (chip) {
		w(a, C7_LAYER2_TXRX_CTRL, TXRX_NONE);
	}
	if (a->pa) {
		a->ops->rf(a->ops->ctx, false);
		a->pa = false;
	}
	a->txq_tail = a->txq_head;
	a->end_req = false;
	a->last_due = false;
	if (chip) {
		clear_requests(a);
		if (a->mod_saved) {
			w(a, C7_RF_MOD_BIAS, a->mod_bias);
		}
		if ((a->fit & AIR_FIT_RX) != 0 && !a->suspended) {
			rx_on(a);	// receiving again: the next input record starts over at a gap
		} else {
			w(a, C7_LAYER2_CONTROL, L2_PASSIVE);
			w(a, C7_WORK_MODE, C7_WORK_MODE_FM);
		}
	}
	a->mod_saved = false;
	a->tx = HRC7000_AIR_RX;
	a->st.tx_done++;
	ev_push(a, AIR_EV_TX_DONE, t);
}

// A slot boundary: set up the slot that goes on air about 5 ms from now
static AIR_ISR void boundary(hrc7000_air_t *a, uint32_t t)
{
	if (a->tick_any && t - a->t_tick < SLOT_US / 2u) {
		return;		// the second interrupt of the same slot
	}
	a->tick_any = true;
	a->t_tick = t;
	last_byte_out(a);	// first thing: its half was set up at the boundary before
	if (a->tx == HRC7000_AIR_START && (!a->pa || a->lead > 0)) {
		// The RF chain is not modulating yet: idle until it is (the queue waits, no underrun)
		if (a->pa) {
			a->lead--;
		}
		write_half(a, NULL);
		return;
	}
	switch (a->tx) {
	case HRC7000_AIR_TAIL:
		tx_stop(a, t, true);	// the idle slot after the last chunk is on air: the PA ramps down in it
		return;
	case HRC7000_AIR_LAST:
		write_half(a, NULL);
		a->tx = HRC7000_AIR_TAIL;
		return;
	default:
		break;
	}
	if (txq_count(a) > 0) {
		air_tx_chunk_t *c = &a->buf->txq[a->txq_tail % HRC7000_AIR_TXQ];
		bool last = (c->flags & AIR_TX_LAST) != 0;
		write_half(a, c);
		__atomic_thread_fence(__ATOMIC_RELEASE);
		a->txq_tail++;
		a->underrun = false;
		if (a->tx == HRC7000_AIR_START) {
			ev_push(a, AIR_EV_TX_STARTED, t + TX_AIR_DELAY_US + HALF_SYM_US);
		}
		a->tx = last ? HRC7000_AIR_LAST : HRC7000_AIR_RUN;
		ev_push(a, AIR_EV_TX_SPACE, t);
	} else if (a->end_req) {
		write_half(a, NULL);	// the last chunk is on air in the slot now starting
		a->tx = HRC7000_AIR_TAIL;
	} else {
		write_half(a, NULL);
		a->st.tx_underruns++;
		if (!a->underrun) {
			a->underrun = true;
			ev_push(a, AIR_EV_TX_UNDERRUN, t + TX_AIR_DELAY_US);
		}
	}
}

// --- Interrupts ---

AIR_ISR void hrc7000_air_isr_sys(hrc7000_air_t *a)
{
	a->st.sys_ints++;
	uint32_t t = now(a), seen = 0;
	bool any = false;

	for (uint32_t n = 0; n < SYS_LOOPS; n++) {
		uint32_t list = r(a, C7_SYS_INTERP_LIST);
		if (list == 0) {
			break;
		}
		// Each event once per entry: a half takes 32 ms to fill and a slot 30 ms, so a bit set again within one entry
		// is the same event, read before its clear took
		uint32_t fresh = list & ~seen;
		seen |= list;
#if CONFIG_HRC7000_NATIVE_RX
		if (a->native_on) {
			// A continuous raw stream is not a DMR slot: its slot-format warning is not lost symbols.
			if ((fresh & SYS_PHY_RX) != 0) {
				take_record(a, t);	// copy the record before acknowledging its ready flag
				any = true;
			}
		}
#endif
		w(a, C7_SYS_INTERP_CLEAR, list);	// else the line stays up and nothing later interrupts
		if ((fresh & C7_SYS_FM_RX) != 0 && a->copy) {
			take_half(a, t);
			any = true;
		}
		if ((fresh & SYS_PHY_RDY) != 0 && a->tx != HRC7000_AIR_RX) {
			boundary(a, t);
			any = true;
		}
	}
	if (any) {
		notify(a);
	}
}

AIR_ISR void hrc7000_air_isr_timeslot(hrc7000_air_t *a)
{
	w(a, C7_INTERP_CLEAR, C7_INT_TS_TX | C7_INT_TS_RX);
	if (a->tx != HRC7000_AIR_RX) {
		boundary(a, now(a));
		notify(a);
	}
}

AIR_ISR void hrc7000_air_isr_rftx(hrc7000_air_t *a)
{
	w(a, C7_INTERP_CLEAR, C7_INT_RF_TX);
	// Once per key-up, the RF TX lead before the first slot sent: the board's TX on runs from it, then held to the end
	if (a->tx != HRC7000_AIR_RX && !a->pa) {
		a->ops->rf(a->ops->ctx, true);
		a->ops->pa_arm(a->ops->ctx);
		a->pa = true;
	}
}

// --- Task side ---

void hrc7000_air_init(hrc7000_air_t *a, hrc7000_t *dev, const hrc7000_dmr_ops_t *ops, hrc7000_air_buf_t *buf,
		      air_softmodem_t *sm)
{
	memset(a, 0, sizeof(*a));
	a->dev = dev;
	a->ops = ops;
	a->buf = buf;
	a->sm = sm;
}

bool hrc7000_air_tap_start(hrc7000_air_t *a)
{
	if (a->open || a->tap) {
		return false;
	}
	lock(a);
	a->tap = true;
	a->head = a->tail = 0;
	memset(&a->st, 0, sizeof(a->st));
	copy_on(a);
	unlock(a);
	return true;
}

uint16_t hrc7000_air_tap_read(hrc7000_air_t *a, int16_t *buf, uint16_t max)
{
	if (!a->tap) {
		return 0;
	}
	uint32_t t = a->tail, n = a->head - t;
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	if (n > max) {
		n = max;
	}
	for (uint32_t i = 0; i < n; i++) {
		buf[i] = a->buf->ring[(t + i) % HRC7000_AIR_RING];
	}
	__atomic_thread_fence(__ATOMIC_RELEASE);
	a->tail = t + n;
	return (uint16_t)n;
}

uint32_t hrc7000_air_tap_time(const hrc7000_air_t *a)
{
	return sample_time(a, a->tail);
}

void hrc7000_air_tap_stop(hrc7000_air_t *a)
{
	lock(a);
	if (a->tap) {
		if (!a->suspended) {
			copy_off(a);
		} else {
			restore(a, false);
		}
		a->tap = false;
		a->suspended = false;
	}
	unlock(a);
}

// The soft modem's chunks to the session's callback (task level, inside hrc7000_air_poll)
static void sm_out(const air_chunk_t *c, void *ctx)
{
	hrc7000_air_t *a = ctx;
	air_event_cb_t cb = a->cb;

	a->st.chunks++;
	if (cb != NULL) {
		air_event_t ev = { .kind = AIR_EV_RX, .t_us = c->t_us, .arg = 0, .chunk = c };
		cb(&ev, a->cb_ctx);
	}
}

bool hrc7000_air_open(hrc7000_air_t *a, const air_profile_t *p, uint32_t fit, air_event_cb_t cb, void *ctx)
{
	if (fit == 0 || cb == NULL || a->open || a->tap) {
		return false;
	}
#if CONFIG_HRC7000_NATIVE_RX
	int sync = native_sync(p);
	bool native = (fit & AIR_FIT_RX) != 0 && sync >= 0;
#else
	const bool native = false;
#endif
	if (!native && (fit & AIR_FIT_RX) != 0
	    && !air_softmodem_rx_open(a->sm, p, HRC7000_AIR_RATE_HZ, 0, sm_out, a)) {
		return false;
	}
	lock(a);
#if CONFIG_HRC7000_NATIVE_RX
	if (native && !phy_reset(a)) {
		unlock(a);
		return false;
	}
#endif
	a->p = p;
	a->cb = cb;
	a->cb_ctx = ctx;
	a->fit = fit;
#if CONFIG_HRC7000_NATIVE_RX
	a->native_rx = native;
	a->native_sync = (uint8_t)(sync < 0 ? 0 : sync);
#endif
	a->ev_head = a->ev_tail = 0;
	a->txq_head = a->txq_tail = 0;
	a->tx = HRC7000_AIR_RX;
	a->pa = false;
	a->end_req = false;
	a->mod_saved = false;
	a->suspended = false;
	a->head = a->tail = 0;
	memset(&a->st, 0, sizeof(a->st));
	if ((fit & AIR_FIT_RX) != 0) {
		rx_on(a);
	} else {
		hrc7000_audio_bits(a->dev, 0, C7_AUDIO_FM_PLAY);	// rule 1 without the copy too
	}
	a->open = true;
	unlock(a);
	return true;
}

void hrc7000_air_close(hrc7000_air_t *a, bool chip)
{
	lock(a);
	if (a->open) {
		if (a->tx != HRC7000_AIR_RX) {
			bool mod = a->mod_saved;
			tx_stop(a, now(a), false);	// its TX_DONE goes nowhere: the session ends
			if (chip) {
				w(a, C7_LAYER2_TXRX_CTRL, TXRX_NONE);
				if (mod) {
					w(a, C7_RF_MOD_BIAS, a->mod_bias);
				}
			}
		}
		if (chip && !a->suspended) {
#if CONFIG_HRC7000_NATIVE_RX
			native_off(a, true);
#endif
			copy_off(a);
		} else {
			a->copy = false;
#if CONFIG_HRC7000_NATIVE_RX
			native_off(a, false);
#endif
			restore(a, false);
		}
		a->open = false;
		a->sys_waiting = false;
#if CONFIG_HRC7000_NATIVE_RX
		a->native_rx = false;
#endif
		a->cb = NULL;
		a->ev_tail = a->ev_head;
	}
	unlock(a);
}

void hrc7000_air_rx_restart(hrc7000_air_t *a)
{
	lock(a);
	if (a->open && !a->suspended && a->tx == HRC7000_AIR_RX) {
		a->sys_waiting = false;
#if CONFIG_HRC7000_NATIVE_RX
		if (a->native_rx) {
			native_on(a);
		} else
#endif
		if (a->copy) {
			a->tail = a->head;	// what the ring holds is from the old channel
			a->gap = true;
		}
	}
	unlock(a);
}

void hrc7000_air_suspend(hrc7000_air_t *a)
{
	lock(a);
	if (a->open || a->tap) {
		if (a->tx != HRC7000_AIR_RX) {
			tx_stop(a, now(a), false);
		}
		a->copy = false;	// the saved registers stay: they are put back at the end
		a->sys_waiting = false;
#if CONFIG_HRC7000_NATIVE_RX
		native_off(a, true);
#endif
		a->suspended = true;
	}
	unlock(a);
	notify(a);
}

void hrc7000_air_resume(hrc7000_air_t *a)
{
	lock(a);
	if ((a->open || a->tap) && a->suspended) {
		a->suspended = false;
		if (a->tap || (a->fit & AIR_FIT_RX) != 0) {
			rx_on(a);
		}
	}
	unlock(a);
}

// Samples the interrupt put in the ring, through the soft modem, a copy half at a time (each has its own time stamp
// and gap flag). Head, tail and a restart all move by whole halves, so each pass is one
static void rx_samples(hrc7000_air_t *a)
{
	for (;;) {
		uint32_t h = a->head, t = a->tail;
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		if (t == h || !a->open) {
			return;
		}
		if (a->gap_half[(t / HALF) % 2u]) {
			air_softmodem_rx_reset(a->sm);	// lost samples or a new set-up before this one: timing starts over
		}
		uint32_t t0 = now(a);
		air_softmodem_rx(a->sm, &a->buf->ring[t % HRC7000_AIR_RING], HALF, sample_time(a, t));
		a->st.modem_us += now(a) - t0;
		a->st.modem_span_us += HALF * SAMPLE_US;
		__atomic_thread_fence(__ATOMIC_RELEASE);
		a->tail = t + HALF;
	}
}

#if CONFIG_HRC7000_NATIVE_RX
// Hard dibits use the same chip record permutation as the released raw 4FSK receiver, including all 288 bits.
// With a detected sync the 24-symbol comparator pattern starts at air symbol 66. No synthetic padding is inserted.
static void rx_records(hrc7000_air_t *a)
{
	static const uint8_t plain[][2] = { { 0, 12 }, { 36, 54 }, { 12, 24 }, { 90, 54 } };
	static const uint8_t synced[][2] = { { 0, 12 }, { 46, 49 }, { 12, 5 }, { 22, 24 }, { 17, 5 }, { 95, 49 } };
	for (;;) {
		lock(a);
		uint32_t t = a->tail;
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		if (!a->open || !a->native_on || t == a->head) {
			unlock(a);
			return;
		}
		hrc7000_air_record_t q = a->buf->records[t % HRC7000_AIR_RXQ];
		a->tail = t + 1u;	// the local copy owns this record before the ISR may reuse its slot
		unlock(a);
		uint8_t hard[HRC7000_AIR_HALF_BYTES] = { 0 };
		const uint8_t (*runs)[2] = q.kind == 2 ? synced : plain;
		uint8_t nruns = q.kind == 2 ? 6 : 4;
		uint16_t out = 0;
		for (uint8_t k = 0; k < nruns; k++) {
			for (uint8_t i = runs[k][0]; i < runs[k][0] + runs[k][1]; i++) {
				uint8_t d = (uint8_t)((q.raw[i / 4] >> (6 - 2 * (i % 4))) & 3u);
				hard[out / 4] |= (uint8_t)(d << (6 - 2 * (out % 4)));
				out++;
			}
		}
		air_chunk_t c = { .t_us = q.t_us, .nsyms = HRC7000_AIR_SYMS,
			.sync_at = q.kind == 2 ? 66 : -1, .sync_id = a->p->sync[a->native_sync].id,
			.flags = q.gap ? AIR_CHUNK_GAP : 0, .hard = hard, .soft = NULL };
		sm_out(&c, a);	// the same deferred AIR_EV_RX interface, without the software demodulator
	}
}
#endif

bool hrc7000_air_poll(hrc7000_air_t *a)
{
	bool recovered = false;
	lock(a);
	// The normal engine is halted while this owner holds the chip, so its lost-request guard cannot run. Measure
	// from the first pending observation, not the previous poll: an IRQ arriving during a late poll is still fresh.
	uint32_t pend = (a->open || a->tap) && !a->suspended
		? r(a, C7_SYS_INTERP_LIST) & r(a, C7_SYS_INTERP_MASK) : 0u;
	if (pend == 0u || a->st.sys_ints != a->sys_seen) {
		a->sys_seen = a->st.sys_ints;
		a->sys_waiting = false;
	} else if (!a->sys_waiting) {
		a->sys_waiting = true;
		a->t_sys_seen = now(a);
	} else if ((uint32_t)(now(a) - a->t_sys_seen) > SYS_STUCK_US) {
		a->st.sys_stuck++;
		w(a, C7_SYS_INTERP_CLEAR, r(a, C7_SYS_INTERP_LIST));
		a->sys_waiting = false;
		a->gap = true;	// the pending receive half/record was discarded
		recovered = true;
	}
	if (a->open && a->tx != HRC7000_AIR_RX) {
		uint32_t t = now(a);
		uint32_t last = a->tick_any ? a->t_tick : a->t_begin;
		if ((uint32_t)(t - last) > STALL_US || (!a->pa && (uint32_t)(t - a->t_begin) > NO_KEY_US)) {
			a->st.stalls++;		// no slot clock or no RF TX interrupt: never leave the PA up without them
			tx_stop(a, t, true);
			recovered = true;
		}
	}
	unlock(a);
	if (a->open && !a->tap && (a->fit & AIR_FIT_RX) != 0) {
#if CONFIG_HRC7000_NATIVE_RX
		if (a->native_rx) {
			rx_records(a);
		} else
#endif
		{
			rx_samples(a);
		}
	}
	while (a->open && a->ev_tail != a->ev_head) {
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		hrc7000_air_ev_t e = a->evq[a->ev_tail % HRC7000_AIR_EVQ];
		a->ev_tail++;
		air_event_t ev = { .kind = e.kind, .t_us = e.t_us, .arg = 0, .chunk = NULL };
		air_event_cb_t cb = a->cb;
		if (cb != NULL) {
			cb(&ev, a->cb_ctx);
		}
	}
	if (a->open && !a->suspended && !a->tap && a->tx == HRC7000_AIR_RX &&
	    (a->fit & AIR_FIT_RX_IDLE_REARM) != 0 && a->cb != NULL) {
		// A stopped symbol stream cannot supply the callback that expires its own partial frame.
		air_event_t ev = { .kind = AIR_EV_RX_POLL, .t_us = now(a) };
		a->cb(&ev, a->cb_ctx);
	}
	return recovered;
}

bool hrc7000_air_tx_begin(hrc7000_air_t *a)
{
	lock(a);
	bool ok = a->open && a->tx == HRC7000_AIR_RX && !a->suspended && (a->fit & AIR_FIT_TX) != 0 && txq_count(a) > 0;

	if (ok && !phy_reset(a)) {
		// The board has not been keyed by this provider. Discard priming, keeping RX ownership/configuration.
		a->txq_tail = a->txq_head;
		a->end_req = false;
		unlock(a);
		return false;
	}
	if (ok) {
		a->sys_waiting = false;
		a->copy = false;	// the chip leaves FM: no copy halves while it sends
#if CONFIG_HRC7000_NATIVE_RX
		if (a->native_rx) {
			native_off(a, true);
			w(a, C7_SYS_INTERP_MASK, C7_SYS_FM_RX | SYS_PHY_RDY);	// the unchanged physical TX interrupt set-up
		}
#endif
		// The modulation bytes the board wrote at hal_radio_t.tx() for this channel, scaled to the profile; back after
		a->mod_bias = r(a, C7_RF_MOD_BIAS);
		a->mod_saved = true;
		w(a, C7_RF_MOD_BIAS, hrc7000_air_mod_bias(a->p, a->mod_bias));
		w(a, C7_LAYER2_TXRX_CTRL, TXRX_NONE);
		w(a, C7_LAYER2_SLOTON, 0);
		w(a, C7_LAYER2_CONTROL, 0);	// release receive ownership before changing physical-layer modes
		w(a, C7_WORK_MODE, C7_WORK_MODE_FM);	// through FM: resets the state machine
		w(a, C7_WORK_MODE, WORK_MODE_PHY);
		// Idle in both halves until the first chunk's slot, so an early key sends idle. Half 0 first
		a->half = 0;
		fill_half(a, NULL, false);
		fill_half(a, NULL, false);
		a->last_due = false;
		a->lead = HRC7000_AIR_TX_LEAD_SLOTS;
		clear_requests(a);	// before own PHY timing: a latched RF/slot request must not strand the first key-up
		w(a, C7_LAYER2_CONTROL, L2_PHY_TX_MASTER);	// own timing: the first boundary comes within a slot
		w(a, C7_AUDIO_BUFFER_CLR, 0x03);
		a->t_begin = now(a);
		a->tick_any = false;
		a->underrun = false;
		a->pa = false;
		a->tx = HRC7000_AIR_START;
	}
	unlock(a);
	return ok;
}

bool hrc7000_air_tx_queue(hrc7000_air_t *a, const air_tx_chunk_t *c)
{
	lock(a);
	bool ok = a->open && (a->tx == HRC7000_AIR_RX || a->tx == HRC7000_AIR_START || a->tx == HRC7000_AIR_RUN)
		  && !a->end_req && !last_queued(a) && c->nsyms > 0 && c->nsyms <= HRC7000_AIR_SYMS
		  && txq_count(a) < HRC7000_AIR_TXQ;

	if (ok) {
		a->buf->txq[a->txq_head % HRC7000_AIR_TXQ] = *c;
		__atomic_thread_fence(__ATOMIC_RELEASE);
		a->txq_head++;
	}
	unlock(a);
	return ok;
}

uint8_t hrc7000_air_tx_space(const hrc7000_air_t *a)
{
	if (!a->open || a->end_req || last_queued(a) || a->tx == HRC7000_AIR_LAST || a->tx == HRC7000_AIR_TAIL) {
		return 0;
	}
	return (uint8_t)(HRC7000_AIR_TXQ - txq_count(a));
}

void hrc7000_air_tx_end(hrc7000_air_t *a)
{
	lock(a);
	if (a->open) {
		if (txq_count(a) > 0) {
			a->buf->txq[(uint8_t)(a->txq_head - 1u) % HRC7000_AIR_TXQ].flags |= AIR_TX_LAST;
		} else if (a->tx == HRC7000_AIR_START || a->tx == HRC7000_AIR_RUN) {
			a->end_req = true;
		}
	}
	unlock(a);
}

void hrc7000_air_tx_abort(hrc7000_air_t *a)
{
	lock(a);
	if (a->open) {
		if (a->tx != HRC7000_AIR_RX) {
			tx_stop(a, now(a), true);
		} else {
			a->txq_tail = a->txq_head;
			a->end_req = false;
		}
	}
	unlock(a);
	notify(a);
}

bool hrc7000_air_tx_active(const hrc7000_air_t *a)
{
	return a->open && a->tx != HRC7000_AIR_RX;
}

#endif	// CONFIG_AIR
