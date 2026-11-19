/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus YSF Voice FR transmit framing (ysf_tx.h). */
#include <string.h>
#include "ysf_tx.h"
#include "ysf_vw.h"
#include "p25.h"

static inline unsigned get_bit(const uint8_t *p, unsigned i) { return (p[i >> 3] >> (7u - (i & 7u))) & 1u; }
static inline void put_bit(uint8_t *p, unsigned i, unsigned b)
{
	uint8_t m = (uint8_t)(0x80u >> (i & 7u));
	p[i >> 3] = b ? (uint8_t)(p[i >> 3] | m) : (uint8_t)(p[i >> 3] & ~m);
}

void ysf_tx_fich_raw(uint8_t fi, uint8_t fn, uint8_t ft, uint8_t dgid, uint8_t raw[4])
{
	raw[0] = (uint8_t)(((fi & 3u) << 6) | (2u << 4));	/* FI, CS 2, CM 0 (group), BN 0 */
	raw[1] = (uint8_t)(((fn & 7u) << 3) | (ft & 7u));	/* BT 0, FN, FT */
	raw[2] = (uint8_t)((YSF_TX_LOW_DEV ? 0x40u : 0u) | YSF_DT_VOICE_FR);	/* Dev, MR 0, VoIP 0, DT */
	raw[3] = (uint8_t)(dgid & 0x7Fu);			/* SQL 0, SQ = DG-ID */
}

/* IMBE frames the frame of a phase carries */
static unsigned voice_needed(uint8_t phase)
{
	return phase == YSF_TX_SUBHEADER ? 2u : phase == YSF_TX_VOICE ? YSF_VW_SECTIONS : 0u;
}

static bool ready(const ysf_tx_t *t)
{
	return t->phase != YSF_TX_DONE && t->voice_in >= voice_needed(t->phase);
}

static void start_frame(ysf_tx_t *t, uint8_t phase)
{
	uint8_t raw[4], csd[YSF_CSD_BYTES];
	t->phase = phase;
	t->voice_in = 0;
	t->sent = 0;
	if (phase == YSF_TX_DONE)
		return;
	memset(t->frame, 0, YSF_FRAME_BYTES);
	switch (phase) {
	case YSF_TX_HEADER:
	case YSF_TX_TERMINATOR:
		ysf_tx_fich_raw(phase == YSF_TX_HEADER ? YSF_FI_HEADER : YSF_FI_TERMINATOR, 0, 1, t->dgid, raw);
		memset(csd, '*', 10);		/* destination: any radio (MMDVM_CM's "*****" + an unknown radio ID) */
		memcpy(csd + 10, t->call, 10);	/* source: our callsign */
		ysf_csd_put(csd, YSF_CSD1, t->frame);
		memset(csd, ' ', sizeof csd);	/* no downlink or uplink (direct) */
		ysf_csd_put(csd, YSF_CSD2, t->frame);
		break;
	case YSF_TX_SUBHEADER:
		ysf_tx_fich_raw(YSF_FI_COMMS, 0, 1, t->dgid, raw);
		memset(csd, ' ', 15);		/* rem1..rem3 blank, rem4 the radio ID: unknown (as MMDVM_CM DMR2YSF) */
		memset(csd + 15, '*', 5);
		ysf_csd_put(csd, YSF_CSD3, t->frame);	/* payload bytes 45..53 stay zero (reserved) */
		break;
	default:
		ysf_tx_fich_raw(YSF_FI_COMMS, 0, 0, t->dgid, raw);
		break;
	}
	ysf_fich_encode(raw, t->frame);
}

void ysf_tx_begin(ysf_tx_t *t, uint8_t frame[YSF_FRAME_BYTES], const char *call, uint8_t dgid)
{
	memset(t, 0, sizeof *t);
	t->frame = frame;
	t->dgid = (uint8_t)(dgid & 0x7Fu);
	memset(t->call, ' ', sizeof t->call);
	for (unsigned i = 0; call && i < sizeof t->call && call[i] != '\0'; i++)
		t->call[i] = (uint8_t)call[i];
	start_frame(t, YSF_TX_HEADER);
}

bool ysf_tx_need_voice(const ysf_tx_t *t)
{
	return t->voice_in < voice_needed(t->phase);
}

void ysf_tx_voice(ysf_tx_t *t, const uint16_t u[8])
{
	if (!ysf_tx_need_voice(t))
		return;
	uint16_t v[8];
	uint8_t cw[YSF_VW_IMBE_BYTES];
	memcpy(v, u, sizeof v);
	v[7] = (uint16_t)((v[7] & ~1u) | (t->imbe & 1u));	/* the IMBE sync bit alternates, as in a P25 LDU */
	p25_imbe_encode(v, cw);
	ysf_vw_section_put(cw, t->phase == YSF_TX_SUBHEADER ? 3u + t->voice_in : t->voice_in, t->frame);
	t->voice_in++;
	t->imbe++;
}

void ysf_tx_close(ysf_tx_t *t)
{
	t->closing = 1;
	if ((t->phase == YSF_TX_VOICE || t->phase == YSF_TX_SUBHEADER) && t->voice_in == 0)
		start_frame(t, YSF_TX_TERMINATOR);
}

unsigned ysf_tx_take(ysf_tx_t *t, uint8_t *out, unsigned at, unsigned n)
{
	unsigned k = 0;
	while (k < n && ready(t)) {
		while (k < n && t->sent < YSF_FRAME_BITS)
			put_bit(out, at + k++, get_bit(t->frame, t->sent++));
		if (t->sent < YSF_FRAME_BITS)
			break;
		t->frames++;
		switch (t->phase) {
		case YSF_TX_HEADER:
			start_frame(t, t->closing ? YSF_TX_TERMINATOR : YSF_TX_SUBHEADER);
			break;
		case YSF_TX_SUBHEADER:
		case YSF_TX_VOICE:
			start_frame(t, t->closing ? YSF_TX_TERMINATOR : YSF_TX_VOICE);
			break;
		default:
			start_frame(t, YSF_TX_DONE);
			break;
		}
	}
	return k;
}

bool ysf_tx_done(const ysf_tx_t *t)
{
	return t->phase == YSF_TX_DONE;
}
