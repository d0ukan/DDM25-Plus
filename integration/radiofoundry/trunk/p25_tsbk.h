/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus P25 trunk following (docs/plus.md): one TSBK (trunking signalling block) of a TSDU, TIA-102.BAAA /
 * TIA-102.AABB. Each block is 196 bits on air: 49 four-bit trellis symbols, interleaved; the 1/2-rate trellis carries
 * 48 data dibits and a zero flush dibit, i.e. 12 octets: octet 0 last-block flag, protect flag, 6-bit opcode; octet 1
 * manufacturer ID; octets 2-9 the arguments; octets 10-11 the CRC-CCITT (TIA-102.BAAA 5.3.1). No RTOS, no hardware. */
#ifndef PLUS_P25_TSBK_H
#define PLUS_P25_TSBK_H

#include <stdint.h>

enum { P25_TSBK_BYTES = 12, P25_TSBK_BITS = 196 };

/* The CRC octets 10-11 hold for octets 0-9: CRC-16 (x^16 + x^12 + x^5 + 1, register starting at 0, MSB first),
 * inverted */
uint16_t p25_tsbk_crc(const uint8_t b[10]);
/* Block n (0..2) of the TSDU in a framer's frame buffer (status symbols in place, sync at bit 0): deinterleave,
 * Viterbi-decode the trellis, check the CRC. 1 with out[12] when the CRC holds, else 0. *bits (may be NULL): the
 * trellis path's distance from what was received (bit errors it corrected; any value when the CRC failed) */
int p25_tsbk_decode(const uint8_t *frame, unsigned block, uint8_t out[P25_TSBK_BYTES], uint8_t *bits);
/* Bit offset within a 196-bit block of the first of deinterleaved symbol s's four bits (0..48); the encoder of the
 * host tests shares it */
unsigned p25_tsbk_symbol_bit(unsigned s);
/* The trellis: the four-bit symbol sent for input dibit `in` after input dibit `state` (the previous input) */
uint8_t p25_tsbk_trellis(unsigned state, unsigned in);

#endif
