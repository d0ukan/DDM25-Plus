/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus dPMR voice receive (dpmr_rx.h). Ours; the receiver wrapper follows src/nxdn/nxdn_rx.c's. */
#include "dpmr_rx.h"
#include <string.h>

enum {
	FS1_SIGNS = 3,		/* FS1: wrong symbol signs of 24 and ... */
	FS1_BITS = 8,		/* ... wrong bits of 48 (a +3 read as +1 is one): about 1e-7 a symbol in noise */
	FS2_HUNT_ERRORS = 2,	/* of 24 bits: about one false hit in 10 s of noise, dropped at the next sync field */
	FS_SIGNS = 2, FS_BITS = 4,	/* FS2 / FS3 where a synced frame's sync field must be */
	CC_ERRORS = 1,		/* the 64 codes are 3 symbols apart: one wrong sign corrected ... */
	CC_BITS = 3,		/* ... with at most 3 bits wrong (1 to confirm a hunt's sync) */
	PREAMBLE_HOLD = 48u,	/* dibits after a preamble the demodulator keeps its acquisition (FS1 is 24) */
	FLYWHEEL = 3,		/* sync fields missed in a row before sync is lost */
	HEADER_DIBITS = (DPMR_HI_BITS * 2u + 24u) / 2u,	/* HI0 CC HI1: 132 */
	BLOCK_DIBITS = 36u,	/* CCH, one TCH block, the END pair */
};

static unsigned pop32(uint32_t v) { return (unsigned)__builtin_popcount(v); }
static unsigned pop64(uint64_t v) { return (unsigned)__builtin_popcountll(v); }

static void emit(dpmr_rx_t *rx)
{
	if (rx->cfg.event)
		rx->cfg.event(rx->cfg.ctx, DPMR_EV_CALL, &rx->call);
}

static void end_call(dpmr_rx_t *rx)
{
	bool was = rx->in_call;
	rx->in_call = rx->admitted = rx->data_frame = rx->playing = false;
	rx->quarters = 0;
	if (was) {
		rx->ends++;
		if (rx->cfg.event)
			rx->cfg.event(rx->cfg.ctx, DPMR_EV_END, NULL);
	}
}

static void begin(dpmr_rx_t *rx, uint8_t state)
{
	rx->state = state;
	rx->pos = 0;
	memset(rx->buf, 0, sizeof rx->buf);
}

void dpmr_rx_init(dpmr_rx_t *rx, const dpmr_rx_cfg_t *cfg)
{
	memset(rx, 0, sizeof *rx);
	rx->cfg = *cfg;
	rx->call.cc = DPMR_CC_NONE;
}

/* Back to the hunt: whatever call was on has ended */
static void hunt(dpmr_rx_t *rx)
{
	end_call(rx);
	begin(rx, DPMR_RX_HUNT);
	rx->tentative = rx->hunt_header = rx->pending = false;
	rx->misses = 0;
}

void dpmr_rx_reset(dpmr_rx_t *rx)
{
	hunt(rx);
	rx->hold = 0;
}

static bool cc_ok(const dpmr_rx_t *rx, unsigned cc)
{
	return rx->cfg.cc == 0 || cc + 1u == rx->cfg.cc;
}

/* Voice plays once the colour code is known to be the channel's (with "any": at once) */
static void admit(dpmr_rx_t *rx)
{
	bool cc = rx->call.cc != DPMR_CC_NONE ? cc_ok(rx, rx->call.cc) : rx->cfg.cc == 0;
	rx->admitted = rx->in_call && cc && dpmr_mode_voice(rx->call.mode);
}

static void new_call(dpmr_rx_t *rx)
{
	rx->calls++;
	rx->in_call = true;
	rx->quarters = 0;
	memset(&rx->call, 0, sizeof rx->call);
	rx->call.cc = DPMR_CC_NONE;
}

static uint32_t bits_at(const uint8_t *b, unsigned first, unsigned n)
{
	uint32_t v = 0;
	for (unsigned i = 0; i < n; i++)
		v = v << 1 | dpmr_bit(b, first + i);
	return v;
}

