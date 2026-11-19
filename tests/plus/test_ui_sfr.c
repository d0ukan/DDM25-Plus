/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus channel editor: the SFR role rows (CONFIG_SFR_EDIT), through the real UI in the upstream simulator.
 * Built like tests/classic/test_ui.c with -DCONFIG_P25_CHANNELS=1 -DCONFIG_SFR_EDIT=1. The channel in use (3, COUNTY
 * DMR) is a one-frequency DMR channel; its SFR row is the 20th (after Battery saver), the last row before Zones. A
 * toast covers the bottom row, so values under one are read once it has gone. */
#define main upstream_ui_main
#include "uiFlowTest.c"
#undef main
#include "ui/ui_svc.h"

#define DOWN5 "down down down down down "
#define SFR_ROW "ok 5 ok " DOWN5 DOWN5 DOWN5 "down down down down "
/* TX 439.5 MHz: two frequencies, then on to the SFR row */
#define TWO_FREQ "ok 5 ok down down down down ok 4 3 9 5 0 0 ok " DOWN5 DOWN5 DOWN5
/* RX only on, then on to the SFR row */
#define RX_ONLY "ok 5 ok " DOWN5 "down ok " DOWN5 DOWN5 "down down down "
/* Channel 4, County fire: FM */
#define FM_EDITOR "ok 5 down ok "
int main(void)
{
    const flow_t checks[]={
        {"Plus SFR row on a DMR channel", SFR_ROW, "", "focus=SFR shows=Off", 0, false},
        {"Plus SFR: no role, no more rows", SFR_ROW, "down", "focus=Zones", 1, false},
        {"Plus SFR user", SFR_ROW, "ok", "focus=SFR shows=User", 1, false},
        {"Plus SFR user: inbound slot row", SFR_ROW, "ok down", "focus=SFR_inbound_slot shows=TS1", 2, false},
        {"Plus SFR user: no outbound color code", SFR_ROW, "ok down down", "focus=Zones", 3, false},
        {"Plus SFR inbound slot 2", SFR_ROW, "ok down ok", "focus=SFR_inbound_slot shows=TS2", 3, false},
        {"Plus SFR ERDM user", SFR_ROW, "ok ok", "focus=SFR shows=ERDM_user", 2, false},
        {"Plus SFR ERDM outbound color code row", SFR_ROW, "ok ok down down", "focus=SFR_outbound_CC shows=Channel's", 4, false},
        {"Plus SFR ERDM outbound color code 2", SFR_ROW, "ok ok down down ok 2 ok",
         "focus=SFR_outbound_CC hides=Channel's", 7, false},
        {"Plus SFR ERDM outbound color code by the knob", SFR_ROW, "ok ok down down ok enc+ ok",
         "focus=SFR_outbound_CC shows=0", 7, false},
        {"Plus SFR repeater toast", SFR_ROW, "ok ok ok", "focus=SFR toast=Repeater_role:_transmits", 3, false},
        {"Plus SFR repeater", SFR_ROW, "ok ok ok wait:6000", "focus=SFR shows=Repeater hides=ERDM", 3, false},
        {"Plus SFR repeater ERDM toast", SFR_ROW, "ok ok ok ok", "toast=Repeater_role:_transmits", 4, false},
        {"Plus SFR repeater ERDM", SFR_ROW, "ok ok ok ok wait:6000", "focus=SFR shows=Repeater_(ERDM", 4, false},
        {"Plus SFR back to Off", SFR_ROW, "ok ok ok ok ok wait:6000 down", "focus=Zones", 6, false},
        {"Plus SFR user saves", SFR_ROW, "ok long:ok", "writes=1 toast=Saved", 2, true},
        {"Plus SFR two frequencies", TWO_FREQ, "", "focus=SFR shows=Needs", 0, false},
        {"Plus SFR two frequencies refused", TWO_FREQ, "ok", "toast=SFR_needs_one_frequency", 1, false},
        {"Plus SFR two frequencies: no role set", TWO_FREQ, "ok down", "focus=Zones", 2, false},
        {"Plus SFR RX only: no repeater", RX_ONLY, "ok ok ok", "focus=SFR toast=Repeater_needs_TX", 3, false},
        {"Plus SFR RX only: back to Off", RX_ONLY, "ok ok ok wait:6000", "focus=SFR shows=Off", 3, false},
        {"Plus SFR row not on FM", FM_EDITOR DOWN5 DOWN5 "down down down ", "", "shows=County_fire focus=Battery_saver", 0, false},
        {"Plus SFR row not on FM: Zones next", FM_EDITOR DOWN5 DOWN5 "down down down ", "down", "focus=Zones", 1, false},
    };
    for(unsigned i=0;i<sizeof checks/sizeof checks[0];i++)
        run_flow(&checks[i],sim_geom_find("240x320"),SIM_MAP_K,"Plus DM32");
    printf("Plus SFR role editor: %d flows, %d failures\n",flows_run,fails);
    return fails!=0;
}
