/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus signal quality window (sigq.h). Pure logic: no RTOS, no hardware. */
#include "config.h"
#if CONFIG_SIGNAL_QUALITY
#include "radio/sigq.h"

static uint16_t sat_add(uint16_t a, uint32_t b)
{
	uint32_t s = (uint32_t)a + b;
	return s > 0xFFFFu ? (uint16_t)0xFFFFu : (uint16_t)s;
}

void sigq_add(sigq_t *q, uint32_t errors, uint32_t bits, uint32_t now_ms)
{
	if (bits == 0u)
		return;
	uint32_t slot = now_ms / SIGQ_SLOT_MS;
	if (slot != q->slot) {
		/* The slots passed since the last frames are empty; the window starts over after a longer gap */
		uint32_t gap = slot - q->slot;
		for (uint32_t k = 1u; k <= gap && k <= SIGQ_SLOTS; k++) {
			uint32_t i = (q->slot + k) % SIGQ_SLOTS;
			q->errors[i] = q->bits[i] = 0u;
		}
		q->slot = slot;
	}
	uint32_t i = slot % SIGQ_SLOTS;
	if (errors > bits)
		errors = bits;
	q->errors[i] = sat_add(q->errors[i], errors);
	q->bits[i] = sat_add(q->bits[i], bits);
}

int sigq_ber_permille(const sigq_t *q, uint32_t now_ms)
{
	uint32_t age = now_ms / SIGQ_SLOT_MS - q->slot;	/* slots since the newest was written */
	if (age > 0x80000000u)
		age = 0u;	/* a clock read just before the newest frames were added */
	uint32_t errors = 0u, bits = 0u;
	for (uint32_t k = 0u; k + age < SIGQ_SLOTS; k++) {
		uint32_t i = (q->slot - k) % SIGQ_SLOTS;
		errors += q->errors[i];
		bits += q->bits[i];
	}
	if (bits == 0u)
		return -1;
	return (int)((errors * 1000u + bits / 2u) / bits);
}
#endif