static void header_done(dpmr_rx_t *rx)
{
	dpmr_hi_t h;
	if (!dpmr_hi_decode_pair(rx->buf, rx->buf + 18, &h)) {
		/* The hunt can't tell FS1 from FS4 in the other polarity: a packet data header (not followed here)? */
		bool data = false;
		if (rx->hunt_header) {
			for (unsigned i = 0; i < sizeof rx->buf; i++)
				rx->buf[i] ^= 0xAAu;
			data = dpmr_hi_decode_pair(rx->buf, rx->buf + 18, &h);
		}
		if (data)
			rx->data_headers++;
		else
			rx->bad_hi++;
		hunt(rx);
		return;
	}
	rx->headers++;
	rx->hunt_header = false;
	int cc = dpmr_cc_match(bits_at(rx->buf, DPMR_HI_BITS, 24), CC_ERRORS, NULL);
	if (cc >= 0 && !cc_ok(rx, (unsigned)cc)) {
		rx->foreign_cc++;
		hunt(rx);
		return;
	}
	if (h.type == DPMR_HT_START || h.type == DPMR_HT_SYS_ACK || h.type == DPMR_HT_SYS_DELIVERY) {
		/* a superframe follows: a call (another caller without an end frame between ends the last) */
		if (rx->in_call && (!rx->call.have_own || rx->call.own != h.own || !rx->call.have_called ||
				    rx->call.called != h.called))
			end_call(rx);
		bool fresh = !rx->in_call;
		if (fresh)
			new_call(rx);
		bool changed = fresh || rx->call.mode != h.mode || rx->call.format != h.format ||
			       rx->call.flags != h.flags || (cc >= 0 && rx->call.cc != (uint8_t)cc);
		rx->call.called = h.called;
		rx->call.own = h.own;
		rx->call.have_called = rx->call.have_own = true;
		rx->call.mode = h.mode;
		rx->call.format = h.format;
		rx->call.flags = h.flags;
		if (cc >= 0)
			rx->call.cc = (uint8_t)cc;
		rx->data_frame = !dpmr_mode_voice(h.mode);
		admit(rx);
		if (changed)
			emit(rx);
	}
	rx->fn_next = 0;
	rx->misses = 0;
	rx->tentative = false;
	begin(rx, DPMR_RX_SYNC);
}

static void take_cc(dpmr_rx_t *rx, unsigned cc)
{
	if (rx->in_call && rx->call.cc != cc) {
		rx->call.cc = (uint8_t)cc;
		admit(rx);
		emit(rx);
	}
}

static void take_cch(dpmr_rx_t *rx, const dpmr_cch_t *c);

/* The 12 dibits where a frame's sync field must be (in rx->shift) */
static void sync_done(dpmr_rx_t *rx)
{
	uint32_t w = (uint32_t)rx->shift & 0xFFFFFFu;
	if (rx->inverted)
		w ^= DPMR_SYMBOL_INVERT24;
	/* Where the field must be: the sync words and codes are +3 / -3 symbols only, so a sign wrong weighs more than a
	 * +3 read as +1 (what noise mostly does). FS2 and FS3 are 6 symbols apart, FS3 one from code 28. A hunt's
	 * tentative sync is confirmed only by a field within 2 bits (FS2) or 1 bit (a code) of 24 */
	unsigned s2 = pop32((w ^ DPMR_FS2) & DPMR_SYMBOL_INVERT24), b2 = pop32(w ^ DPMR_FS2);
	unsigned s3 = pop32((w ^ DPMR_FS3) & DPMR_SYMBOL_INVERT24), b3 = pop32(w ^ DPMR_FS3);
	int cc = dpmr_cc_match(w, CC_ERRORS, NULL);
	bool t = rx->tentative;
	bool fs2 = t ? b2 <= 2u : (s2 <= FS_SIGNS && b2 <= FS_BITS);
	bool fs3 = t ? b3 <= 2u : (s3 <= FS_SIGNS && b3 <= FS_BITS);
	bool code = cc >= 0 && pop32(w ^ dpmr_cc_codes[cc]) <= (t ? 1u : CC_BITS);
	enum { MISS, FRAME, CC, END } kind = MISS;
	if ((rx->fn_next & 1u) == 0)	/* before frames 1 and 3: FS2, or FS3 after a superframe */
		kind = fs3 && b3 < b2 ? END : fs2 ? FRAME : code ? CC : MISS;
	else				/* before frames 2 and 4: a code (FS2 if the numbering slipped) */
		kind = code ? CC : fs2 && !t ? FRAME : MISS;
	if (kind == CC && !cc_ok(rx, (unsigned)cc)) {
		rx->foreign_cc++;
		hunt(rx);
		return;
	}
	if (kind == END) {
		rx->end_frames++;
		end_call(rx);
		rx->tentative = false;
		begin(rx, DPMR_RX_END);
		return;
	}
	if (kind == MISS) {
		if (rx->tentative) {
			rx->false_syncs++;
			hunt(rx);
			return;
		}
		if (++rx->misses > FLYWHEEL) {
			rx->sync_losses++;
			hunt(rx);
			return;
		}
	} else {
		rx->misses = 0;
		if (rx->tentative) {
			/* the hunt's sync is confirmed: its frame's CCH counts now */
			rx->tentative = false;
			if (rx->pending) {
				rx->pending = false;
				rx->frames++;
				take_cch(rx, &rx->held);
			}
		}
		if (kind == CC)
			take_cc(rx, (unsigned)cc);
	}
	begin(rx, DPMR_RX_CCH);
}

