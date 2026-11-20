/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus DOTA Transfer: the protocol (dota_proto.h, docs/dota.md 6 and 7). Installed as core/radio/dota_proto.c. */
#include "config.h"
#if CONFIG_DOTA
#include <string.h>
#include "radio/dota_proto.h"
#include "aprs/ax25.h"		/* ax25_fcs: the CRC-16 (X.25) */
#include "storage/storage.h"	/* storage_crc32: the CRC-32 (IEEE) */

#define SEC_SHIFT	2u
#define LEFT_SHIFT	5u

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static uint16_t get16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

static void put32(uint8_t *p, uint32_t v)
{
	put16(p, (uint16_t)(v >> 16));
	put16(p + 2, (uint16_t)v);
}

static uint32_t get32(const uint8_t *p)
{
	return ((uint32_t)get16(p) << 16) | get16(p + 2);
}

/* --- Frames --- */

size_t dota_frame_put(const dota_frame_t *f, uint8_t *out, size_t max)
{
	size_t call = (f->flags & DOTA_F_CALL) ? DOTA_CALL : 0u;
	size_t n = DOTA_HDR + call + f->len;
	if (n + 2u > max || f->len > DOTA_PAYLOAD_MAX)
		return 0;
	out[0] = (uint8_t)((DOTA_VERSION << 4) | (f->type & 0x0Fu));
	out[1] = (uint8_t)((f->flags & 3u) | ((f->sec & 7u) << SEC_SHIFT) | ((f->left & 7u) << LEFT_SHIFT));
	put16(out + 2, f->code);
	put16(out + 4, f->session);
	put16(out + 6, f->seq);
	put16(out + 8, f->first);
	out[10] = f->count;
	out[11] = f->len;
	if (f->len)
		memmove(out + DOTA_HDR + call, f->payload, f->len);	/* the payload may already be in place */
	if (call) {
		bool end = false;
		for (size_t i = 0; i < DOTA_CALL; i++) {
			end = end || f->call[i] == '\0';
			out[DOTA_HDR + i] = (uint8_t)(end ? ' ' : f->call[i]);
		}
	}
	uint16_t crc = ax25_fcs(out, n);
	out[n] = (uint8_t)crc;
	out[n + 1u] = (uint8_t)(crc >> 8);
	return n + 2u;
}

bool dota_frame_get(const uint8_t *in, size_t n, dota_frame_t *f)
{
	if (n < DOTA_HDR + 2u || (in[0] >> 4) != DOTA_VERSION)
		return false;
	memset(f, 0, sizeof *f);
	f->type = in[0] & 0x0Fu;
	f->flags = in[1] & 3u;
	f->sec = (uint8_t)((in[1] >> SEC_SHIFT) & 7u);
	f->left = (uint8_t)(in[1] >> LEFT_SHIFT);
	f->code = get16(in + 2);
	f->session = get16(in + 4);
	f->seq = get16(in + 6);
	f->first = get16(in + 8);
	f->count = in[10];
	f->len = in[11];
	size_t call = (f->flags & DOTA_F_CALL) ? DOTA_CALL : 0u;
	size_t body = DOTA_HDR + call + f->len;
	if (body + 2u != n || f->len > DOTA_PAYLOAD_MAX || f->type < DOTA_T_HELLO || f->type > DOTA_T_POLL ||
	    f->sec >= DOTA_SEC_COUNT || f->code > 9999u)
		return false;
	uint16_t crc = ax25_fcs(in, body);
	if (in[body] != (uint8_t)crc || in[body + 1u] != (uint8_t)(crc >> 8))
		return false;
	if (call) {
		size_t k = 0;
		for (size_t i = 0; i < DOTA_CALL && in[DOTA_HDR + i] != ' '; i++)
			f->call[k++] = (char)in[DOTA_HDR + i];
		f->call[k] = '\0';
	}
	f->payload = in + DOTA_HDR + call;
	return true;
}

uint32_t dota_frame_crc(uint16_t seq, uint16_t first, uint8_t count, const uint8_t *payload, uint8_t len)
{
	uint8_t h[6];
	put16(h, seq);
	put16(h + 2, first);
	h[4] = count;
	h[5] = len;
	return storage_crc32(storage_crc32(0, h, sizeof h), payload, len);
}

/* --- The manifest --- */

