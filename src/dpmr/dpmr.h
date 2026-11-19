/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus dPMR receive: the channel coding (docs/dpmr.md). Receive only: nothing here builds a frame for the air.
 *
 * Written from ETSI TS 102 490 V1.2.1 (dPMR446) and TS 102 658 V2.3.1 (dPMR, Mode 1-3), which share the air
 * interface used here: 4FSK at 2400 symbols/s (01 +3, 00 +1, 10 -1, 11 -3; +-1050 / +-350 Hz), 80 ms frames of
 * 384 bits, MSB first.
 *
 *   Header frame:  preamble >= 72 | FS1 48 | HI0 120 | CC 24 | HI1 120          (TS 102 490 4.2.2, 9.5)
 *   Payload frame: FS2 24 (frames 1, 3) or CC 24 (frames 2, 4) | CCH 72 | TCH 4 x 72   (4.2.2, 9.1)
 *   End frame:     FS3 24 | END 36 | END 36 (a copy)                                    (9.6)
 *
 * HI: 72 bits (header type 4, called ID 24, own ID 24, communication mode 3, format 4, reserved 2, call information
 * 11) + CRC8 (x^8 + x^2 + x + 1) = 80 bits, ten (12,8) shortened Hamming words (x^4 + x + 1), the 12 x 10 interleave,
 * scrambled; HI1 is the same 120 bits again. CCH: 41 bits (frame number 2, half an ID 12, mode 3, format 4, reserved
 * 2, slow data 18) + CRC7 (x^7 + x^3 + 1) = 48 bits, six Hamming words, the 12 x 6 interleave, scrambled. END: 17
 * bits + CRC7, three Hamming words, scrambled, sent twice. The scrambler is x^9 + x^5 + 1 preset to all ones and
 * starts again for each block (HI0, HI1, CCH, each END copy: TS 102 658 11.6). The TCH (voice) is not scrambled: each
 * 72-bit block is the AMBE+2 3600 bit/s air frame of 20 ms, bit for bit the DMR one (RadioFoundry's ambeFec.h), as
 * open decoders treat it (dsdcc DSDdPMR, F4EXB, uses the DMR deinterleave on it).
 *
 * The TS 102 658 versions put version (2), format (2), emergency (1) and preservation (1) bits where TS 102 490 has
 * format (4) and reserved (2); the values agree (version 0, format 00 all call / 01 peer to peer), so the low two
 * format bits tell an all call in both. No field marks encryption (vendor voice scramblers are outside the
 * standards), so nothing can be reported as encrypted.
 *
 * Bits are MSB first in byte arrays, in air order. No allocation, no RTOS, no hardware, no floating point. */
#ifndef DPMR_H
#define DPMR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DPMR_FS1		0x57FF5F75D577ull	/* header (TS 102 490 6.1.1) */
#define DPMR_FS4		0xFD55F5DF7FDDull	/* packet data header: FS1 with every symbol inverted */
#define DPMR_FS2		0x5FF77Du		/* payload frames 1 and 3 */
#define DPMR_FS3		0x7DDFF5u		/* end frame */
#define DPMR_SYMBOL_INVERT24	0xAAAAAAu		/* every symbol's sign: the first bit of each dibit */
#define DPMR_SYMBOL_INVERT48	0xAAAAAAAAAAAAull
#define DPMR_PREAMBLE		0x5Fu			/* TS 102 658 6.1.6: 5F repeated (+3 +3 -3 -3) */

#define DPMR_FRAME_SYMBOLS	192u
#define DPMR_SYNC_SYMBOLS	12u	/* FS2 / FS3 / CC */
#define DPMR_FS1_SYMBOLS	24u
#define DPMR_HI_BITS		120u
#define DPMR_CCH_BITS		72u
#define DPMR_TCH_BITS		72u
#define DPMR_END_BITS		36u	/* one copy */
#define DPMR_SLOT_BYTES		9u	/* one 72-bit voice block (20 ms), the DMR AMBE+2 air frame */
#define DPMR_CC_COUNT		64u
#define DPMR_CC_NONE		0xFFu
#define DPMR_ID_ALL		0xFFFFFFu	/* all call: every radio hears it */

/* Header types (HT, TS 102 490 5.11) and communication modes (M, 5.7 / TS 102 658 5.5.7) */
enum { DPMR_HT_START = 0, DPMR_HT_CONNECT = 1, DPMR_HT_UNCONNECT = 2, DPMR_HT_ACK = 3, DPMR_HT_SYS_REQ = 4,
       DPMR_HT_SYS_ACK = 5, DPMR_HT_SYS_DELIVERY = 6 };
enum { DPMR_M_VOICE = 0, DPMR_M_VOICE_SLD = 1, DPMR_M_DATA1 = 2, DPMR_M_DATA2 = 3, DPMR_M_PACKET = 4,
       DPMR_M_VOICE_DATA = 5, DPMR_M_SERVICE = 6 };

static inline bool dpmr_mode_voice(uint8_t m) { return m == DPMR_M_VOICE || m == DPMR_M_VOICE_SLD || m == DPMR_M_VOICE_DATA; }
/* format: TS 102 490's four bits; the low two are TS 102 658's F (00 all call, 01 peer to peer) */
static inline bool dpmr_all_call(uint8_t format) { return (format & 3u) == 0u; }
/* flags: TS 102 490's reserved bits, TS 102 658's emergency (bit 1) and preservation (bit 0) */
static inline bool dpmr_emergency(uint8_t flags) { return (flags & 2u) != 0u; }

typedef struct {
	uint8_t type;		/* HT */
	uint32_t called, own;	/* 24-bit air interface IDs */
	uint8_t mode, format, flags;
	uint16_t ci;		/* call information, 11 bits */
} dpmr_hi_t;

typedef struct {
	uint8_t fn;		/* frame number 0..3 */
	uint16_t id;		/* FN 0 / 1: the called ID's upper / lower 12 bits; FN 2 / 3: the own ID's */
	uint8_t mode, format, flags;
	uint32_t sld;		/* slow data, 18 bits */
} dpmr_cch_t;

typedef struct {
	uint8_t type, arq, wait, status;
} dpmr_end_t;

/* Bit helpers (MSB first) */
static inline unsigned dpmr_bit(const uint8_t *b, unsigned i) { return (b[i >> 3] >> (7u - (i & 7u))) & 1u; }
static inline void dpmr_set_bit(uint8_t *b, unsigned i, unsigned v)
{
	uint8_t m = (uint8_t)(0x80u >> (i & 7u));
	b[i >> 3] = (uint8_t)(v ? (b[i >> 3] | m) : (b[i >> 3] & ~m));
}

/* The scrambler's first 120 bits (x^9 + x^5 + 1, all ones; the same for every block), MSB first */
extern const uint8_t dpmr_scrambler[15];
/* The 64 channel / colour codes, 24 bits each, in TS 102 658 Table 6.1's order (code number 0..63) */
extern const uint32_t dpmr_cc_codes[DPMR_CC_COUNT];

/* CRC7 (x^7 + x^3 + 1) and CRC8 (x^8 + x^2 + x + 1) of the first `bits` bits, initial value 0 */
uint8_t dpmr_crc7(const uint8_t *buf, unsigned bits);
uint8_t dpmr_crc8(const uint8_t *buf, unsigned bits);
/* The (12,8) shortened Hamming code: parity of a byte, and a received word (data in bits 11..4) decoded with up to
 * one error corrected. Returns the errors corrected (0, 1) or -1 when the syndrome is no single error */
uint8_t dpmr_hamming_parity(uint8_t data);
int dpmr_hamming_decode(uint16_t word, uint8_t *data);

/* The code number (0..63) of a received 24-bit CC field within max_errors symbols, the nearest one; -1 for none.
 * Only the symbols' signs count (the codes are +3 / -3 only, and 3 symbols apart): a +3 read as +1 is no error.
 * errors (may be NULL): the symbols that differ */
int dpmr_cc_match(uint32_t field, unsigned max_errors, unsigned *errors);

/* A received block, as on the air (descrambled here). corrected (may be NULL): Hamming corrections. false: a word
 * past the code or the CRC */
bool dpmr_hi_decode(const uint8_t air[DPMR_HI_BITS / 8u], dpmr_hi_t *h, unsigned *corrected);
/* The header's two copies: HI0, else HI1, else word by word the copy that decoded with fewer corrections */
bool dpmr_hi_decode_pair(const uint8_t *hi0, const uint8_t *hi1, dpmr_hi_t *h);
bool dpmr_cch_decode(const uint8_t air[DPMR_CCH_BITS / 8u], dpmr_cch_t *c, unsigned *corrected);
/* One END copy: 36 bits from bit `first` of air */
bool dpmr_end_decode(const uint8_t *air, unsigned first, dpmr_end_t *e);

/* An air interface ID as people read it: "ALL" (0xFFFFFF), "CID 12" (TS 102 490 8.1.1: an 8-bit common ID with the
 * 16 bits after it all ones), the seven characters of TS 102 658 annex A.1.2 ("1234567", "468956*") up to 999****,
 * else the number. Returns the length written (always NUL terminated when cap > 0) */
size_t dpmr_id_text(uint32_t id, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