static void take_cch(dpmr_rx_t *rx, const dpmr_cch_t *c)
{
	unsigned fn = c->fn;
	bool pair = (fn & 1u) && ((rx->quarters >> (fn - 1u)) & 1u);
	uint32_t id = pair ? (uint32_t)rx->quarter[fn - 1u] << 12 | c->id : 0;
	bool own = fn >= 2u;
	if (pair && rx->in_call && (own ? rx->call.have_own && rx->call.own != id :
				    rx->call.have_called && rx->call.called != id))
		end_call(rx);	/* another call followed without an end frame */
	bool changed = false;
	if (!rx->in_call) {
		new_call(rx);
		rx->late_entries++;
		changed = true;
	}
	rx->quarter[fn] = c->id;
	rx->quarters = (uint8_t)(rx->quarters | 1u << fn | (pair ? 1u << (fn - 1u) : 0u));
	if (pair) {
		uint32_t *slot = own ? &rx->call.own : &rx->call.called;
		bool *have = own ? &rx->call.have_own : &rx->call.have_called;
		if (!*have || *slot != id) {
			*slot = id;
			*have = true;
			changed = true;
		}
	}
	if (rx->call.mode != c->mode || rx->call.format != c->format || rx->call.flags != c->flags) {
		rx->call.mode = c->mode;
		rx->call.format = c->format;
		rx->call.flags = c->flags;
		changed = true;
	}
	/* voice + appended data: the slow data's data position says when the frames carry data instead */
	rx->data_frame = !dpmr_mode_voice(c->mode) || (c->mode == DPMR_M_VOICE_DATA && ((c->sld >> 11) & 3u) == 3u);
	admit(rx);
	if (changed)
		emit(rx);
}

/* A tentative frame (the hunt's) stays on until its next sync field, whatever its CCH: noise rarely has one 192
 * symbols on. Its CCH is held until then, so no call starts on a false sync */
static void cch_done(dpmr_rx_t *rx)
{
	dpmr_cch_t c;
	unsigned fn = rx->fn_next;
	if (dpmr_cch_decode(rx->buf, &c, NULL)) {
		fn = c.fn;
		if (rx->tentative) {
			rx->held = c;
			rx->pending = true;
		} else {
			rx->frames++;
			take_cch(rx, &c);
		}
	} else if (!rx->tentative) {
		rx->bad_cch++;
		if (rx->in_call)
			rx->frames++;
	}
	rx->fn_next = (uint8_t)((fn + 1u) & 3u);
	rx->slot = 0;
	begin(rx, DPMR_RX_TCH);
}

static void slot_done(dpmr_rx_t *rx)
{
	bool play = rx->in_call && rx->admitted && !rx->tentative && !rx->data_frame;
	rx->playing = play;
	if (play && rx->cfg.voice) {
		rx->cfg.voice(rx->cfg.ctx, rx->buf);
		rx->slots_played++;
	} else if (rx->in_call) {
		rx->slots_muted++;
	}
	begin(rx, ++rx->slot < 4u ? DPMR_RX_TCH : DPMR_RX_SYNC);
}

