/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus ACARS decoder (acars.h; the format and its sources are described there). Written for this project from
 * the public descriptions; no code from other decoders. */
#include "config.h"
#if CONFIG_MSG_RX
#include <string.h>
#include "acars.h"

/* --- front end --- */
#define HP_A_Q12	3315		/* one-pole high pass at about 300 Hz: a = 1 / (1 + 2 pi 300 / 8000) */
#define BIT_STEP	1288490189u	/* 2^32 x 2400 / 8000 */
#define LO_STEP		9u		/* 1800 Hz is 9 / 40 of the sample rate: the mixer steps 9 entries of ... */
#define LO_N		40u		/* ... a 40-entry cosine table */
#define LO_QUARTER	10u
#define IQ_SHIFT	6		/* I and Q to about 12 bits: the detector's products stay in 24 */
#define SNAP_QUIET	10u		/* bits without a crossing after which the next one sets the clock */
#define HALF		0x80000000u	/* half a bit of the clock */
#define GAIN_HUNT	0x04000000u	/* a timing step of 1/64 bit per transition while looking for a block ... */
#define GAIN_LOCKED	0x02000000u	/* ... and 1/128 inside one */

/* cos(2 pi k / 40) in Q14 */
static const int16_t lo_cos[LO_N] = {
	16384, 16182, 15582, 14598, 13255, 11585, 9630, 7438, 5063, 2563, 0, -2563, -5063, -7438, -9630, -11585, -13255,
	-14598, -15582, -16182, -16384, -16182, -15582, -14598, -13255, -11585, -9630, -7438, -5063, -2563, 0, 2563, 5063,
	7438, 9630, 11585, 13255, 14598, 15582, 16182,
};

/* --- framing --- */
#define SYNC_PAT	0x54686880u	/* '*' SYN SYN SOH (0x2A 0x16 0x16 0x01), each least significant bit first */
#define SYNC_ERRS	2u
#define STX		0x02u
#define ETX		0x03u
#define ETB		0x17u
enum { PH_HEAD, PH_TEXT, PH_BCS1, PH_BCS2 };

static unsigned popcount32(uint32_t v)
{
	unsigned n = 0;
	while (v != 0) {
		v &= v - 1u;
		n++;
	}
	return n;
}

uint16_t acars_crc(uint16_t crc, uint8_t byte)
{
	crc ^= byte;
	for (int k = 0; k < 8; k++)
		crc = (crc & 1u) ? (uint16_t)((crc >> 1) ^ 0x8408u) : (uint16_t)(crc >> 1);
	return crc;
}

static void hunt_again(acars_t *a)
{
	a->map = -1;
}

static void finish(acars_t *a)
{
	acars_msg_t *m = &a->m;
	m->crc_ok = a->crc == 0;
	if (!m->crc_ok) {
		a->crc_bad++;
		if (!a->parity_ok) {
			a->parity_bad++;
			hunt_again(a);
			return;
		}
	}
	/* the address without its leading dots */
	size_t k = 0;
	while (k < 7u && m->reg[k] == '.')
		k++;
	memmove(m->reg, m->reg + k, sizeof m->reg - k);
	for (int i = 0; i < 2; i++)
		if (m->label[i] == 0x7F)
			m->label[i] = 'd';	/* DEL: "_d", as ACARS listings write it */
	m->text[m->n] = '\0';
	a->msgs++;
	if (a->fn != NULL)
		a->fn(a->ctx, m);
	hunt_again(a);
}

static void text_char(acars_t *a, char c)
{
	acars_msg_t *m = &a->m;
	uint16_t i = (uint16_t)(a->pos - 1u);
	if (m->block >= '0' && m->block <= '9') {
		if (i < 4u) {
			m->msn[i] = c;
			return;
		}
		if (i < 10u) {
			m->flight[i - 4u] = c;
			return;
		}
	}
	if ((unsigned char)c < 0x20u || c == 0x7F)
		c = ' ';	/* CR LF between lines */
	if (m->n + 1u < ACARS_TEXT_MAX)
		m->text[m->n++] = c;
	else
		m->cut = true;
}

static void byte_in(acars_t *a, uint8_t v)
{
	a->crc = acars_crc(a->crc, v);
	if (a->phase == PH_BCS1) {
		a->phase = PH_BCS2;
		return;
	}
	if (a->phase == PH_BCS2) {
		finish(a);
		return;
	}
	if ((popcount32(v) & 1u) == 0)
		a->parity_ok = false;
	char c = (char)(v & 0x7Fu);
	acars_msg_t *m = &a->m;
	if (a->phase == PH_HEAD) {
		uint16_t i = a->pos++;
		if (i == 0) {
			if ((unsigned char)c < 0x20u) {
				hunt_again(a);	/* not a mode character: a false sync */
				return;
			}
			m->mode = c;
		} else if (i <= 7u) {
			m->reg[i - 1u] = c;
		} else if (i == 8u) {
			m->ack = c;
		} else if (i <= 10u) {
			m->label[i - 9u] = c;
		} else if (i == 11u) {
			m->block = c;
		} else if (c == STX) {
			a->phase = PH_TEXT;
			a->pos = 0;
		} else if (c == ETX) {
			a->phase = PH_BCS1;	/* no text */
		} else {
			hunt_again(a);
		}
		return;
	}
	if (c == ETX || c == ETB) {
		m->more = c == ETB;
		a->phase = PH_BCS1;
		return;
	}
	if (++a->pos > ACARS_TEXT_LIMIT) {
		hunt_again(a);
		return;
	}
	text_char(a, c);
}

