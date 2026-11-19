// SPDX-License-Identifier: GPL-3.0-or-later
// DM32 Plus RAM reclaim: the start-up stack at the top of the RTOS heap (tools/plus_features.py prepare_startup_stack).
// The kernel's own heap_4.c on the host, the heap the Plus size (configTOTAL_HEAP_SIZE from the command line), the
// stack top where platform/hrc7000/common.ld puts it: two heap_4 block headers below the heap's end, 8-byte aligned
// (16 bytes on the radio). main()'s live frames are a canary under that top; every allocation is written all over
// (as xTaskCreate fills a stack). Checked: the boot's own allocations before the scheduler leave the canary alone
// and pass core/app/app.c's check (no free, least free >= PLUS_STARTUP_STACK_IN_HEAP + 64), and in thousands of random
// sequences (with and without frees) that check never passes once the heap has written into the canary.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"

uint8_t ucHeap[configTOTAL_HEAP_SIZE] __attribute__((aligned(8)));	// boards/dm32/board.c

// heap_4 suspends the scheduler around its lists and takes a critical section for its statistics
void vTaskSuspendAll(void) {}
BaseType_t xTaskResumeAll(void) { return pdFALSE; }
void vPortEnterCritical(void) {}
void vPortExitCritical(void) {}

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

enum { NEED = PLUS_STARTUP_STACK_IN_HEAP, CANARY = 0x5A };

static uint8_t *stack_top(void)
{
	size_t hdr = (2 * sizeof(void *) + 7u) & ~(size_t)7u;	// heap_4's xHeapStructSize: 8 on the radio
	return (uint8_t *)(((uintptr_t)ucHeap + configTOTAL_HEAP_SIZE - 2 * hdr) & ~(uintptr_t)7u);
}

// main()'s frames: the NEED bytes under the stack top (and the few bytes from the top to heap_4's end marker)
static void canary_set(void) { memset(stack_top() - NEED, CANARY, NEED); }
static int canary_ok(void)
{
	const uint8_t *p = stack_top() - NEED;
	for (unsigned i = 0; i < NEED; i++)
		if (p[i] != CANARY) return 0;
	return 1;
}

// core/app/app.c plus_startup_stack_check()
static int boot_check_passes(void)
{
	HeapStats_t h;
	vPortGetHeapStats(&h);
	return h.xNumberOfSuccessfulFrees == 0 && h.xMinimumEverFreeBytesRemaining >= NEED + 64u;
}

static void *take(size_t n)
{
	void *p = pvPortMalloc(n);
	if (p) memset(p, 0xA5, n);	// tskSTACK_FILL_BYTE: a new task's stack is written all over
	return p;
}

static void fresh(void)
{
	vPortHeapResetState();
	memset(ucHeap, 0, sizeof ucHeap);
	canary_set();
}

static uint32_t lcg = 12345u;
static uint32_t rnd(uint32_t n) { lcg = lcg * 1103515245u + 12345u; return (lcg >> 8) % n; }

int main(void)
{
	CHECK(stack_top() > ucHeap + NEED && stack_top() < ucHeap + configTOTAL_HEAP_SIZE, "stack top inside the heap");

	// The radio's boot before the scheduler: the start-up task (2,560 B stack), the idle (576 B) and timer (1,024 B)
	// tasks and the timer queue, each with a generous control block; then tasks the start-up task makes (board.c
	// mutex, radio, UI, ...) fit too, the scheduler running by then
	fresh();
	static const size_t boot[] = { 2560, 192, 576, 192, 1024, 192, 256 };
	for (unsigned i = 0; i < sizeof boot / sizeof boot[0]; i++)
		CHECK(take(boot[i]) != NULL, "boot allocation %u", i);
	CHECK(canary_ok(), "the boot's allocations wrote into main()'s stack");
	CHECK(boot_check_passes(), "the start-up task's check refuses the radio's own boot");

	// Random sequences up to an empty heap: the check passing always means the canary is whole
	unsigned hit = 0, passed_whole = 0, with_frees = 0;
	for (unsigned run = 0; run < 3000; run++) {
		fresh();
		void *live[64];
		unsigned n = 0;
		int frees = run % 3 == 0;	// a third with frees (a failed xTaskCreate gives its stack back)
		for (unsigned step = 0; step < 400; step++) {
			if (frees && n && rnd(4) == 0) {
				unsigned k = rnd(n);
				vPortFree(live[k]);
				live[k] = live[--n];
			} else {
				void *p = take(16 + rnd(run % 2 ? 6000 : 1500));
				if (!p) break;
				if (n < 64) live[n++] = p;
			}
			int whole = canary_ok(), pass = boot_check_passes();
			if (pass) {
				CHECK(whole, "run %u step %u: the check passes over a written canary", run, step);
				passed_whole++;
			}
			if (!whole) {
				hit++;
				with_frees += frees;
				break;
			}
		}
	}
	CHECK(hit > 1000, "the random runs reach main()'s stack (%u)", hit);
	CHECK(with_frees > 100, "and some of them with frees (%u)", with_frees);
	CHECK(passed_whole > 10000, "the check passes often enough to matter (%u)", passed_whole);
	printf("test_startup_heap: heap %u B, stack top heap+%u, %u runs reached the stack, %u passing checks, %d failures\n",
	       (unsigned)configTOTAL_HEAP_SIZE, (unsigned)(stack_top() - ucHeap), hit, passed_whole, failures);
	return failures != 0;
}