size_t dota_manifest_put(const dota_manifest_t *m, uint8_t *out, size_t max)
{
	size_t fw = strnlen(m->fw, DOTA_FW_MAX);
	if (max < 29u + fw)
		return 0;
	memcpy(out, m->model, 4);
	out[4] = m->rec_ver;
	out[5] = m->layout;
	out[6] = m->modes;
	out[7] = m->kind;
	for (unsigned s = 1; s < DOTA_SEC_COUNT; s++) {
		put16(out + 8u + (s - 1u) * 2u, m->recs[s]);
		put16(out + 16u + (s - 1u) * 2u, m->frames[s]);
	}
	out[24] = (uint8_t)(m->bytes >> 16);	/* 3 octets: at most 512 frames of 192 */
	out[25] = (uint8_t)(m->bytes >> 8);
	out[26] = (uint8_t)m->bytes;
	out[27] = m->zones;
	out[28] = (uint8_t)fw;
	memcpy(out + 29, m->fw, fw);
	return 29u + fw;
}

bool dota_manifest_get(const uint8_t *in, size_t n, dota_manifest_t *m)
{
	if (n < 29u || in[28] > DOTA_FW_MAX || n != 29u + in[28])
		return false;
	memset(m, 0, sizeof *m);
	memcpy(m->model, in, 4);
	m->rec_ver = in[4];
	m->layout = in[5];
	m->modes = in[6];
	m->kind = in[7];
	uint32_t total = 0;
	for (unsigned s = 1; s < DOTA_SEC_COUNT; s++) {
		m->recs[s] = get16(in + 8u + (s - 1u) * 2u);
		m->frames[s] = get16(in + 16u + (s - 1u) * 2u);
		total += m->frames[s];
	}
	m->bytes = ((uint32_t)in[24] << 16) | ((uint32_t)in[25] << 8) | in[26];
	m->zones = in[27];
	memcpy(m->fw, in + 29, in[28]);
	return total >= 1u && total <= DOTA_FRAMES_MAX;
}

uint16_t dota_manifest_total(const dota_manifest_t *m)
{
	uint32_t t = 0;
	for (unsigned s = 1; s < DOTA_SEC_COUNT; s++)
		t += m->frames[s];
	return (uint16_t)t;
}

/* --- The sender --- */

void dota_tx_start(dota_tx_t *s, const dota_link_t *link, const dota_manifest_t *m, uint16_t code, uint16_t session,
		   dota_fill_fn fill, void *ctx)
{
	memset(s, 0, sizeof *s);
	s->link = link;
	s->m = *m;
	s->code = code;
	s->session = session;
	s->fill = fill;
	s->ctx = ctx;
	s->total = dota_manifest_total(m);
	s->next_seq = 1;
	s->cur_sec = DOTA_SEC_CONTACTS;
	s->state = DOTA_TX_READY;
}

static void tx_fail(dota_tx_t *s, uint8_t why)
{
	s->state = DOTA_TX_FAILED;
	/* Once END went out the receiver may have had everything and written it: only its own answer (a refusal, its
	 * stop, a failed result) says otherwise */
	bool told = why == DOTA_TF_REFUSED || why == DOTA_TF_RX_STOPPED || why == DOTA_TF_RESULT;
	s->fail = s->end_sent && !told ? DOTA_TF_UNSURE : why;
}

/* The next section with records left, from cur_sec */
static void next_section(dota_tx_t *s)
{
	while (s->cur_sec < DOTA_SEC_COUNT && s->cur_first >= s->m.recs[s->cur_sec]) {
		s->cur_sec++;
		s->cur_first = 0;
	}
}

static dota_slot_t *find_slot(dota_tx_t *s, uint16_t seq)
{
	for (uint8_t i = 0; i < s->nfly; i++)
		if (s->fly[i].seq == seq)
			return &s->fly[i];
	return NULL;
}

/* A window's frame's bytes on the link (a DATA frame's payload as packed) */
static uint16_t frame_bytes(const dota_tx_t *s, uint16_t payload)
{
	return (uint16_t)(DOTA_HDR + (s->link->call ? DOTA_CALL : 0u) + payload + 2u + s->link->frame_extra);
}