static void lock(acars_t *a, int8_t map)
{
	a->map = map;
	a->nb = 0;
	a->byte = 0;
	a->phase = PH_HEAD;
	a->pos = 0;
	a->crc = 0;
	a->parity_ok = true;
	memset(&a->m, 0, sizeof a->m);
	a->syncs++;
}

static void tone_bit(acars_t *a, unsigned t)
{
	a->bits++;
	if (a->quiet < 255u)
		a->quiet++;
	unsigned change = t != (unsigned)a->t_last;
	a->t_last = t != 0;
	a->sr_tone = a->sr_tone << 1 | t;
	a->sr_change = a->sr_change << 1 | change;
	if (a->map < 0) {
		for (int8_t map = 0; map < 4; map++) {
			uint32_t sr = (map & 2) ? a->sr_change : a->sr_tone;
			if (map & 1)
				sr = ~sr;
			if (popcount32(sr ^ SYNC_PAT) <= SYNC_ERRS) {
				lock(a, map);
				return;
			}
		}
		return;
	}
	unsigned b = (a->map & 2) ? change : t;
	if (a->map & 1)
		b ^= 1u;
	a->byte = (uint8_t)((a->byte >> 1) | (b << 7));
	if (++a->nb == 8u) {
		a->nb = 0;
		byte_in(a, a->byte);
	}
}

/* The detector's output between the last sample (s0) and this one (s1), past (clock units) before this one */
static int32_t strobe(int32_t s0, int32_t s1, uint32_t past)
{
	int32_t f = (int32_t)(past / (BIT_STEP >> 5));	/* in 1/32 of a sample (the product stays in int32) */
	if (f > 32)
		f = 32;
	return s1 - (((s1 - s0) * f) >> 5);
}

static void sample(acars_t *a, int32_t x)
{
	int32_t y = (HP_A_Q12 * (a->hp_y + x - a->hp_x)) >> 12;
	a->hp_x = x;
	a->hp_y = y;
	if (y > 32767)
		y = 32767;
	else if (y < -32767)
		y = -32767;
	/* to baseband around 1800 Hz: I = y cos, Q = -y sin; sin(t) = cos(t - quarter) */
	int32_t ii = (y * lo_cos[a->lo]) >> 14;
	int32_t qq = -((y * lo_cos[(a->lo + LO_N - LO_QUARTER) % LO_N]) >> 14);
	a->lo = (uint8_t)((a->lo + LO_STEP) % LO_N);
	/* low pass [1 1 1] then [1 1]: its zeros near the mixing images at 3000 and 3800 Hz */
	int32_t ia = ii + a->i1 + a->i2, qa = qq + a->q1 + a->q2;
	a->i2 = a->i1;
	a->i1 = ii;
	a->q2 = a->q1;
	a->q1 = qq;
	int32_t ib = (ia + a->ia) >> IQ_SHIFT, qb = (qa + a->qa) >> IQ_SHIFT;
	a->ia = ia;
	a->qa = qa;
	/* the turn over three samples (0.9 bit): MSK at +-600 Hz turns +-81 degrees, positive for 1200 Hz */
	int32_t sm = ib * a->qh[2] - qb * a->ih[2];
	a->ih[2] = a->ih[1];
	a->ih[1] = a->ih[0];
	a->ih[0] = ib;
	a->qh[2] = a->qh[1];
	a->qh[1] = a->qh[0];
	a->qh[0] = qb;
	int32_t s0 = a->sm;
	a->sm = sm;
	int32_t mag = sm < 0 ? -sm : sm;
	a->level += (mag - a->level) >> 6;
	int32_t hyst = a->level >> 3;
	bool edge = false;
	if (a->hi && sm < -hyst) {
		a->hi = false;
		edge = true;
	} else if (!a->hi && sm > hyst) {
		a->hi = true;
		edge = true;
	}
	if (edge && a->quiet >= SNAP_QUIET && a->map < 0) {
		/* the first crossing after a steady tone: a bit boundary about now, so a bit center half a bit on */
		a->ph = HALF;
		a->quiet = 0;
		return;
	}
	uint32_t old = a->ph;
	a->ph += BIT_STEP;
	if (old < HALF && a->ph >= HALF) {
		a->y_half = strobe(s0, sm, a->ph - HALF);
		a->ph += (uint32_t)a->adj;	/* the timing correction, here where it cannot skip a strobe */
		a->adj = 0;
	}
	if (a->ph < old) {
		int32_t yb = strobe(s0, sm, a->ph);
		bool now = yb > 0, before = a->y_prev > 0;
		if (now != before) {
			/* Gardner, signs only: the sample between the two bits already on the new side means late strobes */
			int32_t g = a->map >= 0 ? (int32_t)GAIN_LOCKED : (int32_t)GAIN_HUNT;
			a->adj = (a->y_half > 0) == now ? g : -g;
			a->quiet = 0;
		}
		a->y_prev = yb;
		tone_bit(a, now ? 1u : 0u);
	}
}

void acars_init(acars_t *a, acars_fn fn, void *ctx)
{
	memset(a, 0, sizeof *a);
	a->map = -1;
	a->fn = fn;
	a->ctx = ctx;
}

void acars_audio(acars_t *a, const int16_t *s, size_t n)
{
	for (size_t i = 0; i < n; i++)
		sample(a, s[i]);
}
#endif
