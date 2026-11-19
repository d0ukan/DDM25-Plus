// SPDX-License-Identifier: GPL-3.0-or-later
// DM32 Plus: every mode in the VFO (vfox.h, docs/plus.md "Every mode in the VFO")
#include "config.h"
#if CONFIG_VFO_MODES
#include <string.h>
#include "radio/vfox.h"
#include "radio/radio_am.h"

#define P25_ID_MAX	9999999u	// codeplug_p25_valid's limit
#define NAC_ANY		0xF7Eu
#define NAC_BAD		0xF7Fu

_Static_assert(sizeof(vfox_slot_t) == 32, "vfox_slot_t: 32 bytes a ring letter");

uint8_t vfox_mode_of(const channel_t *c)
{
	return c->am && c->mode == RADIO_MODE_FM ? (uint8_t)VFOX_AM : c->mode;
}

bool vfox_mode_built(uint8_t m)
{
	switch (m) {
	case RADIO_MODE_FM: return CONFIG_VFO_FM;
	case RADIO_MODE_DMR: return CONFIG_VFO_DMR;
	case RADIO_MODE_P25: return CONFIG_P25_NATIVE;
	case RADIO_MODE_YSF: return CONFIG_YSF;
	case RADIO_MODE_NXDN: return CONFIG_NXDN;
	case RADIO_MODE_DPMR: return CONFIG_DPMR;
	case VFOX_AM: return CONFIG_VFO_FM && radio_am_capable();
	default: return false;
	}
}

bool vfox_native(uint8_t m)
{
	return m == RADIO_MODE_P25 || m == RADIO_MODE_YSF || m == RADIO_MODE_NXDN || m == RADIO_MODE_DPMR;
}

uint8_t vfox_mode_next(const channel_t *c)
{
	uint8_t m = vfox_mode_of(c);
	for (uint8_t i = 1; i < VFOX_MODES; i++) {
		uint8_t n = (uint8_t)((m + i) % VFOX_MODES);
		if (vfox_mode_built(n))
			return n;
	}
	return m;
}

const char *vfox_mode_name(uint8_t m)
{
	static const char *const names[VFOX_MODES] = { "FM", "DMR", "P25", "YSF", "NXDN", "dPMR", "AM" };
	return m < VFOX_MODES ? names[m] : "FM";
}

// The DMR fields cleared (contact, RX group list, own ID: the VFO has its own talk target), "every
// talkgroup" kept where the build has it
static void dmr_union(channel_t *c, bool keep_any)
{
	bool any = CONFIG_DMR_ANY && keep_any && c->listen_list == CODEPLUG_LIST_ANY;
	c->contact = 0;
	c->dmr_id = 0;
	c->listen_list = any ? CODEPLUG_LIST_ANY : 0;
}

void vfox_fix(channel_t *c)
{
	if (c->mode != RADIO_MODE_FM || !vfox_mode_built(VFOX_AM))
		c->am = false;
#if CONFIG_P25_TXNAC
	// DM32 Plus: a TX NAC of its own only on P25, and never Any / $F7F (RAM only: the settings record has no room for
	// it). channel_t.p25_tx_nac_override as Classic r13 keeps it: 0 = Same as RX, else the TX NAC + 1
	if (c->mode != RADIO_MODE_P25 || c->p25_tx_nac_override > 0x1000u ||
	    (c->p25_tx_nac_override && ((c->p25_tx_nac_override - 1u) & 0xFFEu) == 0xF7Eu))
		c->p25_tx_nac_override = 0;
#else
	c->p25_tx_nac_override = 0;
#endif
	if (!vfox_native(c->mode)) {
		dmr_union(c, true);
		c->rx_only = false;	// the VFO transmits where the band and the mode let it (radio_sm_tx_check)
		if (c->mode == RADIO_MODE_DMR) {
			if (c->color_code > 15 && !(CONFIG_DMR_ANY && c->color_code == CODEPLUG_CC_ANY))
				c->color_code = 1;
			if (c->timeslot != 1 && c->timeslot != 2 && !(CONFIG_DMR_ANY && c->timeslot == CODEPLUG_TS_ANY))
				c->timeslot = 1;
		}
		return;
	}
	c->bw = RADIO_BW_12K5;
	if (c->mode == RADIO_MODE_P25) {
		if (c->p25_nac > 0xFFFu || c->p25_nac == NAC_BAD)
			c->p25_nac = NAC_ANY;
		if (c->p25_id > P25_ID_MAX)
			c->p25_id = 0;
		// The talkgroup filters (Selective; talkgroup 0 = Any), as on the VFO before Classic r13's squelch setting
		c->p25_squelch = 1;
		// A P25 channel's TX rule (codeplug_p25_valid): a TX NAC, a talkgroup and an ID, else it only listens. Classic
		// r13's TX NAC: Same as RX inherits the RX NAC, $293 when that is Any (codeplug_p25_tx_nac)
		c->rx_only = codeplug_p25_tx_nac(c) == NAC_ANY || c->p25_tg == 0 || c->p25_id == 0;
		return;
	}
	uint16_t top = c->mode == RADIO_MODE_YSF ? 127u : c->mode == RADIO_MODE_NXDN ? 63u : 64u;
	c->p25_nac = c->mode == RADIO_MODE_YSF ? NAC_ANY : 0;	// as the codeplug decodes these modes
	c->p25_id = 0;
	if (c->p25_tg > top)
		c->p25_tg = 0;
	c->rx_only = true;	// YSF, NXDN and dPMR are receive only
}

