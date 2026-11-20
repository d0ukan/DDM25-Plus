/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus DOTA Transfer: the records, format version 1 (dota_rec.h, docs/dota.md 9). Installed as
 * core/radio/dota_rec.c. */
#include "config.h"
#if CONFIG_DOTA
#include <string.h>
#include "radio/dota_rec.h"

/* Channel flags A */
#define A_MODE		0x07u
#define A_WIDE		0x08u
#define A_TX_SHIFT	4u	/* bits 5-4: 0 simplex, 1 offset (3 B, 25 Hz units), 2 absolute TX (4 B) */
#define A_RXT		0x40u
#define A_TXT		0x80u
/* Channel flags B */
#define B_POWER		0x03u
#define B_RXONLY	0x04u
#define B_SKIP		0x08u
#define B_NOBEEP	0x10u
#define B_NOECO		0x20u
#define B_VOX		0x40u
#define B_EXTRA		0x80u
/* Extras */
#define X_DMO		0x01u
#define X_TOT		0x02u
#define X_SFR		0x04u
#define X_ALIAS_SHIFT	3u	/* bits 6-3: the talker alias bits */
/* The DMR block's first byte */
#define D_CC		0x1Fu
#define D_TS_SHIFT	5u
#define D_LIST		0x80u

enum { M_FM, M_DMR, M_P25, M_YSF, M_NXDN, M_DPMR, M_COUNT };

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static uint16_t get16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

static void put24(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 16);
	put16(p + 1, (uint16_t)v);
}

