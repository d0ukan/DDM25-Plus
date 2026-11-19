/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus DMR Tier III CSBK fields (dmr_csbk.h). Layouts from ETSI TS 102 361-4 (C_ALOHA, C_BCAST, the channel
 * grants, P_CLEAR) and the MBC absolute channel parameters as decoders print them. No RTOS, no hardware. */
#include "config.h"
#if CONFIG_DMR_TRUNK
#include <string.h>
#include "radio/dmr_csbk.h"

uint16_t dmr_csbk_crc(const uint8_t b[10])
{
	uint16_t crc = 0;
	for (unsigned i = 0; i < 10u; i++) {
		crc ^= (uint16_t)(b[i] << 8);
		for (unsigned k = 0; k < 8u; k++) {
			bool top = (crc & 0x8000u) != 0;
			crc = (uint16_t)(crc << 1);
			if (top)
				crc ^= 0x1021u;
		}
	}
	return (uint16_t)~crc;
}

bool dmr_csbk_crc_ok(const uint8_t b[12], uint16_t mask)
{
	uint16_t crc = (uint16_t)(dmr_csbk_crc(b) ^ mask);
	return b[10] == (uint8_t)(crc >> 8) && b[11] == (uint8_t)crc;
}

uint32_t dmr_csbk_bits(const uint8_t *b, unsigned pos, unsigned n)
{
	uint32_t v = 0;
	for (unsigned i = 0; i < n; i++, pos++)
		v = v << 1 | ((b[pos >> 3] >> (7u - (pos & 7u))) & 1u);
	return v;
}

void dmr_csbk_parse(const uint8_t b[10], dmr_csbk_t *out)
{
	memset(out, 0, sizeof *out);
	out->op = b[0] & 0x3Fu;
	out->last = (b[0] & 0x80u) != 0;
	out->protect = (b[0] & 0x40u) != 0;
	out->fid = b[1];
	switch (out->op) {
	case CSBKO_C_ALOHA:
		/* Reserved 2, TS sync 1, version 3, offset 1, active connection 1, mask 5, service function 2, NRand_Wait 4,
		 * Reg 1, Backoff 4, system identity code 16, MS address 24 */
		out->syscode = (uint16_t)dmr_csbk_bits(b, 40, 16);
		break;
	case CSBKO_C_BCAST:
		/* Announcement type 5, Bcast_Parms1 14, Reg 1, Backoff 4, system identity code 16, Bcast_Parms2 24 */
		out->ann = (uint8_t)dmr_csbk_bits(b, 16, 5);
		out->parms1 = (uint16_t)dmr_csbk_bits(b, 21, 14);
		out->syscode = (uint16_t)dmr_csbk_bits(b, 40, 16);
		out->parms2 = dmr_csbk_bits(b, 56, 24);
		break;
	case CSBKO_PV_GRANT:
	case CSBKO_TV_GRANT:
	case CSBKO_BTV_GRANT:
	case CSBKO_P_CLEAR:
		/* LPCN 12, logical channel 1, late entry / high rate 1, emergency 1, offset 1, target 24, source 24 (P_CLEAR:
		 * the same channel and addresses, its flags reserved) */
		out->lpcn = (uint16_t)dmr_csbk_bits(b, 16, 12);
		out->ts = (uint8_t)(1u + dmr_csbk_bits(b, 28, 1));
		out->emergency = out->op != CSBKO_P_CLEAR && dmr_csbk_bits(b, 30, 1);
		out->target = dmr_csbk_bits(b, 32, 24);
		out->source = dmr_csbk_bits(b, 56, 24);
		break;
	default:
		break;
	}
}

bool dmr_csbk_chan(const uint8_t b[10], dmr_chan_t *out)
{
	/* LB 1, PF 1, CDEFTYPE 4, reserved 2 + 8, then CDEFPARMS: LPCN 12, TX MHz 10, TX 125 Hz steps 13, RX MHz 10, RX
	 * 125 Hz steps 13, reserved 6 (octets 10-11: the MBC's CRC) */
	if (dmr_csbk_bits(b, 2, 4) != 0)
		return false;
	out->lpcn = (uint16_t)dmr_csbk_bits(b, 16, 12);
	out->tx_hz = dmr_csbk_bits(b, 28, 10) * 1000000u + dmr_csbk_bits(b, 38, 13) * 125u;
	out->rx_hz = dmr_csbk_bits(b, 51, 10) * 1000000u + dmr_csbk_bits(b, 61, 13) * 125u;
	return out->rx_hz != 0 || out->tx_hz != 0;
}

void dmr_csbk_sys(uint16_t syscode, dmr_sys_t *out)
{
	static const uint8_t net_bits[4] = { 9, 7, 4, 2 };
	out->model = (uint8_t)(syscode >> 14);
	unsigned nb = net_bits[out->model], sb = 12u - nb;
	out->net = (uint16_t)((syscode >> (2u + sb)) & ((1u << nb) - 1u));
	out->site = (uint16_t)((syscode >> 2) & ((1u << sb) - 1u));
	out->par = (uint8_t)(syscode & 3u);
}
#endif