void dpmr_rx_push_dibit(dpmr_rx_t *rx, uint8_t dibit)
{
	dibit &= 3u;
	rx->shift = ((rx->shift << 2) | dibit) & 0xFFFFFFFFFFFFull;
	uint64_t x = rx->shift ^ DPMR_FS1;
	unsigned f1 = pop64(x), f1s = pop64(x & DPMR_SYMBOL_INVERT48), f1i = pop64(x ^ DPMR_SYMBOL_INVERT48);
	bool fs1 = f1s <= FS1_SIGNS && f1 <= FS1_BITS;		/* FS1 (or FS4 in the other polarity) */
	bool fs1i = 24u - f1s <= FS1_SIGNS && f1i <= FS1_BITS;	/* FS1 inverted, or FS4 */
	uint32_t w = (uint32_t)rx->shift & 0xFFFFFFu;
	if (rx->state == DPMR_RX_HUNT) {
		/* 16 symbols of preamble (+3 +3 -3 -3 ...: 5F 5F, any phase, either polarity): a transmission starts,
		 * the demodulator keeps the acquisition it made on it until FS1 is through (dpmr_receiver_audio) */
		uint32_t p = (uint32_t)rx->shift;
		if (p == 0x5F5F5F5Fu || p == 0x7D7D7D7Du || p == 0xF5F5F5F5u || p == 0xD7D7D7D7u)
			rx->hold = PREAMBLE_HOLD;
		else if (rx->hold)
			rx->hold--;
		if (fs1 || fs1i) {
			rx->inverted = fs1i;
			rx->hunt_header = true;
			begin(rx, DPMR_RX_HEADER);
			return;
		}
		unsigned e = pop32(w ^ DPMR_FS2), ei = pop32(w ^ DPMR_FS2 ^ DPMR_SYMBOL_INVERT24);
		if (e <= FS2_HUNT_ERRORS || ei <= FS2_HUNT_ERRORS) {
			rx->inverted = ei < e;
			rx->sync_errors = (uint8_t)(ei < e ? ei : e);
			rx->tentative = true;
			rx->misses = 0;
			rx->fn_next = 0;
			begin(rx, DPMR_RX_CCH);
		}
		return;
	}
	if (rx->tentative && (fs1 || fs1i)) {
		/* parts of FS1 are within two bits of FS2: the hunt's FS2 was the header's */
		rx->inverted = fs1i;
		rx->hunt_header = true;
		rx->tentative = rx->pending = false;
		begin(rx, DPMR_RX_HEADER);
		return;
	}
	if (rx->state != DPMR_RX_HEADER && (rx->inverted ? fs1i : fs1)) {
		rx->hunt_header = false;	/* a header in the polarity already known */
		begin(rx, DPMR_RX_HEADER);
		return;
	}
	if (rx->tentative && (rx->state == DPMR_RX_CCH || rx->state == DPMR_RX_TCH)) {
		/* until the next sync field confirms the hunt's FS2, a cleaner one replaces it */
		unsigned e = pop32(w ^ DPMR_FS2), ei = pop32(w ^ DPMR_FS2 ^ DPMR_SYMBOL_INVERT24);
		unsigned m = ei < e ? ei : e;
		if (m < rx->sync_errors) {
			rx->false_syncs++;
			rx->inverted = ei < e;
			rx->sync_errors = (uint8_t)m;
			rx->pending = false;
			begin(rx, DPMR_RX_CCH);
			return;
		}
	}
	if (rx->inverted)
		dibit ^= 2u;
	if (rx->state == DPMR_RX_SYNC) {
		if (++rx->pos == DPMR_SYNC_SYMBOLS)
			sync_done(rx);
		return;
	}
	dpmr_set_bit(rx->buf, 2u * rx->pos, dibit >> 1);
	dpmr_set_bit(rx->buf, 2u * rx->pos + 1u, dibit & 1u);
	rx->pos++;
	switch (rx->state) {
	case DPMR_RX_HEADER:
		if (rx->pos == HEADER_DIBITS)
			header_done(rx);
		break;
	case DPMR_RX_CCH:
		if (rx->pos == BLOCK_DIBITS)
			cch_done(rx);
		break;
	case DPMR_RX_TCH:
		if (rx->pos == BLOCK_DIBITS)
			slot_done(rx);
		break;
	case DPMR_RX_END:
		if (rx->pos == BLOCK_DIBITS) {
			dpmr_end_t e;
			(void)(dpmr_end_decode(rx->buf, 0, &e) || dpmr_end_decode(rx->buf, DPMR_END_BITS, &e));
			begin(rx, DPMR_RX_HUNT);
		}
		break;
	default:
		break;
	}
}

void dpmr_receiver_init(dpmr_receiver_t *r, const dpmr_rx_cfg_t *cfg, uint16_t hpf_hz, uint16_t deemph_us)
{
	memset(r, 0, sizeof *r);
	nxdn_demod_init(&r->demod, hpf_hz, deemph_us);
	dpmr_rx_init(&r->rx, cfg);
	r->demod.onset_reacquire = true;
}

void dpmr_receiver_audio(dpmr_receiver_t *r, const int16_t *pcm, size_t n)
{
	uint8_t dib[48];
	while (n) {
		size_t chunk = n > 150u ? 150u : n;	/* at most 45 dibits */
		size_t nd = nxdn_demod_samples(&r->demod, pcm, chunk, dib, sizeof dib);
		for (size_t i = 0; i < nd; i++)
			dpmr_rx_push_dibit(&r->rx, dib[i]);
		/* no squelch edge to restart on: while hunting, the demodulator acquires again at a carrier's onset */
		r->demod.onset_reacquire = r->rx.state == DPMR_RX_HUNT && r->rx.hold == 0;
		pcm += chunk;
		n -= chunk;
		r->samples += (uint32_t)chunk;
	}
}

void dpmr_receiver_restart(dpmr_receiver_t *r)
{
	nxdn_demod_reacquire(&r->demod);
	dpmr_rx_reset(&r->rx);
	r->demod.onset_reacquire = true;
}
