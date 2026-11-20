/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus: the tool screens' shared buffer (ui_scratch.h). Installed as core/ui/ui_scratch.c. */
#include "config.h"
#if CONFIG_SPECTRUM || CONFIG_DMR_SEARCH || CONFIG_P25_SEARCH || CONFIG_DMR_TRUNK || CONFIG_P25_TRUNK || CONFIG_MSG_RX
#include "ui/ui_scratch.h"

ui_scratch_t ui_scratch;
static uint8_t owner_now;

ui_scratch_t *ui_scratch_take(uint8_t owner)
{
	owner_now = owner;
	return &ui_scratch;
}

bool ui_scratch_mine(uint8_t owner)
{
	return owner != UI_SCRATCH_NONE && owner_now == owner;
}
#endif
