/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus NXDN48 receive: the frame layer (docs/nxdn.md). Receive only: nothing here builds a frame for the air.
 *
 * A frame is 80 ms, 384 bits = 192 4FSK symbols at 2400 symbols/s:
 *   FSW 20 | LICH 16 | SACCH 60 | 4 x 72 voice (or FACCH1 2 x 144)
 * Everything after the frame sync word is scrambled with a PN9 sequence on the symbol signs. Bits are MSB first in
 * byte arrays, in air order; a dibit is two consecutive bits (01 +3, 00 +1, 10 -1, 11 -3).
 *
 * Ported from the GPL-3.0-or-later DM32-nxdn-p25 project (src/nxdn/nxdn.{c,h} at 05c527ae, "lyra XE1POC"), receive
 * half only, reworked for RAM (a constant scrambler mask, a Viterbi decoder that reads the frame in place and keeps its
 * survivors in the caller's state). That code states: the convolutional code, Viterbi decoder, puncturing,
 * interleaving and CRC generators follow MMDVMHost (Copyright (C) 2018,2023,2025 by Jonathan Naylor G4KLX,
 * GPL-2.0-or-later; NXDNConvolution.cpp, NXDNCRC.cpp, NXDNSACCH.cpp, NXDNFACCH1.cpp, NXDNLICH.cpp, NXDNLayer3.cpp),
 * rewritten in C. The AMBE+2 voice field layout follows MMDVMHost NXDNAudio.cpp and RadioFoundry's ambeFec.h (the
 * 72-bit NXDN voice field is the DMR AMBE+2 air frame, bit for bit). tests/plus/test_nxdn_ref.cpp checks all of it
 * against MMDVMHost and ambeFec.
 *
 * No allocation, no RTOS, no hardware, no floating point. */
#ifndef NXDN_H
#define NXDN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NXDN_FRAME_BITS		384u
#define NXDN_FRAME_BYTES	48u
#define NXDN_FRAME_SYMBOLS	192u
#define NXDN_FSW_BITS		20u
#define NXDN_LICH_BITS		16u
#define NXDN_SACCH_BITS		60u
#define NXDN_FACCH1_BITS	144u
#define NXDN_VOICE_BITS		72u
#define NXDN_SACCH_OFFSET	36u	/* bit offsets in the frame */
#define NXDN_PAYLOAD_OFFSET	96u
#define NXDN_LAYER3_BYTES	10u	/* FACCH1 data: 80 bits */
#define NXDN_SLOT_BYTES		9u	/* one 72-bit voice field (20 ms) */
#define NXDN_AMBE_BITS		49u
#define NXDN_FSW		0xCDF59u	/* 20 bits: -3 +1 -3 +3 -3 -3 +3 +3 -1 +3 */
#define NXDN_VIT_STEPS		96u	/* survivor words the longest code (FACCH1) needs */

/* LICH fields (TS 1-A) */
enum { NXDN_RFCT_RCCH = 0, NXDN_RFCT_RTCH = 1, NXDN_RFCT_RDCH = 2, NXDN_RFCT_RTCH_C = 3 };
enum { NXDN_USC_SACCH_NS = 0, NXDN_USC_UDCH = 1, NXDN_USC_SACCH_SS = 2, NXDN_USC_SACCH_SS_IDLE = 3 };
enum { NXDN_STEAL_FACCH = 0, NXDN_STEAL_FACCH1_1 = 1, NXDN_STEAL_FACCH1_2 = 2, NXDN_STEAL_NONE = 3 };
enum { NXDN_DIR_INBOUND = 0, NXDN_DIR_OUTBOUND = 1 };
/* SACCH structure: where an 18-bit layer 3 chunk sits in the superframe (part = 3 - structure) */
enum { NXDN_SF_SINGLE = 0, NXDN_SF_4_4 = 0, NXDN_SF_3_4 = 1, NXDN_SF_2_4 = 2, NXDN_SF_1_4 = 3 };
/* Layer 3 message types conventional voice uses */
enum { NXDN_MSG_VCALL = 0x01, NXDN_MSG_TX_REL = 0x08, NXDN_MSG_IDLE = 0x10 };
/* VCALL call types (3 bits) */
enum { NXDN_CALL_BROADCAST = 0, NXDN_CALL_GROUP = 1, NXDN_CALL_INDIVIDUAL = 4 };
#define NXDN_GROUP_ALL		0xFFFFu	/* a destination every unit hears */

typedef struct {
	uint8_t rfct, fct, option, direction;
} nxdn_lich_t;