uint8_t dota_tx_window(dota_tx_t *s, uint32_t now, uint8_t *scratch)
{
	const dota_link_t *l = s->link;
	if (s->state == DOTA_TX_REST) {
		if (now - s->t_ms < l->rest_ms)
			return 0;
		s->keyed_ms = 0;
		s->state = DOTA_TX_READY;
	}
	if (s->state != DOTA_TX_READY)
		return 0;
	if (l->rest_after_ms != 0 && s->keyed_ms >= l->rest_after_ms) {
		s->state = DOTA_TX_REST;	/* FM: a rest after each minute of key time (docs/dota.md 7.5) */
		s->t_ms = now;
		return 0;
	}
	uint8_t w = l->window < DOTA_WINDOW_MAX ? l->window : DOTA_WINDOW_MAX;
	uint32_t bytes = 0, budget = l->window_bytes ? l->window_bytes : 0xFFFFFFFFu;
	s->nwin = 0;
	if (s->polls != 0) {
		/* An answer went missing: ask again (HELLO again while it was never answered) */
		s->win[s->nwin++] = !s->hello_ok ? DOTA_WIN_HELLO : s->acked == s->total ? DOTA_WIN_END : DOTA_WIN_POLL;
	} else {
		if (!s->hello_ok) {
			s->win[s->nwin++] = DOTA_WIN_HELLO;
			bytes += frame_bytes(s, DOTA_MANIFEST_MAX);
		}
		/* Resends first, lowest number first (fly is kept in order) */
		for (uint8_t i = 0; i < s->nfly && s->nwin < w; i++) {
			dota_slot_t *t = &s->fly[i];
			if (!t->sent)
				continue;
			uint16_t b = frame_bytes(s, l->payload_max);
			if (bytes + b > budget && s->nwin != 0)
				break;
			if (++t->tries > l->tries) {
				tx_fail(s, DOTA_TF_FRAME);
				return 0;
			}
			s->resent++;
			bytes += b;
			s->win[s->nwin++] = t->seq;
		}
		/* New frames while the span and the window have room */
		uint16_t base = s->nfly ? s->fly[0].seq : s->next_seq;
		while (s->nwin < w && s->next_seq <= s->total && s->next_seq < base + DOTA_SPAN && s->nfly < DOTA_SPAN) {
			next_section(s);
			if (s->cur_sec >= DOTA_SEC_COUNT) {
				tx_fail(s, DOTA_TF_SOURCE);	/* fewer records than the manifest said */
				return 0;
			}
			uint8_t count = 0;
			size_t n = s->fill(s->ctx, s->cur_sec, s->cur_first, &count, scratch, l->payload_max);
			uint16_t b = frame_bytes(s, (uint16_t)n);
			if (n == 0 || count == 0) {
				tx_fail(s, DOTA_TF_SOURCE);
				return 0;
			}
			if (bytes + b > budget && s->nwin != 0)
				break;
			dota_slot_t *t = &s->fly[s->nfly++];
			t->seq = s->next_seq++;
			t->sec = s->cur_sec;
			t->first = s->cur_first;
			t->count = count;
			t->tries = 1;
			t->sent = false;
			s->crc += dota_frame_crc(t->seq, t->first, count, scratch, (uint8_t)n);
			s->cur_first = (uint16_t)(s->cur_first + count);
			bytes += b;
			s->win[s->nwin++] = t->seq;
		}
		if (s->next_seq > s->total && s->nwin < w + 1u && bytes + frame_bytes(s, 12) <= budget)
			s->win[s->nwin++] = DOTA_WIN_END;	/* everything sent at least once: END closes the window */
		if (s->nwin == 0)
			s->win[s->nwin++] = s->next_seq > s->total ? DOTA_WIN_END : DOTA_WIN_POLL;
	}
	s->state = DOTA_TX_SENDING;
	return s->nwin;
}

