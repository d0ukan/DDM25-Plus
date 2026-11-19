/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus NXDN48 receive call layer (nxdn_rx.h). Ported from DM32-nxdn-p25 src/nxdn/nxdn_call.c and nxdn_link.c
 * (GPL-3.0-or-later), receive half; events and the receiver wrapper are ours. */
#include "nxdn_rx.h"
#include <string.h>

enum { FSW_MAX_ERRORS = 2, FLYWHEEL_FRAMES = 3 };
/* A symbol's sign inverted flips the first bit of its dibit (01 +3 <-> 11 -3, 00 +1 <-> 10 -1). The FM RX RAM
 * audio's sign on the DM-32 is unmeasured, so the sync is hunted in both polarities, as p25.c does */
#define FSW_INVERT	0xAAAAAu

static void end_call(nxdn_rx_t *rx)
{
	bool was = rx->in_call;
	rx->in_call = rx->have_msg = rx->admitted = rx->playing = false;
	rx->part_mask = 0;
	if (was && rx->cfg.event)
		rx->cfg.event(rx->cfg.ctx, NXDN_EV_END, NULL);
}

void nxdn_rx_init(nxdn_rx_t *rx, const nxdn_rx_cfg_t *cfg)
{
	memset(rx, 0, sizeof *rx);
	rx->cfg = *cfg;
}

void nxdn_rx_reset(nxdn_rx_t *rx)
{
	end_call(rx);
	rx->state = NXDN_RX_HUNT;
	rx->shift = 0;
	rx->fsw_hits = rx->fsw_inverted = 0;
	rx->tentative = false;
	rx->pos = 0;
	rx->misses = 0;
}

static unsigned popcount(uint32_t v)
{
	unsigned n = 0;
	for (; v; v &= v - 1u)
		n++;
	return n;
}

static bool admit(const nxdn_rx_t *rx, const nxdn_call_msg_t *m)
{
	if (!rx->cfg.group)
		return true;
	if (m->call_type == NXDN_CALL_INDIVIDUAL)
		return rx->cfg.unit && m->destination == rx->cfg.unit;
	return m->destination == rx->cfg.group || m->destination == NXDN_GROUP_ALL;
}

static void take_msg(nxdn_rx_t *rx, const nxdn_call_msg_t *m)
{
	if (m->type == NXDN_MSG_TX_REL) {
		rx->releases++;
		end_call(rx);
		return;
	}
	bool changed = !rx->have_msg || m->source != rx->msg.source || m->destination != rx->msg.destination ||
		       m->cipher_type != rx->msg.cipher_type || m->call_type != rx->msg.call_type;
	if (rx->in_call && rx->have_msg && (m->source != rx->msg.source || m->destination != rx->msg.destination))
		end_call(rx);	/* another call without a release between */
	if (!rx->in_call)
		rx->calls++;
	rx->in_call = rx->have_msg = true;
	rx->msg = *m;
	rx->admitted = admit(rx, m) && m->cipher_type == 0;
	if (changed && rx->cfg.event)
		rx->cfg.event(rx->cfg.ctx, NXDN_EV_CALL, &rx->msg);
}

static void voice(nxdn_rx_t *rx, unsigned first, unsigned count)
{
	rx->playing = rx->admitted;
	for (unsigned k = first; k < first + count; k++) {
		if (rx->admitted && rx->cfg.voice) {
			uint8_t slot[NXDN_SLOT_BYTES];
			nxdn_voice_get(rx->frame, k, slot);
			rx->cfg.voice(rx->cfg.ctx, slot);
			rx->slots_played++;
		} else {
			rx->slots_muted++;
		}
	}
}

/* The VCALL repeats unchanged in each SACCH superframe, so its four parts may come from two superframes: late entry
 * needs four frames, not up to seven. Each part has its own CRC */
static void superframe(nxdn_rx_t *rx, const nxdn_sacch_t *s)
{
	unsigned part = 3u - (s->structure & 3u);
	rx->parts[part][0] = (uint8_t)(s->data >> 10);
	rx->parts[part][1] = (uint8_t)(s->data >> 2);
	rx->parts[part][2] = (uint8_t)(s->data << 6);
	rx->part_mask |= (uint8_t)(1u << part);
	if (rx->part_mask != 0xFu)
		return;
	uint8_t l3[NXDN_LAYER3_BYTES] = { 0 };
	for (unsigned p = 0; p < 4u; p++)
		for (unsigned i = 0; i < 18u; i++)
			nxdn_set_bit(l3, p * 18u + i, nxdn_bit(rx->parts[p], i));
	rx->part_mask = 0;
	nxdn_call_msg_t m;
	if (!nxdn_layer3_decode(l3, &m) || m.type != NXDN_MSG_VCALL)
		return;
	if (!rx->have_msg)
		rx->late_entries++;
	take_msg(rx, &m);
}

