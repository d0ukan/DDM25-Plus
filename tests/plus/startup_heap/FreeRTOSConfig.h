/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Host build of the kernel's heap_4.c alone for tests/plus/test_startup_heap.c (the POSIX port's portmacro.h).
 * configTOTAL_HEAP_SIZE and PLUS_STARTUP_STACK_IN_HEAP come from the command line (tools/plus_features.py). */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H
#define configUSE_PREEMPTION			1
#define configUSE_IDLE_HOOK			0
#define configUSE_TICK_HOOK			0
#define configTICK_RATE_HZ			1000
#define configMAX_PRIORITIES			8
#define configMINIMAL_STACK_SIZE		144
#define configTICK_TYPE_WIDTH_IN_BITS		TICK_TYPE_WIDTH_32_BITS
#define configSUPPORT_DYNAMIC_ALLOCATION	1
#define configSUPPORT_STATIC_ALLOCATION		0
#define configUSE_MALLOC_FAILED_HOOK		0
#define configUSE_TIMERS			0
#define configAPPLICATION_ALLOCATED_HEAP	1	/* as boards/dm32/freertos_board.h in the Plus build */
#define configASSERT(x)				do { if (!(x)) __builtin_trap(); } while (0)
#endif