size_t dota_tx_frame(dota_tx_t *s, uint8_t i, uint8_t *out, size_t max)
{
	if (i >= s->nwin || max < DOTA_HDR + (s->link->call ? DOTA_CALL : 0u) + s->link->payload_max + 2u)
		return 0;
	dota_frame_t f;
	memset(&f, 0, sizeof f);
	uint8_t *pay = out + DOTA_HDR + (s->link->call ? DOTA_CALL : 0u);	/* packed where dota_frame_put puts it */
	uint8_t body[DOTA_MANIFEST_MAX];
	f.code = s->code;
	f.session = s->session;
	f.left = (uint8_t)(s->nwin - 1u - i);
	f.flags = (uint8_t)((s->link->call ? DOTA_F_CALL : 0u) | (f.left == 0 ? DOTA_F_POLL : 0u));
	memcpy(f.call, s->link->me, sizeof f.call);
	uint16_t e = s->win[i];
	if (e == DOTA_WIN_HELLO) {
		f.type = DOTA_T_HELLO;
		f.len = (uint8_t)dota_manifest_put(&s->m, body, sizeof body);
		f.payload = body;
	} else if (e == DOTA_WIN_END) {
		f.type = DOTA_T_END;
		s->end_sent = true;
		put16(body, s->total);
		for (unsigned k = 1; k < DOTA_SEC_COUNT; k++)
			put16(body + 2u * k, s->m.recs[k]);
		put32(body + 2u * DOTA_SEC_COUNT, s->crc);
		f.len = (uint8_t)(2u * DOTA_SEC_COUNT + 4u);
		f.payload = body;
	} else if (e == DOTA_WIN_POLL) {
		f.type = DOTA_T_POLL;
	} else {
		dota_slot_t *t = find_slot(s, e);
		if (t == NULL)
			return 0;
		uint8_t count = t->count;
		size_t n = s->fill(s->ctx, t->sec, t->first, &count, pay, s->link->payload_max);
		if (n == 0 || count != t->count) {
			tx_fail(s, DOTA_TF_SOURCE);
			return 0;
		}
		f.type = DOTA_T_DATA;
		f.sec = t->sec;
		f.seq = t->seq;
		f.first = t->first;
		f.count = t->count;
		f.len = (uint8_t)n;
		f.payload = pay;
		t->sent = true;
	}
	return dota_frame_put(&f, out, max);
}

void dota_tx_sent(dota_tx_t *s, uint32_t keyed_ms, uint32_t now)
{
	if (s->state != DOTA_TX_SENDING)
		return;
	for (uint8_t i = 0; i < s->nwin; i++) {
		dota_slot_t *t = find_slot(s, s->win[i]);
		if (t != NULL)
			t->sent = true;
	}
	s->keyed_ms += keyed_ms;
	s->t_ms = now;
	s->state = DOTA_TX_WAIT;
}

/* The receiver has every frame below base and those its bitmap names */
static void take_ack(dota_tx_t *s, uint16_t base, uint32_t bits)
{
	uint8_t keep = 0;
	for (uint8_t i = 0; i < s->nfly; i++) {
		uint16_t q = s->fly[i].seq;
		uint16_t d = (uint16_t)(q - base);
		bool got = q < base || (d < 32u && ((bits >> d) & 1u));
		if (!got)
			s->fly[keep++] = s->fly[i];
	}
	s->nfly = keep;
	uint16_t acked = (uint16_t)(s->next_seq - 1u - keep);
	if (acked > s->acked)
		s->acked = acked;
}

void dota_tx_heard(dota_tx_t *s, const uint8_t *in, size_t n, const char *peer, uint32_t now)
{
	dota_frame_t f;
	if (s->state != DOTA_TX_WAIT && s->state != DOTA_TX_SENDING)
		return;
	if (!dota_frame_get(in, n, &f) || f.code != s->code || f.session != s->session || f.type == DOTA_T_HELLO ||
	    f.type == DOTA_T_DATA || f.type == DOTA_T_END || f.type == DOTA_T_POLL)
		return;	/* not an answer (our own frames heard back, another sender) */
	const char *who = (f.flags & DOTA_F_CALL) ? f.call : peer;
	if (who != NULL) {
		strncpy(s->peer, who, DOTA_CALL);
		s->peer[DOTA_CALL] = '\0';
	}
	const uint8_t *p = f.payload;
	switch (f.type) {
	case DOTA_T_REFUSE:
		if (f.len < 5u)
			return;
		s->status = p[0];
		s->free_ch = get16(p + 1);
		s->free_zones = p[3];
		s->zone_room = p[4];
		if (f.len >= 6u && p[5] <= DOTA_FW_MAX && f.len >= 6u + p[5]) {
			memcpy(s->peer_fw, p + 6, p[5]);
			s->peer_fw[p[5]] = '\0';
		}
		tx_fail(s, DOTA_TF_REFUSED);
		return;
	case DOTA_T_ACK:
		if (f.len < 7u)
			return;
		s->hello_ok = true;
		take_ack(s, f.seq, get32(p));
		break;
	case DOTA_T_END_ACK:
		if (f.len < 9u)
			return;
		s->hello_ok = true;
		s->status = p[0];
		if (p[0] == DOTA_E_DONE) {
			s->acked = s->total;
			s->ch_added = get16(p + 1);
			s->zones_added = p[3];
			s->ct_added = get16(p + 4);
			s->ct_matched = get16(p + 6);
			s->lists_added = p[8];
			s->state = DOTA_TX_DONE;
			return;
		}
		if (p[0] != DOTA_E_WRITING) {
			tx_fail(s, DOTA_TF_RESULT);
			return;
		}
		/* Still writing (so every frame is in): ask again after end_gap_ms (the wait below) */
		s->acked = s->total;
		s->nfly = 0;
		s->polls = 0;
		s->t_ms = now - s->link->ack_ms + s->link->end_gap_ms;
		s->state = DOTA_TX_WAIT;
		if (++s->end_tries > s->link->end_tries)
			tx_fail(s, DOTA_TF_UNSURE);
		else
			s->polls = 1;	/* the wait ends in END again, not a new window */
		return;
	case DOTA_T_ABORT:
		tx_fail(s, DOTA_TF_RX_STOPPED);
		return;
	default:
		return;
	}
	s->polls = 0;
	s->state = DOTA_TX_READY;
}