void vfox_set_mode(channel_t *c, uint8_t m, uint32_t own_id)
{
	c->contact = 0;
	c->listen_list = 0;
	c->dmr_id = 0;
	c->am = false;
	switch (m) {
	case VFOX_AM:
		c->mode = RADIO_MODE_FM;
		c->am = true;
		break;
	case RADIO_MODE_P25:
		c->mode = RADIO_MODE_P25;
		c->p25_nac = NAC_ANY;	// listen to everything first; the VFO settings set a NAC and a talkgroup to talk
		c->p25_tx_nac_override = 0;
		c->p25_tg = 0;
		c->p25_id = own_id <= P25_ID_MAX ? own_id : 0;
		break;
	case RADIO_MODE_YSF:
		c->mode = RADIO_MODE_YSF;
		c->p25_nac = NAC_ANY;
		break;
	case RADIO_MODE_DMR:
	case RADIO_MODE_NXDN:
	case RADIO_MODE_DPMR:
		c->mode = m;
		break;
	default:
		c->mode = RADIO_MODE_FM;
		break;
	}
	vfox_fix(c);
}

void vfox_copy(channel_t *vfo, const channel_t *ch)
{
	vfo->am = false;
	dmr_union(vfo, false);
	if (vfox_native(ch->mode)) {
		vfo->mode = ch->mode;
		vfo->p25_tg = ch->p25_tg;
		vfo->p25_nac = ch->p25_nac;
		vfo->p25_id = ch->p25_id;
		vfo->p25_tx_nac_override = ch->p25_tx_nac_override;
	} else if (CONFIG_DMR_ANY && ch->mode == RADIO_MODE_DMR) {
		// radio_sm.c vfo_copy keeps the low nibble and slot 1 / 2 only: the channel's "any" come along
		if (ch->color_code == CODEPLUG_CC_ANY)
			vfo->color_code = CODEPLUG_CC_ANY;
		if (ch->timeslot == CODEPLUG_TS_ANY && vfo->timeslot == 1)
			vfo->timeslot = CODEPLUG_TS_ANY;
		if (ch->listen_list == CODEPLUG_LIST_ANY)
			vfo->listen_list = CODEPLUG_LIST_ANY;
	}
	vfox_fix(vfo);
}

// ---- The settings record (docs/plus.md "Every mode in the VFO": the byte map) ----

// VFO idx's fields: the mode in bits 0-3 of its mode byte (VFO 1's bits 4-7 are the talk target's), the color code in
// bits 0-3 of its cc byte (the same), the timeslot in bits 0-1 of its ts byte
typedef struct {
	uint8_t *mode, *cc, *ts, *rx_type, *tx_type;
	uint16_t *rx_tone, *tx_tone;
} slot_t;

static void slot_of(settings_t *st, uint8_t idx, slot_t *s)
{
	if (idx == 0) {
		*s = (slot_t){ &st->vfo_mode, &st->vfo_cc, &st->vfo_ts, &st->vfo_rx_tone_type, &st->vfo_tx_tone_type,
			       &st->vfo_rx_tone, &st->vfo_tx_tone };
	} else {
		*s = (slot_t){ &st->vfo2_mode, &st->vfo2_cc, &st->vfo2_ts, &st->vfo2_rx_tone_type, &st->vfo2_tx_tone_type,
			       &st->vfo2_rx_tone, &st->vfo2_tx_tone };
	}
}