static bool facch(nxdn_rx_t *rx, unsigned which, nxdn_call_msg_t *m)
{
	uint8_t l3[NXDN_LAYER3_BYTES];
	return nxdn_facch1_decode(rx->frame, which, l3, NULL, rx->surv) && nxdn_layer3_decode(l3, m);
}

static void process(nxdn_rx_t *rx)
{
	nxdn_scramble(rx->frame);
	rx->frames++;
	nxdn_lich_t l;
	uint8_t raw = nxdn_lich_get(rx->frame, NULL);
	if (!nxdn_lich_unpack(raw, &l) || (l.rfct != NXDN_RFCT_RDCH && l.rfct != NXDN_RFCT_RTCH)) {
		rx->bad_lich++;
		/* The LICH has only a parity bit. In a call, a damaged one is taken to be the voice frame's it replaces */
		if (!rx->in_call || !nxdn_lich_unpack(rx->last_lich, &l) || l.fct != NXDN_USC_SACCH_SS)
			return;
		rx->lich_assumed++;
	} else {
		rx->last_lich = raw;
	}
	nxdn_sacch_t s;
	bool sacch_ok = nxdn_sacch_decode(rx->frame, &s, NULL, rx->surv);
	if (sacch_ok) {
		rx->last_ran = s.ran;
		if (rx->cfg.ran && s.ran && s.ran != rx->cfg.ran) {
			rx->foreign_ran++;
			end_call(rx);
			return;
		}
	} else {
		rx->bad_sacch++;
	}
	nxdn_call_msg_t m;
	if (l.fct == NXDN_USC_SACCH_NS) {
		/* header or release: VCALL / TX_REL in FACCH1 (either copy) */
		if (facch(rx, 0, &m) || facch(rx, 1, &m))
			take_msg(rx, &m);
		return;
	}
	if (l.fct != NXDN_USC_SACCH_SS)
		return;
	if (sacch_ok)
		superframe(rx, &s);
	switch (l.option) {
	case NXDN_STEAL_NONE:
		voice(rx, 0, 4);
		break;
	case NXDN_STEAL_FACCH1_1:
		if (facch(rx, 0, &m))
			take_msg(rx, &m);
		voice(rx, 2, 2);
		break;
	case NXDN_STEAL_FACCH1_2:
		voice(rx, 0, 2);
		if (facch(rx, 1, &m))
			take_msg(rx, &m);
		break;
	default:
		for (unsigned h = 0; h < 2u; h++)
			if (facch(rx, h, &m))
				take_msg(rx, &m);
		break;
	}
}

/* Sync on the FSW that ended `ago` dibits back: the frame restarts with the FSW and the dibits since, which are still
 * in the shift register (ago < 10) */
static void sync_at(nxdn_rx_t *rx, unsigned ago, unsigned errors, bool inverted)
{
	rx->state = NXDN_RX_SYNCED;
	rx->tentative = true;
	rx->inverted = inverted;
	rx->sync_errors = (uint8_t)errors;
	memset(rx->frame, 0, sizeof rx->frame);
	for (unsigned i = 0; i < NXDN_FSW_BITS; i++)
		nxdn_set_bit(rx->frame, i, (NXDN_FSW >> (19u - i)) & 1u);
	rx->pos = NXDN_FSW_BITS;
	for (unsigned k = ago; k-- > 0;) {
		uint8_t d = (uint8_t)(((rx->shift >> (2u * k)) & 3u) ^ (inverted ? 2u : 0u));
		nxdn_set_bit(rx->frame, rx->pos, d >> 1);
		nxdn_set_bit(rx->frame, rx->pos + 1u, d & 1u);
		rx->pos = (uint16_t)(rx->pos + 2u);
	}
	rx->misses = 0;
}

/* The LICH of the frame being collected (its 16 bits are in) is one a frame can carry */
static bool lich_plausible(const nxdn_rx_t *rx)
{
	uint8_t raw = 0;
	for (unsigned i = 0; i < 8u; i++)
		raw = (uint8_t)(raw << 1 | (nxdn_bit(rx->frame, NXDN_FSW_BITS + 2u * i) ^
					    nxdn_bit(nxdn_scrambler, NXDN_FSW_BITS + 2u * i)));
	nxdn_lich_t l;
	return nxdn_lich_unpack(raw, &l) && (l.rfct == NXDN_RFCT_RDCH || l.rfct == NXDN_RFCT_RTCH);
}

