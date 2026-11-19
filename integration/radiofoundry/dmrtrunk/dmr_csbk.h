/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus DMR Tier III trunk following (docs/plus.md): the control signalling blocks of a trunked site's control
 * channel (TSCC), ETSI TS 102 361-2 / -4. A CSBK is one rate 1/2 BPTC(196,96) burst: 12 octets, octet 0 last-block
 * flag, protect flag, 6-bit opcode (CSBKO); octet 1 feature set ID (FID, 0x00 = ETSI standard); octets 2-9 the
 * arguments (bits 16-79 below, bit 0 = the MSB of octet 0); octets 10-11 CRC-CCITT, masked (CSBK 0xA5A5, MBC header
 * 0xAAAA). The HR_C7000 decodes the BPTC and checks the CRC itself; these work on its 12 octets. A multi-block CSBK
 * (MBC: a header with the last-block flag clear, then continuation blocks) carries absolute channel parameters (an
 * LPCN and its frequencies) after a C_BCAST Chan_Freq or a grant whose LPCN is 0xFFF. No RTOS, no hardware. */
#ifndef PLUS_DMR_CSBK_H
#define PLUS_DMR_CSBK_H

#include <stdbool.h>
#include <stdint.h>

enum {
	DMR_CSBK_MASK = 0xA5A5u, DMR_MBC_MASK = 0xAAAAu,
	/* Tier III opcodes (TS 102 361-4 annex: CSBKO) */
	CSBKO_C_ALOHA = 25, CSBKO_C_BCAST = 40, CSBKO_P_MAINT = 42, CSBKO_P_CLEAR = 46,
	CSBKO_PV_GRANT = 48, CSBKO_TV_GRANT = 49, CSBKO_BTV_GRANT = 50,
	/* C_BCAST announcement types */
	DMR_ANN_WD_TSCC = 0, DMR_ANN_VOTE_NOW = 2, DMR_ANN_CHAN_FREQ = 5, DMR_ANN_ADJ_SITE = 6, DMR_ANN_SITE_PARMS = 7,
	DMR_LPCN_ABSOLUTE = 0xFFFu,	/* a grant's LPCN: the channel's parameters follow in the MBC continuation */
};

/* What one CSBK says (only the fields of its opcode are filled; the rest is 0) */
typedef struct {
	uint8_t op;		/* CSBKO */
	uint8_t fid;
	bool last;		/* LB: false = an MBC header, continuation blocks follow */
	bool protect;		/* PF */
	uint16_t syscode;	/* C_ALOHA / C_BCAST: the system identity code */
	uint8_t ann;		/* C_BCAST: announcement type */
	uint16_t parms1;	/* C_BCAST: Bcast_Parms1 (14 bits) */
	uint32_t parms2;	/* C_BCAST: Bcast_Parms2 (24 bits) */
	uint16_t lpcn;		/* grants, P_CLEAR: logical physical channel number (12 bits) */
	uint8_t ts;		/* ... its logical channel: 1 or 2 */
	bool emergency;		/* grants */
	uint32_t target, source;	/* grants, P_CLEAR (24 bits each) */
} dmr_csbk_t;

/* The MBC continuation block's absolute channel parameters (CDEFTYPE 0): the LPCN and its two frequencies */
typedef struct {
	uint16_t lpcn;
	uint32_t tx_hz, rx_hz;	/* as named on air: the MS's transmit and receive frequencies */
} dmr_chan_t;

/* The system identity code split by its model: Tiny (Net 9 bits, Site 3), Small (7, 5), Large (4, 8), Huge (2, 10),
 * then the partition (2 bits). Net and Site as sent (the standard shows them plus 1) */
typedef struct {
	uint8_t model;		/* 0 Tiny, 1 Small, 2 Large, 3 Huge */
	uint16_t net, site;
	uint8_t par;
} dmr_sys_t;

/* CRC-CCITT of octets 0-9 (polynomial 0x1021, register starting at 0, MSB first, inverted: CRC-16/GSM) */
uint16_t dmr_csbk_crc(const uint8_t b[10]);
/* Octets 10-11 hold that CRC xor mask */
bool dmr_csbk_crc_ok(const uint8_t b[12], uint16_t mask);
/* n bits from bit pos of b (bit 0 = the MSB of octet 0) */
uint32_t dmr_csbk_bits(const uint8_t *b, unsigned pos, unsigned n);
/* Octets 0-9 of a CSBK or MBC header */
void dmr_csbk_parse(const uint8_t b[10], dmr_csbk_t *out);
/* Octets 0-9 of an MBC continuation block: true when it holds absolute channel parameters (CDEFTYPE 0) */
bool dmr_csbk_chan(const uint8_t b[10], dmr_chan_t *out);
void dmr_csbk_sys(uint16_t syscode, dmr_sys_t *out);

#endif