// DMR's Any flags. VFO 1: settings_t.unused_dmr_id (upstream reads it nowhere; an older record may hold an own DMR ID
// there, under 2^24: ours carry VFOX_EXT_MAGIC in the top byte). VFO 2: vfo2_talk bits 24-26 (upstream reads bits
// 0-23 and 31 only, and wrote 0 there)
static uint8_t ext_get(const settings_t *st, uint8_t idx)
{
	if (idx == 0)
		return (st->unused_dmr_id & 0xFF000000u) == VFOX_EXT_MAGIC ?
		       (uint8_t)((st->unused_dmr_id >> VFOX_EXT_SHIFT) & 7u) : 0;
	return (uint8_t)((st->vfo2_talk >> 24) & 7u);
}

static void ext_set(settings_t *st, uint8_t idx, uint8_t f)
{
	if (idx != 0) {
		st->vfo2_talk = (st->vfo2_talk & ~0x07000000u) | ((uint32_t)(f & 7u) << 24);
	} else if ((st->unused_dmr_id & 0xFF000000u) == VFOX_EXT_MAGIC) {
		// the Plus word: the priority channel (scanx, bits 0-10 and 16-22) stays; nothing left: 0
		uint32_t w = (st->unused_dmr_id & ~VFOX_EXT_BITS) | ((uint32_t)(f & 7u) << VFOX_EXT_SHIFT);
		st->unused_dmr_id = w == VFOX_EXT_MAGIC ? 0u : w;
	} else if (f != 0) {
		st->unused_dmr_id = VFOX_EXT_MAGIC | ((uint32_t)(f & 7u) << VFOX_EXT_SHIFT);	// an old own ID there goes
	}
}

void vfox_store(settings_t *st, uint8_t idx, const channel_t *v)
{
	slot_t s;
	slot_of(st, idx, &s);
	uint8_t m = vfox_mode_of(v);
	*s.mode = (uint8_t)((*s.mode & 0xF0u) | (m & 0x0Fu));
	if (!vfox_native(m)) {
		uint8_t f = 0;
		if (CONFIG_DMR_ANY) {
			f |= v->color_code == CODEPLUG_CC_ANY ? VFOX_EXT_ANY_CC : 0;
			f |= v->timeslot == CODEPLUG_TS_ANY ? VFOX_EXT_ANY_TS : 0;
			f |= v->listen_list == CODEPLUG_LIST_ANY ? VFOX_EXT_ANY_TG : 0;
		}
		ext_set(st, idx, f);
		return;
	}
	// A native mode: its 52 bits in the FM tone fields (both type bytes marked: upstream reads no tone there), the
	// color code nibble and the timeslot's two bits. NAC 12 bits, talkgroup / DG-ID / RAN / CC 16, unit ID 24
	uint32_t id = v->p25_id & 0xFFFFFFu;
	uint16_t nac = (uint16_t)(v->p25_nac & 0xFFFu);
	ext_set(st, idx, 0);
	*s.rx_tone = v->p25_tg;
	*s.tx_tone = (uint16_t)id;
	*s.rx_type = (uint8_t)(VFOX_NATIVE_MARK | ((id >> 16) & 0x7Fu));
	*s.tx_type = (uint8_t)(VFOX_NATIVE_MARK | (nac & 0x7Fu));
	*s.cc = (uint8_t)((*s.cc & 0xF0u) | ((nac >> 7) & 0x0Fu));
	*s.ts = (uint8_t)((*s.ts & 0xFCu) | ((id >> 23) & 1u) | (((nac >> 11) & 1u) << 1));
}