void dota_tx_tick(dota_tx_t *s, uint32_t now)
{
	if (s->state != DOTA_TX_WAIT || now - s->t_ms < s->link->ack_ms)
		return;
	if (++s->polls > s->link->polls) {
		tx_fail(s, s->acked == s->total ? DOTA_TF_UNSURE : DOTA_TF_NO_ANSWER);
		return;
	}
	s->state = DOTA_TX_READY;	/* the next window asks again */
}

size_t dota_tx_stop(dota_tx_t *s, uint8_t reason, uint8_t *out, size_t max)
{
	bool live = s->state != DOTA_TX_IDLE && s->state != DOTA_TX_DONE && s->state != DOTA_TX_FAILED;
	if (s->state != DOTA_TX_IDLE && s->state != DOTA_TX_DONE)
		tx_fail(s, DOTA_TF_STOPPED);
	if (!live)
		return 0;
	dota_frame_t f;
	memset(&f, 0, sizeof f);
	f.type = DOTA_T_ABORT;
	f.code = s->code;
	f.session = s->session;
	f.flags = s->link->call ? DOTA_F_CALL : 0u;
	memcpy(f.call, s->link->me, sizeof f.call);
	f.len = 1;
	f.payload = &reason;
	return dota_frame_put(&f, out, max);
}

/* --- The receiver --- */

void dota_rx_arm(dota_rx_t *r, const dota_link_t *link, uint16_t code, dota_hello_fn hello, dota_data_fn data,
		 dota_commit_fn commit, void *ctx, uint32_t now)
{
	char fw[DOTA_FW_MAX + 1];
	memcpy(fw, r->fw, sizeof fw);	/* set by the caller before arming */
	memset(r, 0, sizeof *r);
	memcpy(r->fw, fw, sizeof fw);
	r->link = link;
	r->code = code;
	r->hello = hello;
	r->data = data;
	r->commit = commit;
	r->ctx = ctx;
	r->state = DOTA_RX_ARMED;
	r->last_ms = now;
}

static bool got_bit(const dota_rx_t *r, uint16_t seq)
{
	return seq >= 1u && seq <= DOTA_FRAMES_MAX && (r->got[(seq - 1u) >> 3] >> ((seq - 1u) & 7u)) & 1u;
}

/* When the answer is due: after the POLL frame, or when the window should have ended */
static void schedule(dota_rx_t *r, uint8_t type, const dota_frame_t *f, uint32_t now)
{
	r->answer = type;
	r->answer_at = now + r->link->turn_ms + ((f->flags & DOTA_F_POLL) ? 0u : (uint32_t)f->left * r->link->frame_ms + 300u);
}

static uint16_t sec_start(const dota_rx_t *r, uint8_t sec)
{
	uint16_t q = 1;
	for (uint8_t s = DOTA_SEC_CONTACTS; s < sec; s++)
		q = (uint16_t)(q + r->m.frames[s]);
	return q;
}