typedef struct {
	uint8_t ran, structure;
	uint32_t data;		/* 18 bits */
} nxdn_sacch_t;

/* Layer 3 VCALL / TX_REL (TS 1-C): the fields conventional voice uses */
typedef struct {
	uint8_t type;		/* NXDN_MSG_VCALL / NXDN_MSG_TX_REL */
	uint8_t cc_option;	/* bit 7 emergency */
	uint8_t call_type;	/* NXDN_CALL_* */
	uint8_t voice_option;	/* bit 4 duplex, bits 2-0 transmission mode: 0 = 4800 bit/s EHR */
	uint16_t source, destination;
	uint8_t cipher_type, key_id;
} nxdn_call_msg_t;

/* Bit helpers (MSB first) */
static inline unsigned nxdn_bit(const uint8_t *b, unsigned i) { return (b[i >> 3] >> (7u - (i & 7u))) & 1u; }
static inline void nxdn_set_bit(uint8_t *b, unsigned i, unsigned v)
{
	uint8_t m = (uint8_t)(0x80u >> (i & 7u));
	b[i >> 3] = (uint8_t)(v ? (b[i >> 3] | m) : (b[i >> 3] & ~m));
}

/* LICH: 8 bits, even parity of RFCT and FCT in bit 0 (MMDVMHost's rule for every RDCH value). False on a parity
 * error */
bool nxdn_lich_unpack(uint8_t raw, nxdn_lich_t *l);
/* The LICH of a descrambled frame: each bit is sent as a dibit (b, 1); *off counts second bits that are 0 */
uint8_t nxdn_lich_get(const uint8_t frame[NXDN_FRAME_BYTES], unsigned *off);

/* The PN9 (x^9 + x^4 + 1, seed 0xE4) symbol scrambler over the 364 bits after the FSW, as a constant 48-byte XOR
 * mask (its first 20 bits zero). Scrambling and descrambling are the same XOR */
extern const uint8_t nxdn_scrambler[NXDN_FRAME_BYTES];
void nxdn_scramble(uint8_t frame[NXDN_FRAME_BYTES]);

/* CRCs over the first `bits` bits of buf, MSB first, initial value all ones */
uint8_t nxdn_crc6(const uint8_t *buf, unsigned bits);
uint16_t nxdn_crc12(const uint8_t *buf, unsigned bits);

/* SACCH of a descrambled frame: 26 bits (RAN 6, structure 2, 18 data) + CRC6. corrected (may be NULL): the Viterbi
 * path metric, about the number of channel bit errors. surv: NXDN_VIT_STEPS words of scratch */
bool nxdn_sacch_decode(const uint8_t frame[NXDN_FRAME_BYTES], nxdn_sacch_t *s, unsigned *corrected, uint16_t *surv);
/* FACCH1 copy `which` (0 = first, 1 = second half of the payload): 80 data bits + CRC12 */
bool nxdn_facch1_decode(const uint8_t frame[NXDN_FRAME_BYTES], unsigned which, uint8_t data[NXDN_LAYER3_BYTES],
			unsigned *corrected, uint16_t *surv);
/* VCALL / TX_REL fields; false for any other message type */
bool nxdn_layer3_decode(const uint8_t data[NXDN_LAYER3_BYTES], nxdn_call_msg_t *m);

/* Voice field k (0..3) of a descrambled frame: the 72-bit AMBE+2 air frame, the same bits RadioFoundry's DMR voice
 * sink takes (ambeFec.h) */
void nxdn_voice_get(const uint8_t frame[NXDN_FRAME_BYTES], unsigned k, uint8_t slot[NXDN_SLOT_BYTES]);
/* A 72-bit voice field -> the 49 AMBE+2 parameter bits (one per byte, 0/1, a12 b12 c25: the order ambeFecEncode
 * takes), Golay corrected. Returns the bits corrected (0..6), or -1 when C0 holds 4 or more errors (lost frame) */
int nxdn_ambe_params(const uint8_t slot[NXDN_SLOT_BYTES], uint8_t params[NXDN_AMBE_BITS]);

/* Golay (23,12) and (24,12) as the AMBE+2 frame uses them (generator 0xC75, data in the top 12 bits). Return the
 * errors corrected, -1 when the 24-bit word has 4 or more */
int nxdn_golay23_decode(uint32_t cw, uint16_t *data);
int nxdn_golay24_decode(uint32_t cw, uint16_t *data);
/* The 23-bit whitening of C1 from C0's 12 data bits */
uint32_t nxdn_ambe_c1_mask(uint16_t c0_data);

#ifdef __cplusplus
}
#endif
#endif