void vfox_load(const settings_t *st, uint8_t idx, channel_t *v)
{
	slot_t s;
	slot_of((settings_t *)st, idx, &s);	// read only
	uint8_t m = *s.mode & 0x0Fu;
	if ((m == RADIO_MODE_DMR || m == RADIO_MODE_FM || m == VFOX_AM) && CONFIG_DMR_ANY) {
		uint8_t f = ext_get(st, idx);
		if (f & VFOX_EXT_ANY_CC)
			v->color_code = CODEPLUG_CC_ANY;
		if (f & VFOX_EXT_ANY_TS)
			v->timeslot = CODEPLUG_TS_ANY;
		if (f & VFOX_EXT_ANY_TG)
			v->listen_list = CODEPLUG_LIST_ANY;
	}
	if (m == VFOX_AM && v->mode == RADIO_MODE_FM && vfox_mode_built(VFOX_AM)) {
		v->am = true;
	} else if (vfox_native(m) && vfox_mode_built(m) && (*s.rx_type & VFOX_NATIVE_MARK) &&
		   (*s.tx_type & VFOX_NATIVE_MARK)) {
		v->mode = m;
		v->p25_tg = *s.rx_tone;
		v->p25_id = *s.tx_tone | ((uint32_t)(*s.rx_type & 0x7Fu) << 16) | ((uint32_t)(*s.ts & 1u) << 23);
		v->p25_nac = (uint16_t)((*s.tx_type & 0x7Fu) | ((*s.cc & 0x0Fu) << 7) | (((*s.ts >> 1) & 1u) << 11));
		v->color_code = 1;	// the DMR fields held the parameters: the defaults for a later DMR
		v->timeslot = 1;
		v->rx_tone = v->tx_tone = (subtone_t){ TONE_NONE, 0 };
	}
	vfox_fix(v);
}

// ---- The ring letters' own VFOs (multi-watch) ----

void vfox_slot_put(vfox_slot_t *s, const channel_t *c, uint32_t talk, bool seeded)
{
	// A native mode's talkgroup, NAC and unit ID, else the DMR contact, RX group list and own ID (one union in a
	// Classic r7 channel_t, separate fields since r13)
	bool native = vfox_native(c->mode);
	*s = (vfox_slot_t){ .rx_hz = c->rx_hz, .tx_hz = c->tx_hz, .id = native ? c->p25_id : c->dmr_id, .talk = talk,
			    .tg = native ? c->p25_tg : c->contact, .nac = native ? c->p25_nac : c->listen_list, .rx_tone = c->rx_tone.value, .tx_tone = c->tx_tone.value,
			    .rx_type = c->rx_tone.type, .tx_type = c->tx_tone.type,
			    .mode = (uint8_t)(vfox_mode_of(c) | (seeded ? 0x80u : 0)), .bw = c->bw, .power = c->power,
			    .cc = c->color_code, .ts = c->timeslot };
#if CONFIG_P25_TXNAC
	// DM32 Plus: the TX NAC's 13 bits, 0-7 in the spare byte, 8-12 above the bandwidth (0 or 1) in its byte
	s->spare = (uint8_t)c->p25_tx_nac_override;
	s->bw = (uint8_t)((c->bw & 7u) | ((c->p25_tx_nac_override >> 8) << 3));
#endif
}

void vfox_slot_get(const vfox_slot_t *s, channel_t *c, uint32_t *talk, bool *seeded)
{
	memset(c, 0, sizeof *c);
	strcpy(c->name, "VFO");
	*seeded = (s->mode & 0x80u) != 0;
	*talk = *seeded ? s->talk : 0;
	if (!*seeded) {	// radio_sm.c vfo_defaults
		c->mode = RADIO_MODE_FM;
		c->bw = RADIO_BW_12K5;
		c->power = CODEPLUG_POWER_DEFAULT;
		c->color_code = 1;
		c->timeslot = 1;
		return;
	}
	uint8_t m = s->mode & 0x0Fu;
	c->rx_hz = s->rx_hz;
	c->tx_hz = s->tx_hz;
	c->mode = m == VFOX_AM ? RADIO_MODE_FM : m;
	c->am = m == VFOX_AM;
	c->bw = s->bw;
#if CONFIG_P25_TXNAC
	c->bw = s->bw & 7u;
	c->p25_tx_nac_override = (uint16_t)(s->spare | ((s->bw >> 3) << 8));
#endif
	c->power = s->power;
	c->color_code = s->cc;
	c->timeslot = s->ts;
	c->rx_tone = (subtone_t){ s->rx_type, s->rx_tone };
	c->tx_tone = (subtone_t){ s->tx_type, s->tx_tone };
	if (vfox_native(c->mode)) {
		c->p25_tg = s->tg;
		c->p25_nac = s->nac;
		c->p25_id = s->id;
	} else {
		c->contact = s->tg;
		c->listen_list = s->nac;
		c->dmr_id = s->id;
	}
	vfox_fix(c);
}
#endif