int nxdn_rx_push_dibit(nxdn_rx_t *rx, uint8_t dibit)
{
	rx->shift = ((rx->shift << 2) | (dibit & 3u)) & 0xFFFFFu;
	unsigned normal = popcount(rx->shift ^ NXDN_FSW), inverse = popcount(rx->shift ^ NXDN_FSW ^ FSW_INVERT);
	bool inv = inverse < normal;
	unsigned errors = inv ? inverse : normal;
	rx->fsw_hits = (uint16_t)(rx->fsw_hits << 1 | (errors <= FSW_MAX_ERRORS));
	rx->fsw_inverted = (uint16_t)(rx->fsw_inverted << 1 | inv);
	if (rx->state == NXDN_RX_HUNT) {
		if (rx->fsw_hits & 1u)
			sync_at(rx, 0, errors, inv);
		return 0;
	}
	/* Until the next frame's FSW confirms a sync the hunt found, a cleaner FSW elsewhere replaces it */
	if (rx->tentative && errors < rx->sync_errors && rx->pos > NXDN_FSW_BITS) {
		rx->false_syncs++;
		sync_at(rx, 0, errors, inv);
		return 0;
	}
	if (rx->inverted)
		dibit ^= 2u;
	nxdn_set_bit(rx->frame, rx->pos, (dibit >> 1) & 1u);
	nxdn_set_bit(rx->frame, rx->pos + 1u, dibit & 1u);
	rx->pos = (uint16_t)(rx->pos + 2u);
	if (rx->tentative && rx->pos == NXDN_FSW_BITS + NXDN_LICH_BITS && !lich_plausible(rx)) {
		/* A sync the hunt found in noise (a 20-bit word within two errors turns up in about 5 % of 100 ms of
		 * noise) must not hold the receiver while the real one passes: hunt again from the FSWs seen since */
		rx->false_syncs++;
		rx->state = NXDN_RX_HUNT;
		rx->tentative = false;
		uint16_t since = rx->fsw_hits & ((1u << (NXDN_LICH_BITS / 2u)) - 1u);
		for (unsigned k = NXDN_LICH_BITS / 2u; k-- > 0;)
			if ((since >> k) & 1u) {
				sync_at(rx, k, FSW_MAX_ERRORS, (rx->fsw_inverted >> k) & 1u);
				break;
			}
		return 0;
	}
	if (rx->pos == NXDN_FSW_BITS) {
		/* The next frame's FSW is in: the flywheel rides over a few damaged ones; then sync is lost */
		if ((rx->inverted ? inverse : normal) > 2u * FSW_MAX_ERRORS) {
			if (++rx->misses > FLYWHEEL_FRAMES) {
				rx->sync_losses++;
				nxdn_rx_reset(rx);
				return 0;
			}
		} else {
			rx->misses = 0;
			rx->tentative = false;	/* confirmed */
		}
	}
	if (rx->pos < NXDN_FRAME_BITS)
		return 0;
	process(rx);
	rx->pos = 0;
	memset(rx->frame, 0, sizeof rx->frame);
	return 1;
}

void nxdn_receiver_init(nxdn_receiver_t *r, const nxdn_rx_cfg_t *cfg, uint16_t hpf_hz, uint16_t deemph_us)
{
	memset(r, 0, sizeof *r);
	nxdn_demod_init(&r->demod, hpf_hz, deemph_us);
	nxdn_rx_init(&r->rx, cfg);
	r->demod.onset_reacquire = true;
}

void nxdn_receiver_audio(nxdn_receiver_t *r, const int16_t *pcm, size_t n)
{
	uint8_t dib[48];
	while (n) {
		size_t chunk = n > 150u ? 150u : n;	/* at most 45 dibits */
		size_t nd = nxdn_demod_samples(&r->demod, pcm, chunk, dib, sizeof dib);
		for (size_t i = 0; i < nd; i++)
			nxdn_rx_push_dibit(&r->rx, dib[i]);
		/* No squelch edge to restart on: while the frame layer hunts, the demodulator acquires again at a
		 * carrier's onset */
		r->demod.onset_reacquire = r->rx.state == NXDN_RX_HUNT;
		pcm += chunk;
		n -= chunk;
		r->samples += (uint32_t)chunk;
	}
}

void nxdn_receiver_restart(nxdn_receiver_t *r)
{
	nxdn_demod_reacquire(&r->demod);
	nxdn_rx_reset(&r->rx);
	r->demod.onset_reacquire = true;
}
