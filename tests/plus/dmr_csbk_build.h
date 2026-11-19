/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus DMR Tier III host tests: CSBKs and MBC blocks as a trunked site's control channel sends them (ETSI
 * TS 102 361-4 layouts, written field by field here, independently of dmr_csbk.c's reader), with their CRC. */
#ifndef PLUS_DMR_CSBK_BUILD_H
#define PLUS_DMR_CSBK_BUILD_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "radio/dmr_csbk.h"

static void cb_put(uint8_t *b, unsigned pos, unsigned n, uint32_t v)
{
    for (unsigned i = 0; i < n; i++, pos++) {
        uint8_t m = (uint8_t)(0x80u >> (pos & 7u));
        if ((v >> (n - 1u - i)) & 1u) b[pos >> 3] |= m; else b[pos >> 3] &= (uint8_t)~m;
    }
}

/* Octets 10-11: CRC-16/GSM of octets 0-9 (computed here bit by bit from the polynomial), xor mask */
static void cb_crc(uint8_t b[12], uint16_t mask)
{
    uint16_t reg = 0;
    for (unsigned i = 0; i < 80u; i++) {
        unsigned bit = (b[i >> 3] >> (7u - (i & 7u))) & 1u;
        unsigned fb = ((reg >> 15) & 1u) ^ bit;
        reg = (uint16_t)(reg << 1);
        if (fb) reg ^= 0x1021u;
    }
    reg = (uint16_t)(~reg ^ mask);
    b[10] = (uint8_t)(reg >> 8);
    b[11] = (uint8_t)reg;
}

/* Octet 0 and 1: last block, opcode, FID 0 (standard); the CRC with the CSBK mask, or the MBC header's when !last */
static void cb_head(uint8_t b[12], uint8_t op, bool last)
{
    memset(b, 0, 12);
    b[0] = (uint8_t)((last ? 0x80u : 0u) | op);
}
static void cb_seal(uint8_t b[12]) { cb_crc(b, (b[0] & 0x80u) ? DMR_CSBK_MASK : DMR_MBC_MASK); }

static uint16_t cb_syscode(uint8_t model, uint16_t net, uint16_t site, uint8_t par)
{
    static const unsigned nb[4] = { 9, 7, 4, 2 };
    unsigned sb = 12u - nb[model];
    return (uint16_t)(model << 14 | net << (2u + sb) | site << 2 | par);
}

/* C_ALOHA: TS sync, version 1, mask 0, service function 0, NRand_Wait 5, Reg 1, Backoff 3, the system, MS address 0 */
static void cb_aloha(uint8_t b[12], uint16_t syscode)
{
    cb_head(b, CSBKO_C_ALOHA, true);
    cb_put(b, 18, 1, 1);
    cb_put(b, 19, 3, 1);
    cb_put(b, 31, 4, 5);
    cb_put(b, 35, 1, 1);
    cb_put(b, 36, 4, 3);
    cb_put(b, 40, 16, syscode);
    cb_seal(b);
}

static void cb_bcast(uint8_t b[12], uint8_t ann, uint16_t parms1, uint16_t syscode, uint32_t parms2, bool last)
{
    cb_head(b, CSBKO_C_BCAST, last);
    cb_put(b, 16, 5, ann);
    cb_put(b, 21, 14, parms1);
    cb_put(b, 40, 16, syscode);
    cb_put(b, 56, 24, parms2);
    cb_seal(b);
}

/* TV_GRANT / BTV_GRANT / PV_GRANT (and P_CLEAR with its flags 0): LPCN, slot, emergency, target, source */
static void cb_grant(uint8_t b[12], uint8_t op, uint16_t lpcn, uint8_t ts, bool emergency, uint32_t target,
                     uint32_t src, bool last)
{
    cb_head(b, op, last);
    cb_put(b, 16, 12, lpcn);
    cb_put(b, 28, 1, ts == 2u);
    cb_put(b, 30, 1, emergency);
    cb_put(b, 32, 24, target);
    cb_put(b, 56, 24, src);
    cb_seal(b);
}

/* The last MBC block with absolute channel parameters (CDEFTYPE 0); its CRC octets left 0 */
static void cb_chan(uint8_t b[12], uint16_t lpcn, uint32_t tx_hz, uint32_t rx_hz)
{
    memset(b, 0, 12);
    b[0] = 0x80u;
    cb_put(b, 16, 12, lpcn);
    cb_put(b, 28, 10, tx_hz / 1000000u);
    cb_put(b, 38, 13, (tx_hz % 1000000u) / 125u);
    cb_put(b, 51, 10, rx_hz / 1000000u);
    cb_put(b, 61, 13, (rx_hz % 1000000u) / 125u);
}

#endif
