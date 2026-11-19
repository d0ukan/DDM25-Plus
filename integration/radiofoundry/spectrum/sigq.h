/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus signal quality (CONFIG_SIGNAL_QUALITY): a bit error rate estimate for the digital voice being heard, from
 * the corrections its forward error correction already counts, over about the last second. The decoders count the
 * bits they corrected in each voice frame; corrected / FEC-protected bits is the channel's bit error rate as long as
 * no code word has more errors than its code corrects (beyond that the estimate reads low, so a frame that could not
 * be decoded at all counts as the most its code corrects). Pure logic: no RTOS, no hardware; callers lock. */
#ifndef DM32_PLUS_SIGQ_H
#define DM32_PLUS_SIGQ_H
#include <stdint.h>

#define SIGQ_SLOT_MS	250u	/* the window moves on in steps of this */
#define SIGQ_SLOTS	4u	/* ... and holds this many: the last 0.75 to 1 s */

/* P25 IMBE frame: 137 of its 144 bits are protected (4 Golay(23,12) + 3 Hamming(15,11) code words), at most 15
 * corrected; a superframe that failed to decode counts its 9 frames at that most */
#define SIGQ_P25_BITS	137u
#define SIGQ_P25_LOST	15u
/* DMR AMBE+2 frame: 47 of its 72 bits are protected (Golay(24,12) + Golay(23,12)); an uncorrectable frame counts 7,
 * as ambe_fec_result_t does */
#define SIGQ_DMR_BITS	47u

typedef struct {
	uint32_t slot;			/* the newest slot's number (ms / SIGQ_SLOT_MS) */
	uint16_t errors[SIGQ_SLOTS];	/* bits corrected */
	uint16_t bits[SIGQ_SLOTS];	/* protected bits they were found in */
} sigq_t;

/* Note frames heard at now_ms: their corrected bits and the protected bits they hold (both saturate per slot) */
void sigq_add(sigq_t *q, uint32_t errors, uint32_t bits, uint32_t now_ms);
/* The bit error rate over the last second in 0.1 % steps (0..1000), -1 when nothing was heard in it */
int sigq_ber_permille(const sigq_t *q, uint32_t now_ms);

#endif