void dota_rx_heard(dota_rx_t *r, const uint8_t *in, size_t n, const char *peer, uint32_t now)
{
	dota_frame_t f;
	if (r->state == DOTA_RX_IDLE)
		return;
	if (!dota_frame_get(in, n, &f) || f.code != r->code)
		return;		/* a wrong code is never answered */
	if (r->state == DOTA_RX_DONE || r->state == DOTA_RX_FAILED) {
		/* Over: the sender asking for the result again (its END_ACK went missing) gets the same one */
		bool asks = f.type == DOTA_T_END || f.type == DOTA_T_POLL || f.type == DOTA_T_DATA;
		if (f.session == r->session && asks && (r->state == DOTA_RX_DONE || r->fail == DOTA_RF_CRC ||
							  r->fail == DOTA_RF_FLASH))
			schedule(r, DOTA_T_END_ACK, &f, now);
		return;
	}
	const char *who = (f.flags & DOTA_F_CALL) ? f.call : peer != NULL ? peer : "";
	if (r->state == DOTA_RX_ARMED) {
		if (f.type != DOTA_T_HELLO || f.session == 0) {
			if (r->answer == DOTA_T_REFUSE && f.session == r->session)
				schedule(r, DOTA_T_REFUSE, &f, now);	/* the refused window goes on: answer at its end */
			return;
		}
		dota_manifest_t m;
		if (!dota_manifest_get(f.payload, f.len, &m))
			return;
		uint8_t st = r->hello(r->ctx, &m, who);
		r->m = m;
		strncpy(r->peer, who, DOTA_CALL);
		r->peer[DOTA_CALL] = '\0';
		if (st != DOTA_R_OK) {
			r->refused_status = st;
			r->session = f.session;	/* answered as this session, then forgotten */
			schedule(r, DOTA_T_REFUSE, &f, now);
			return;
		}
		r->session = f.session;
		r->total = dota_manifest_total(&m);
		r->base = 1;
		r->state = DOTA_RX_RECEIVING;
		r->last_ms = now;
		schedule(r, DOTA_T_ACK, &f, now);
		return;
	}
	if (f.session != r->session)
		return;
	r->last_ms = now;
	switch (f.type) {
	case DOTA_T_HELLO:
		schedule(r, r->state == DOTA_RX_COMMIT ? DOTA_T_END_ACK : DOTA_T_ACK, &f, now);
		return;
	case DOTA_T_POLL:
		schedule(r, r->state == DOTA_RX_COMMIT ? DOTA_T_END_ACK : DOTA_T_ACK, &f, now);
		return;
	case DOTA_T_ABORT:
		if (r->state == DOTA_RX_COMMIT)
			return;		/* the writing runs to its end: the sender says it can't be sure */
		r->state = DOTA_RX_FAILED;
		r->fail = DOTA_RF_TX_STOPPED;
		r->answer = 0;
		return;
	case DOTA_T_DATA:
		if (r->state == DOTA_RX_RECEIVING && f.seq >= 1u && f.seq <= r->total && f.sec >= DOTA_SEC_CONTACTS &&
		    f.seq >= sec_start(r, f.sec) && f.seq < sec_start(r, (uint8_t)(f.sec + 1u)) && !got_bit(r, f.seq) &&
		    r->base >= sec_start(r, f.sec)) {
			/* in its section, not had, every frame of the sections before it in: keep it if there is room */
			if (r->data(r->ctx, f.sec, f.first, f.count, f.payload, f.len)) {
				r->got[(f.seq - 1u) >> 3] |= (uint8_t)(1u << ((f.seq - 1u) & 7u));
				r->got_n++;
				r->crc += dota_frame_crc(f.seq, f.first, f.count, f.payload, f.len);
				while (r->base <= r->total && got_bit(r, r->base))
					r->base++;
			} else {
				r->slow = true;
			}
		}
		schedule(r, r->state == DOTA_RX_COMMIT ? DOTA_T_END_ACK : DOTA_T_ACK, &f, now);
		return;
	case DOTA_T_END:
		if (r->state == DOTA_RX_RECEIVING && r->got_n == r->total && f.len >= 2u * DOTA_SEC_COUNT + 4u) {
			if (get32(f.payload + 2u * DOTA_SEC_COUNT) != r->crc || get16(f.payload) != r->total) {
				r->result = DOTA_E_CRC;
				r->state = DOTA_RX_FAILED;
				r->fail = DOTA_RF_CRC;
			} else {
				r->state = DOTA_RX_COMMIT;
				r->result = DOTA_E_WRITING;
				r->commit(r->ctx);
			}
		}
		schedule(r, r->state == DOTA_RX_RECEIVING ? DOTA_T_ACK : DOTA_T_END_ACK, &f, now);
		return;
	default:
		return;
	}
}

