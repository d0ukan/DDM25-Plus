/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus user data packs' UI flows: what tests/plus/ui_sim_dpk.c adds to the air items' simulator */
#ifndef UI_SIM_DPK_H
#define UI_SIM_DPK_H

#include "ui_sim_air.h"

/* After sim_reset: air_sim_setup, and an erased 16 MiB data chip on the board (no pack in force until written) */
void dpk_sim_setup(void);

#endif
