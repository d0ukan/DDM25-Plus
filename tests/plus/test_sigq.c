/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus signal quality window (integration/radiofoundry/spectrum/sigq.c): the bit error rate of the last second
 * in 0.1 % steps, nothing before anything was heard or once a second has passed without frames, older frames leave
 * the window as it moves, counts saturate instead of wrapping. */
#include <stdio.h>
#include <string.h>
#include "radio/sigq.h"

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

int main(void)
{
	sigq_t q;
	memset(&q, 0, sizeof q);
	CHECK(sigq_ber_permille(&q, 0) == -1);			/* nothing heard */
	CHECK(sigq_ber_permille(&q, 123456) == -1);

	/* P25: a clean superframe, then one with 9 bits corrected in its 9 frames: 9 / (18 * 137) = 0.36 % */
	uint32_t t = 100000;
	sigq_add(&q, 0, 9 * SIGQ_P25_BITS, t);
	CHECK(sigq_ber_permille(&q, t) == 0);
	sigq_add(&q, 9, 9 * SIGQ_P25_BITS, t + 180);
	CHECK(sigq_ber_permille(&q, t + 180) == 4);
	sigq_add(&q, 0, 0, t + 200);				/* no bits: no change */
	CHECK(sigq_ber_permille(&q, t + 200) == 4);

	/* A lost superframe counts its frames at the most the FEC corrects: 15 / 137 = 10.9 % */
	memset(&q, 0, sizeof q);
	sigq_add(&q, 9 * SIGQ_P25_LOST, 9 * SIGQ_P25_BITS, t);
	CHECK(sigq_ber_permille(&q, t) == 109);
	CHECK(sigq_ber_permille(&q, t + 700) == 109);		/* still inside the window ... */
	CHECK(sigq_ber_permille(&q, t + 1000) == -1);		/* ... a second later it has left */

	/* The window moves: bad frames a second ago drop out, recent clean ones stay */
	memset(&q, 0, sizeof q);
	sigq_add(&q, 47, 10 * SIGQ_DMR_BITS, t);		/* 10 % */
	sigq_add(&q, 0, 10 * SIGQ_DMR_BITS, t + 500);
	CHECK(sigq_ber_permille(&q, t + 500) == 50);
	sigq_add(&q, 0, 10 * SIGQ_DMR_BITS, t + 1000);
	CHECK(sigq_ber_permille(&q, t + 1000) == 0);
	/* A long gap empties the window; frames after it count alone */
	sigq_add(&q, 47, 10 * SIGQ_DMR_BITS, t + 10000);
	CHECK(sigq_ber_permille(&q, t + 10000) == 100);
	/* A reader whose clock is just behind the writer's still sees the newest slot */
	CHECK(sigq_ber_permille(&q, t + 9990) == 100);

	/* More errors than bits is clamped; counts saturate per slot (no wrap to a small number) */
	memset(&q, 0, sizeof q);
	sigq_add(&q, 99, 10, t);
	CHECK(sigq_ber_permille(&q, t) == 1000);
	memset(&q, 0, sizeof q);
	for (int i = 0; i < 1000; i++)
		sigq_add(&q, 1, 137, t);
	CHECK(q.bits[(t / SIGQ_SLOT_MS) % SIGQ_SLOTS] == 0xFFFFu);
	CHECK(sigq_ber_permille(&q, t) == 15);			/* 1000 / 65535 = 1.5 % */

	/* Early after boot (slot numbers near 0) and across the millisecond counter's wrap */
	memset(&q, 0, sizeof q);
	sigq_add(&q, 1, 100, 10);
	CHECK(sigq_ber_permille(&q, 10) == 10);
	sigq_add(&q, 1, 100, 0xFFFFFFF0u);
	CHECK(sigq_ber_permille(&q, 0xFFFFFFF0u) == 10);
	sigq_add(&q, 3, 100, 20);
	CHECK(sigq_ber_permille(&q, 20) == 30);

	if (failures)
		return 1;
	printf("test_sigq: ok\n");
	return 0;
}
