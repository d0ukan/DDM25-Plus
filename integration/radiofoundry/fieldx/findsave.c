/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM25 Plus search finds as channels (findsave.h): pure, the host tests run it as is. */
#include "config.h"
#if CONFIG_FIND_SAVE
#include <string.h>
#include "ui/findsave.h"

static char *put_u(char *p, uint32_t v, int min_digits)
{
	char d[10];
	int n = 0;
	do {
		d[n++] = (char)('0' + v % 10u);
		v /= 10u;
	} while (v != 0 && n < (int)sizeof d);
	while (n < min_digits && n < (int)sizeof d)
		d[n++] = '0';
	while (n > 0)
		*p++ = d[--n];
	return p;
}

void findsave_name(const findsave_item_t *f, char out[CODEPLUG_NAME_LEN + 1])
{
	char *p = out;
	memcpy(p, f->kind == FINDSAVE_DMR ? "DMR " : "P25 ", 4);
	p += 4;
	uint32_t hz = f->hz;
	p = put_u(p, hz / 1000000u, 1);
	*p++ = '.';
	uint32_t frac = (hz / 100u) % 10000u;	/* 100 Hz units */
	if (frac % 10u == 0u)
		p = put_u(p, frac / 10u, 3);
	else
		p = put_u(p, frac, 4);
	*p = '\0';
}

/* The channel editor's outside-band rule (ui_menus.c ced_outside_ham): 2 m and 70 cm are the amateur bands */
static bool ham(uint32_t hz)
{
	return (hz >= 144000000u && hz <= 148000000u) || (hz >= 420000000u && hz <= 450000000u);
}

bool findsave_p25_tx(const findsave_item_t *f, uint32_t own_p25_id, bool tx_band)
{
	return f->kind == FINDSAVE_P25 && f->tg != 0u && own_p25_id != 0u && own_p25_id <= 0xFFFFFEu &&
	       f->nac <= 0xFFFu && f->nac != 0xF7Eu && f->nac != 0xF7Fu && ham(f->hz) && tx_band;
}

void findsave_channel(const findsave_item_t *f, uint32_t own_p25_id, bool tx_band, channel_t *out)
{
	memset(out, 0, sizeof *out);
	findsave_name(f, out->name);
	out->rx_hz = out->tx_hz = f->hz;
	out->bw = RADIO_BW_12K5;
	out->power = CODEPLUG_POWER_DEFAULT;
	out->rx_only = true;
	out->color_code = 1;
	out->timeslot = 1;
#if CONFIG_P25_CHANNELS
	out->system = 0xFFu;
	if (f->kind == FINDSAVE_P25) {
		out->mode = RADIO_MODE_P25;
		out->kind = CODEPLUG_KIND_P25;
		out->p25_nac = f->nac;
		out->p25_tx_nac_override = 0;	/* TX NAC: Same as RX */
		out->p25_squelch = 0;		/* Normal: every talkgroup on the NAC */
		out->p25_tg = f->tg;		/* the TX talkgroup, 0 = none heard */
		if (findsave_p25_tx(f, own_p25_id, tx_band)) {
			out->p25_id = own_p25_id;	/* the radio's own (dmr_id 0: "Default") */
			out->rx_only = false;
		}
		return;
	}
#else
	(void)own_p25_id;
	(void)tx_band;
#endif
	out->mode = RADIO_MODE_DMR;
	if (f->cc <= 15u)
		out->color_code = f->cc;
	else
		out->color_code = CONFIG_DMR_ANY ? CODEPLUG_CC_ANY : 1u;
	uint8_t ts = f->ts_mask & 3u;
	if (ts == 1u || ts == 2u)
		out->timeslot = ts;
	else
		out->timeslot = CONFIG_DMR_ANY ? CODEPLUG_TS_ANY : 1u;
	out->listen_list = CONFIG_DMR_ANY ? CODEPLUG_LIST_ANY : 0u;
	out->contact = 0;	/* no TX target known: RX only */
}

bool findsave_same(const channel_t *ch, const findsave_item_t *f)
{
	if (ch->rx_hz != f->hz)
		return false;
#if CONFIG_P25_CHANNELS
	if (f->kind == FINDSAVE_P25)
		return ch->mode == RADIO_MODE_P25 && ch->p25_nac == f->nac;
#endif
	if (f->kind != FINDSAVE_DMR || ch->mode != RADIO_MODE_DMR)
		return false;
	uint8_t want = f->cc <= 15u ? f->cc : (CONFIG_DMR_ANY ? CODEPLUG_CC_ANY : 1u);
	return ch->color_code == want;
}

uint16_t findsave_zone_number(const char *name)
{
	size_t n = sizeof FINDSAVE_ZONE_BASE - 1u;
	if (strncmp(name, FINDSAVE_ZONE_BASE, n) != 0 || name[n] != ' ')
		return 0;
	const char *p = name + n + 1;
	uint32_t v = 0;
	if (*p < '1' || *p > '9')
		return 0;
	for (; *p >= '0' && *p <= '9'; p++) {
		v = v * 10u + (uint32_t)(*p - '0');
		if (v > FINDSAVE_ZONE_NUM_MAX)
			return 0;
	}
	return *p == '\0' ? (uint16_t)v : 0u;
}

void findsave_zone_name(uint16_t n, char out[CODEPLUG_NAME_LEN + 1])
{
	size_t k = sizeof FINDSAVE_ZONE_BASE - 1u;
	memcpy(out, FINDSAVE_ZONE_BASE, k);
	char *p = out + k;
	*p++ = ' ';
	p = put_u(p, n, 1);
	*p = '\0';
}
#endif