static uint32_t get24(const uint8_t *p)
{
	return ((uint32_t)p[0] << 16) | get16(p + 1);
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

/* A name: its length, then printable ASCII (anything else as '?') */
static size_t put_name(const char *name, uint8_t *out)
{
	size_t n = strnlen(name, CODEPLUG_NAME_LEN);
	out[0] = (uint8_t)n;
	for (size_t i = 0; i < n; i++) {
		char c = name[i];
		out[1 + i] = (uint8_t)(c >= 0x20 && c < 0x7F ? c : '?');
	}
	return 1 + n;
}

static size_t get_name(const uint8_t *in, size_t n, char *name)
{
	if (n < 1u || in[0] > CODEPLUG_NAME_LEN || n < 1u + in[0])
		return 0;
	for (size_t i = 0; i < in[0]; i++) {
		if (in[1 + i] < 0x20 || in[1 + i] >= 0x7F)
			return 0;
		name[i] = (char)in[1 + i];
	}
	name[in[0]] = '\0';
	return 1u + in[0];
}

/* A sub-audio tone: type in bits 15-14 (1 CTCSS, 2 DCS, 3 DCS inverted), the value in 13-0 */
static uint16_t tone_word(const subtone_t *t)
{
	return (uint16_t)((t->type << 14) | (t->value & 0x3FFFu));
}

static bool tone_back(uint16_t w, subtone_t *t)
{
	t->type = (uint8_t)(w >> 14);
	t->value = (uint16_t)(w & 0x3FFFu);
	if (t->type == TONE_CTCSS)
		return t->value >= 600u && t->value <= 2600u;
	return t->type != TONE_NONE && t->value <= 0777u;
}

uint8_t dota_power_class(uint8_t power, uint8_t power_count)
{
	if (power == CODEPLUG_POWER_DEFAULT || power_count == 0)
		return 0;
	if (power == 0)
		return 1;
	return power + 1u >= power_count ? 3 : 2;
}

uint8_t dota_power_level(uint8_t cls, uint8_t power_count)
{
	if (cls == 0 || power_count == 0)
		return CODEPLUG_POWER_DEFAULT;
	return cls == 1 ? 0 : cls == 3 ? (uint8_t)(power_count - 1u) : (uint8_t)(power_count / 2u);
}

static uint8_t mode_code(uint8_t mode)
{
	switch (mode) {
	case RADIO_MODE_DMR: return M_DMR;
#if CONFIG_P25_CHANNELS
	case RADIO_MODE_P25: return M_P25;
	case RADIO_MODE_YSF: return M_YSF;
	case RADIO_MODE_NXDN: return M_NXDN;
	case RADIO_MODE_DPMR: return M_DPMR;
#endif
	default: return M_FM;
	}
}

static bool mode_back(uint8_t code, uint8_t *mode)
{
	static const uint8_t modes[M_COUNT] = {
		RADIO_MODE_FM, RADIO_MODE_DMR,
#if CONFIG_P25_CHANNELS
		RADIO_MODE_P25, CONFIG_YSF ? RADIO_MODE_YSF : 0xFFu, CONFIG_NXDN ? RADIO_MODE_NXDN : 0xFFu,
		CONFIG_DPMR ? RADIO_MODE_DPMR : 0xFFu,
#else
		0xFFu, 0xFFu, 0xFFu, 0xFFu,
#endif
	};
	if (code >= M_COUNT || modes[code] == 0xFFu)
		return false;	/* a mode this build lacks */
	*mode = modes[code];
	return true;
}

size_t dota_rec_put_channel(const dota_ch_t *c, uint8_t power_count, uint8_t *out, size_t max)
{
	uint8_t b[DOTA_CH_MAX];
	const channel_t *ch = &c->ch;
	uint8_t m = mode_code(ch->mode);
	size_t n = 6;
	uint8_t a = (uint8_t)(m | (ch->bw == RADIO_BW_25K ? A_WIDE : 0u));
	uint8_t f = (uint8_t)(dota_power_class(ch->power, power_count) | (ch->rx_only ? B_RXONLY : 0u) |
			      (ch->scan_skip ? B_SKIP : 0u) | (ch->no_beep ? B_NOBEEP : 0u) | (ch->no_eco ? B_NOECO : 0u) |
			      (ch->vox ? B_VOX : 0u));
	put32(b + 2, ch->rx_hz);
	int32_t off = (int32_t)(ch->tx_hz - ch->rx_hz);
	if (ch->tx_hz != ch->rx_hz) {
		if (off % 25 == 0 && off / 25 >= -0x7FFFFF && off / 25 <= 0x7FFFFF) {
			a |= 1u << A_TX_SHIFT;
			put24(b + n, (uint32_t)(off / 25) & 0xFFFFFFu);
			n += 3;
		} else {
			a |= 2u << A_TX_SHIFT;
			put32(b + n, ch->tx_hz);
			n += 4;
		}
	}
	if (ch->rx_tone.type != TONE_NONE && (m == M_FM)) {
		a |= A_RXT;
		put16(b + n, tone_word(&ch->rx_tone));
		n += 2;
	}
	if (ch->tx_tone.type != TONE_NONE && (m == M_FM)) {
		a |= A_TXT;
		put16(b + n, tone_word(&ch->tx_tone));
		n += 2;
	}
	uint8_t x = (uint8_t)((ch->dmo ? X_DMO : 0u) | (ch->tot_s ? X_TOT : 0u) | (ch->sfr_flag1 ? X_SFR : 0u) |
			      ((ch->alias_tx & 0x0Fu) << X_ALIAS_SHIFT));
	if (x != 0) {
		f |= B_EXTRA;
		b[n++] = x;
		if (ch->tot_s)
			b[n++] = (uint8_t)((ch->tot_s + 14u) / 15u > 255u ? 255u : (ch->tot_s + 14u) / 15u);
		if (ch->sfr_flag1) {
			b[n++] = ch->sfr_flag1;
			b[n++] = ch->sfr_a;
			b[n++] = ch->sfr_b;
		}
	}
	switch (m) {
	case M_DMR: {
		bool list = c->list != DOTA_LIST_NONE;
		uint8_t ts = ch->timeslot >= 1u && ch->timeslot <= 3u ? ch->timeslot : 1u;
		b[n++] = (uint8_t)((ch->color_code & D_CC) | (ts << D_TS_SHIFT) | (list ? D_LIST : 0u));
		b[n++] = c->ct_type & 3u;
		if (c->ct_type != DOTA_CT_NONE) {
			put24(b + n, c->ct_id);
			n += 3;
		}
		if (list)
			b[n++] = c->list;
		break;
	}
#if CONFIG_P25_CHANNELS
	case M_P25:
		put16(b + n, ch->p25_nac);
		put16(b + n + 2, ch->p25_tx_nac_override);
		put16(b + n + 4, ch->p25_tg);
		b[n + 6] = ch->p25_squelch;
		n += 7;
		break;
	case M_YSF:
	case M_NXDN:
	case M_DPMR:
		b[n++] = (uint8_t)ch->p25_tg;
		break;
#endif
	default:
		break;
	}
	b[0] = a;
	b[1] = f;
	n += put_name(ch->name, b + n);
	if (n > max)
		return 0;
	memcpy(out, b, n);
	return n;
}

size_t dota_rec_get_channel(const uint8_t *in, size_t n, uint8_t power_count, dota_ch_t *c)
{
	if (n < 7u)
		return 0;
	memset(c, 0, sizeof *c);
	channel_t *ch = &c->ch;
	uint8_t a = in[0], f = in[1];
	uint8_t m = a & A_MODE;
	if (!mode_back(m, &ch->mode))
		return 0;
	ch->bw = (a & A_WIDE) ? RADIO_BW_25K : RADIO_BW_12K5;
	ch->rx_hz = get32(in + 2);
	ch->tx_hz = ch->rx_hz;
	size_t k = 6;
	uint8_t tx = (a >> A_TX_SHIFT) & 3u;
	if (tx == 1u) {
		if (n < k + 3u)
			return 0;
		int32_t steps = (int32_t)(get24(in + k) << 8) >> 8;	/* signed 24 bit */
		ch->tx_hz = (uint32_t)((int64_t)ch->rx_hz + (int64_t)steps * 25);
		k += 3;
	} else if (tx == 2u) {
		if (n < k + 4u)
			return 0;
		ch->tx_hz = get32(in + k);
		k += 4;
	} else if (tx == 3u) {
		return 0;
	}
	if (a & A_RXT) {
		if (n < k + 2u || m != M_FM || !tone_back(get16(in + k), &ch->rx_tone))
			return 0;
		k += 2;
	}
	if (a & A_TXT) {
		if (n < k + 2u || m != M_FM || !tone_back(get16(in + k), &ch->tx_tone))
			return 0;
		k += 2;
	}
	ch->power = dota_power_level(f & B_POWER, power_count);
	ch->rx_only = (f & B_RXONLY) != 0;
	ch->scan_skip = (f & B_SKIP) != 0;
	ch->no_beep = (f & B_NOBEEP) != 0;
	ch->no_eco = (f & B_NOECO) != 0;
	ch->vox = (f & B_VOX) != 0;
	ch->color_code = 1;
	ch->timeslot = 1;
	if (f & B_EXTRA) {
		if (n < k + 1u)
			return 0;
		uint8_t x = in[k++];
		if (x & 0x80u)
			return 0;
		ch->dmo = (x & X_DMO) != 0;
		ch->alias_tx = (uint8_t)((x >> X_ALIAS_SHIFT) & 0x0Fu);
		if (x & X_TOT) {
			if (n < k + 1u || in[k] == 0)
				return 0;
			ch->tot_s = (uint16_t)(in[k++] * 15u);
		}
		if (x & X_SFR) {
			if (n < k + 3u || in[k] == 0)
				return 0;
			ch->sfr_flag1 = in[k];
			ch->sfr_a = in[k + 1];
			ch->sfr_b = in[k + 2];
			k += 3;
		}
	}
	switch (m) {
	case M_DMR: {
		if (n < k + 2u)
			return 0;
		uint8_t d = in[k++];
		ch->color_code = d & D_CC;
		ch->timeslot = (uint8_t)((d >> D_TS_SHIFT) & 3u);
		if (ch->color_code > 16u || ch->timeslot == 0 || (!CONFIG_DMR_ANY && (ch->color_code > 15u || ch->timeslot > 2u)))
			return 0;
		c->ct_type = in[k++];
		if (c->ct_type > DOTA_CT_ALL)
			return 0;
		if (c->ct_type != DOTA_CT_NONE) {
			if (n < k + 3u)
				return 0;
			c->ct_id = get24(in + k);
			k += 3;
			if (c->ct_id == 0)
				return 0;
		}
		if (d & D_LIST) {
			if (n < k + 1u || in[k] == DOTA_LIST_NONE)
				return 0;
			c->list = in[k++];
		}
		break;
	}
#if CONFIG_P25_CHANNELS
	case M_P25:
		if (n < k + 7u)
			return 0;
		ch->p25_nac = get16(in + k);
		ch->p25_tx_nac_override = get16(in + k + 2);
		ch->p25_tg = get16(in + k + 4);
		ch->p25_squelch = in[k + 6];
		k += 7;
		if (ch->p25_nac > 0xFFFu || ch->p25_tx_nac_override > 0x1000u)
			return 0;
		break;
	case M_YSF:
	case M_NXDN:
	case M_DPMR:
		if (n < k + 1u)
			return 0;
		ch->p25_tg = in[k++];
		break;
#endif
	default:
		break;
	}
	size_t l = get_name(in + k, n - k, ch->name);
	if (l == 0 || ch->rx_hz == 0 || ch->tx_hz == 0 || ch->rx_hz >= 1000000000u || ch->tx_hz >= 1000000000u)
		return 0;
	return k + l;
}

/* --- Contacts: ID (3), type (bits 1-0) and timeslot override (bits 3-2), name --- */

size_t dota_rec_put_contact(const contact_t *c, uint8_t *out, size_t max)
{
	uint8_t b[DOTA_CT_MAX];
	put24(b, c->id);
	b[3] = (uint8_t)((c->type & 3u) | ((c->timeslot <= 2u ? c->timeslot : 0u) << 2));
	size_t n = 4 + put_name(c->name, b + 4);
	if (n > max)
		return 0;
	memcpy(out, b, n);
	return n;
}

size_t dota_rec_get_contact(const uint8_t *in, size_t n, contact_t *c)
{
	if (n < 5u)
		return 0;
	memset(c, 0, sizeof *c);
	c->id = get24(in);
	c->type = in[3] & 3u;
	c->timeslot = (uint8_t)((in[3] >> 2) & 3u);
	if (c->id == 0 || c->type > CONTACT_ALL_CALL || c->timeslot > 2u || (in[3] & 0xF0u))
		return 0;
	size_t l = get_name(in + 4, n - 4, c->name);
	return l ? 4 + l : 0;
}

/* --- RX group lists: count, name, private bits (4), IDs (3 each) --- */

size_t dota_rec_put_list(const dota_list_t *l, uint8_t *out, size_t max)
{
	uint8_t count = l->count <= CODEPLUG_LIST_LEN ? l->count : CODEPLUG_LIST_LEN;
	size_t n = 1;
	uint8_t b[DOTA_LIST_MAX];
	b[0] = count;
	n += put_name(l->name, b + n);
	put32(b + n, l->private_bits);
	n += 4;
	for (uint8_t i = 0; i < count; i++) {
		put24(b + n, l->id[i]);
		n += 3;
	}
	if (n > max)
		return 0;
	memcpy(out, b, n);
	return n;
}

size_t dota_rec_get_list(const uint8_t *in, size_t n, dota_list_t *l)
{
	if (n < 1u)
		return 0;
	memset(l, 0, sizeof *l);
	l->count = in[0];
	if (l->count > CODEPLUG_LIST_LEN)
		return 0;
	size_t k = get_name(in + 1, n - 1, l->name);
	if (k == 0 || n < 1u + k + 4u + 3u * l->count)
		return 0;
	k += 1;
	l->private_bits = get32(in + k);
	k += 4;
	for (uint8_t i = 0; i < l->count; i++, k += 3) {
		l->id[i] = get24(in + k);
		if (l->id[i] == 0)
			return 0;
	}
	if (l->count < 32u && (l->private_bits >> l->count) != 0)
		return 0;
	return k;
}

/* --- Zone parts: zone, part (bit 7: the name follows), total, offset, [name], runs, (start 2, count 1)... --- */

size_t dota_rec_put_zone(const dota_zone_t *z, uint8_t *out, size_t max)
{
	uint8_t b[DOTA_ZONE_MAX];
	size_t n = 0;
	if (z->runs > DOTA_ZONE_RUNS)
		return 0;
	b[n++] = z->zone;
	b[n++] = (uint8_t)((z->part & 0x7Fu) | (z->part == 0 ? 0x80u : 0u));
	b[n++] = z->total;
	b[n++] = z->offset;
	if (z->part == 0)
		n += put_name(z->name, b + n);
	b[n++] = z->runs;
	for (uint8_t i = 0; i < z->runs; i++) {
		put16(b + n, z->start[i]);
		b[n + 2] = z->len[i];
		n += 3;
	}
	if (n > max)
		return 0;
	memcpy(out, b, n);
	return n;
}

size_t dota_rec_get_zone(const uint8_t *in, size_t n, dota_zone_t *z)
{
	if (n < 5u)
		return 0;
	memset(z, 0, sizeof *z);
	z->zone = in[0];
	z->part = in[1] & 0x7Fu;
	bool named = (in[1] & 0x80u) != 0;
	z->total = in[2];
	z->offset = in[3];
	size_t k = 4;
	if (named != (z->part == 0) || z->total == 0 || z->total > CODEPLUG_ZONE_MAX || z->offset >= z->total)
		return 0;
	if (named) {
		size_t l = get_name(in + k, n - k, z->name);
		if (l == 0)
			return 0;
		k += l;
	}
	if (n < k + 1u)
		return 0;
	z->runs = in[k++];
	if (z->runs == 0 || z->runs > DOTA_ZONE_RUNS || n < k + 3u * z->runs)
		return 0;
	uint32_t members = z->offset;
	for (uint8_t i = 0; i < z->runs; i++, k += 3) {
		z->start[i] = get16(in + k);
		z->len[i] = in[k + 2];
		members += z->len[i];
		if (z->len[i] == 0 || z->start[i] + (uint32_t)z->len[i] > CODEPLUG_CHANNELS_MAX)
			return 0;
	}
	return members <= z->total ? k : 0;
}
#endif
