// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Copyright (c) 2026 Open25-DM32 Authors
#ifndef RADIO_P25_TX_POLICY_H
#define RADIO_P25_TX_POLICY_H
#include "board.h"
#include "config.h"

/* Product frequency policy, independent of channel RX-only and TX inhibit.
 * Service exceptions are continuous ranges, including between channel centers. */
static inline bool radio_p25_tx_allowed(uint32_t hz)
{
 if (!board_in_band(hz, true)) return false;
#if CONFIG_P25_TX_BAND_POLICY
 if ((hz >= 144000000u && hz < 148000000u) ||
     (hz >= 420000000u && hz < 450000000u)) return true;
 return (hz>=151820000u && hz<=151940000u) ||
        (hz>=154570000u && hz<=154600000u) ||
        (hz>=462550000u && hz<=462725000u) ||
        (hz>=467550000u && hz<=467725000u);
#else
 return true;
#endif
}
#endif