size_t dota_rx_answer(dota_rx_t *r, uint32_t now, bool clear, uint8_t *out, size_t max)
{
	if (r->answer == 0 || (int32_t)(now - r->answer_at) < 0 || !clear)
		return 0;
	uint8_t p[6u + DOTA_FW_MAX];
	dota_frame_t f;
	memset(&f, 0, sizeof f);
	f.type = r->answer;
	f.code = r->code;
	f.session = r->session;
	f.flags = (uint8_t)(DOTA_F_POLL | (r->link->call ? DOTA_F_CALL : 0u));
	memcpy(f.call, r->link->me, sizeof f.call);
	f.payload = p;
	switch (r->answer) {
	case DOTA_T_REFUSE: {
		size_t fw = strnlen(r->fw, DOTA_FW_MAX);
		p[0] = r->refused_status;
		put16(p + 1, r->free_ch);
		p[3] = r->free_zones;
		p[4] = r->zone_room;
		p[5] = (uint8_t)fw;
		memcpy(p + 6, r->fw, fw);
		f.len = (uint8_t)(6u + fw);
		r->session = 0;		/* forgotten: back to waiting for a HELLO */
		break;
	}
	case DOTA_T_ACK: {
		uint32_t bits = 0;
		for (uint8_t i = 0; i < 32u; i++)
			if (got_bit(r, (uint16_t)(r->base + i)))
				bits |= 1u << i;
		f.seq = r->base;
		put32(p, bits);
		put16(p + 4, r->got_n);
		p[6] = r->slow ? 1u : 0u;
		r->slow = false;
		f.len = 7;
		break;
	}
	default:	/* END_ACK */
		f.type = DOTA_T_END_ACK;
		p[0] = r->result;
		put16(p + 1, r->ch_added);
		p[3] = r->zones_added;
		put16(p + 4, r->ct_added);
		put16(p + 6, r->ct_matched);
		p[8] = r->lists_added;
		f.len = 9;
		break;
	}
	r->answer = 0;
	return dota_frame_put(&f, out, max);
}

void dota_rx_tick(dota_rx_t *r, uint32_t now)
{
	if ((r->state == DOTA_RX_RECEIVING) && now - r->last_ms >= r->link->idle_ms) {
		r->state = DOTA_RX_FAILED;
		r->fail = DOTA_RF_IDLE;
		r->answer = 0;
	}
}

void dota_rx_commit_done(dota_rx_t *r, bool ok)
{
	if (r->state != DOTA_RX_COMMIT)
		return;
	r->result = ok ? DOTA_E_DONE : DOTA_E_FLASH;
	r->state = ok ? DOTA_RX_DONE : DOTA_RX_FAILED;
	r->fail = ok ? DOTA_RF_NONE : DOTA_RF_FLASH;
}

size_t dota_rx_stop(dota_rx_t *r, uint8_t reason, uint8_t *out, size_t max)
{
	bool live = r->state == DOTA_RX_RECEIVING;
	if (r->state == DOTA_RX_COMMIT)
		return 0;		/* writing: not stopped half way (the UI offers no stop then) */
	if (r->state != DOTA_RX_DONE && r->state != DOTA_RX_IDLE) {
		r->state = DOTA_RX_FAILED;
		r->fail = DOTA_RF_STOPPED;
	}
	r->answer = 0;
	if (!live)
		return 0;
	dota_frame_t f;
	memset(&f, 0, sizeof f);
	f.type = DOTA_T_ABORT;
	f.code = r->code;
	f.session = r->session;
	f.flags = r->link->call ? DOTA_F_CALL : 0u;
	memcpy(f.call, r->link->me, sizeof f.call);
	f.len = 1;
	f.payload = &reason;
	return dota_frame_put(&f, out, max);
}

uint8_t dota_rx_percent(const dota_rx_t *r)
{
	if (r->state == DOTA_RX_DONE)
		return 100;
	return r->total ? (uint8_t)((uint32_t)r->got_n * 100u / r->total) : 0u;
}
#endif
