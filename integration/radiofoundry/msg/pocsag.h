/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus pager receive (CONFIG_MSG_RX, docs/plus.md): a POCSAG decoder for 8 kHz FM receive audio. Installed as
 * core/radio/pocsag.{c,h}. Pure integer DSP, no RTOS, no board: the radio task glue is radio_msg.c, the host test
 * tests/plus/test_pocsag.c. Written for this project from ITU-R Recommendation M.584-2 ("Codes and formats for radio
 * paging"); no code from other decoders.
 *
 * The format (M.584 Annex 1): 2-FSK NRZ at 512, 1200 or 2400 bit/s; a preamble of at least 576 alternating bits, then
 * batches, each a synchronization codeword 0x7CD215D8 and 8 frames of 2 codewords. A codeword is 32 bits, sent MSB
 * first: a flag (0 address, 1 message), 20 information bits, 10 BCH(31,21) check bits (generator x^10 + x^9 + x^8 +
 * x^6 + x^5 + x^3 + 1) and an even parity bit. An address codeword carries the 18 high bits of the 21-bit address
 * (the capcode; its 3 low bits are the frame the codeword is sent in) and 2 function bits; the message codewords that
 * follow carry 20 bits each, numeric (4-bit characters) or alphanumeric (7-bit ITU-T T.50 / ASCII), each character
 * least significant bit first. A message ends at the next address codeword, the idle codeword 0x7A89C197 or the end
 * of the transmission.
 *
 * Demodulator, six in parallel on the same samples (each rate with two slicers): a one-pole low pass (none at 2400
 * bit/s), a slicer with hysteresis around the middle of the two levels it has seen (each level a running mean of the
 * samples on its side), and a bit clock (a phase accumulator at the rate that each slicer edge pulls a quarter of
 * the way toward a bit boundary; after 10 bits without an edge the next one sets it outright, but not while a batch
 * is being received). A bit is the slicer's majority over the bit. The "level" slicer's hysteresis is an eighth of
 * the distance between the two levels: right for flat audio (the Pager screen's flat receive path) and audio through
 * de-emphasis. The "edge" slicer's is half the peak swing: it keeps its side until the signal steps to the other
 * one, so audio through a high pass (long runs decaying toward the middle, an edge a step of twice the swing) still
 * slices as NRZ. The first of the six to find a sync receives the transmission. Which polarity is a 1 is not
 * assumed: the sync is also taken inverted.
 *
 * Sync: the first batch of a transmission needs the sync codeword within 2 bit errors at most 40 bits after a
 * preamble (32 or more alternating bits), or exactly anywhere; the next batch's sync is expected after 16 codewords
 * (4 bit errors allowed there), and a missing one ends the transmission. For a short while after that (two batches)
 * a sync within 2 errors is taken again without a preamble (a transmitter that skipped the preamble, a bit slip).
 * Codewords: up to 2 bit errors corrected (syndrome table), the parity checked. A codeword with more errors whose
 * flag reads 0 is most likely the idle codeword or the next address: it ends the message; one whose flag reads 1 is
 * taken into the message as it came and marks it (errors). */
#ifndef POCSAG_H
#define POCSAG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define POCSAG_FS		8000u	/* the audio's sample rate */
#define POCSAG_SYNC		0x7CD215D8u
#define POCSAG_IDLE		0x7A89C197u
#define POCSAG_BITS_MAX		560u	/* message bits kept: 80 alphanumeric characters */

enum { POCSAG_512, POCSAG_1200, POCSAG_2400, POCSAG_RATES };
#define POCSAG_DEMODS		(2u * POCSAG_RATES)	/* each rate with both slicers: index rate x 2 + slicer */
enum { POCSAG_TONE, POCSAG_NUMERIC, POCSAG_ALPHA };	/* pocsag_text */

typedef struct {
	uint32_t capcode;	/* 21-bit address: the 18 address bits and the frame number */
	uint8_t function;	/* 0-3: the address codeword's function bits */
	uint8_t rate;		/* POCSAG_512 / _1200 / _2400 */
	bool errors;		/* a codeword had more errors than the code corrects */
	bool cut;		/* longer than POCSAG_BITS_MAX: the rest left out */
	uint16_t nbits;		/* message bits (0: a tone only call) */
	const uint8_t *bits;	/* nbits in transmission order, MSB first in each byte */
} pocsag_msg_t;

typedef void (*pocsag_fn)(void *ctx, const pocsag_msg_t *m);

typedef struct {
	int32_t lp;		/* the low pass, input x 16 */
	int32_t hi, lo;		/* the levels of the two sides */
	int32_t pk;		/* the peak swing from their middle */
	uint32_t ph;		/* bit clock, 2^32 = one bit */
	uint32_t sr;		/* the last 32 bits */
	int16_t acc;		/* samples on the high side minus the low side in this bit */
	uint8_t quiet;		/* bits since the last edge */
	uint8_t alt;		/* alternating bits in a row */
	uint8_t since_pre;	/* bits since a preamble ended, 255 = long ago */
	bool state;		/* the slicer's side */
	bool last;		/* the last bit */
	int8_t word;		/* the batches it follows: the codeword's place, 16 = the next sync; -1 = hunting */
	uint8_t nbit;		/* bits of that codeword */
	bool inv;		/* its bits are inverted */
} pocsag_demod_t;

typedef struct {
	pocsag_demod_t d[POCSAG_DEMODS];
	int8_t rate;		/* the rate a transmission is received at, -1 = hunting */
	int8_t grace_rate;	/* the rate of the transmission that just ended */
	uint16_t grace;		/* its bits left in which a sync within 2 errors is taken without a preamble */
	/* a codeword from one slicer waiting for the other's copy */
	bool pend;
	uint8_t pend_v, pend_k;	/* its slicer, its place in the batch */
	int8_t pend_e;		/* its errors, -1 = too many */
	uint32_t pend_cw;
	bool in_msg;		/* a message is being assembled */
	pocsag_msg_t m;
	uint8_t bits[(POCSAG_BITS_MAX + 7u) / 8u];
	/* counters (diagnostics, host test) */
	uint32_t syncs, batches, corrected, uncorrectable, msgs;
	pocsag_fn fn;
	void *ctx;
} pocsag_t;

void pocsag_init(pocsag_t *p, pocsag_fn fn, void *ctx);
/* n samples of 8 kHz FM receive audio; messages come through fn as they end */
void pocsag_audio(pocsag_t *p, const int16_t *s, size_t n);
/* The signal is gone (another frequency, the screen left): a message being received is handed out */
void pocsag_flush(pocsag_t *p);

/* The 32-bit codeword for 21 information bits (bit 20: the flag; bits 19-0: address and function, or message bits) */
uint32_t pocsag_encode(uint32_t info21);
/* Corrects up to 2 bit errors in place: the number corrected, -1 when there are more */
int pocsag_correct(uint32_t *cw);
/* A message as text (out of at least 2 bytes, always terminated): alphanumeric or numeric by the function bits,
 * unless the content says otherwise (function 0 numeric, others alphanumeric; a numeric message with spare
 * characters that reads as clean text is taken as text, and the reverse). Returns POCSAG_TONE / _NUMERIC / _ALPHA */
uint8_t pocsag_text(const pocsag_msg_t *m, char *out, size_t size);

#endif
