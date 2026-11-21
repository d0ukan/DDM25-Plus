/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus channel editor: "Any" NAC and talkgroup (CONFIG_P25_ANY), through the real UI in the upstream simulator.
 * Built like tests/classic/test_ui.c with -DCONFIG_P25_CHANNELS=1 -DCONFIG_P25_ANY=1. */
#define main upstream_ui_main
#include "uiFlowTest.c"
#undef main
#include "ui/ui_svc.h"

#define P25_EDIT "ok 5 ok down down ok "
#define P25_TG P25_EDIT "down down down down down down down "
int main(void)
{
    const flow_t checks[]={
        /* Talkgroup 0 shows as Any, makes the channel RX only and then saves (Classic refused it) */
        {"Plus any talkgroup", P25_TG, "ok 0 ok", "shows=Any toast=Any_talkgroup", 3, false},
        {"Plus any talkgroup saves", P25_TG, "ok 0 ok long:ok", "writes=1 toast=Saved", 4, true},
        /* The context key while editing: Any at once */
        {"Plus any talkgroup by context", P25_TG, "ok ctx ok", "shows=Any toast=Any_talkgroup", 3, false},
        {"Plus any NAC by context", P25_TG "down", "ok ctx ok", "shows=Any_3966_($F7E) toast=none", 3, false},	/* Classic r13: RX Any keeps TX (Same as RX sends $293) */
        {"Plus any NAC saves", P25_TG "down", "ok ctx ok long:ok", "writes=1 toast=Saved", 4, true},
#if !CONFIG_NAC_DEC
        /* An ordinary NAC still edits as before */
        {"Plus NAC unchanged", P25_TG "down", "ok enc+ enc+ ok", "shows=$493", 4, false},
#else
        /* CONFIG_NAC_DEC (round 15): decimal first, "659 ($293)"; digits type the decimal value (a factory "293"
         * is $125), past 4095 refused; the knob steps it by one; # twice types hex digits */
        {"Plus NAC in decimal", P25_TG "down", "", "shows=659_($293)", 0, false},
        {"Plus NAC knob steps by one", P25_TG "down", "ok enc+ enc+ ok", "shows=661_($295)", 4, false},
        {"Plus NAC decimal entry", P25_TG "down", "ok 2 9 3", "shows=[293]_($125)", 4, false},
        {"Plus NAC decimal stored", P25_TG "down", "ok 2 9 3 ok", "shows=293_($125)", 5, false},
        {"Plus NAC decimal saves", P25_TG "down", "ok 2 9 3 ok long:ok", "writes=1 toast=Saved", 6, true},
        {"Plus NAC over 4095 refused", P25_TG "down", "ok 4 0 9 6 ok", "toast=NAC_goes_0_to_4095 shows=409_($199)", 6,
         false},
        {"Plus NAC back takes a digit back", P25_TG "down", "ok 2 9 3 back ok", "shows=29_($01D)", 6, false},
        {"Plus NAC hex digits", P25_TG "down", "ok ctx ctx 1 2 5 ok", "shows=293_($125)", 7, false},
        {"Plus NAC 3967 refused", P25_TG "down", "ok 3 9 6 7 ok", "toast=3967_($F7F)_is_not shows=[3967]", 6, false},
#endif
    };
    for(unsigned i=0;i<sizeof checks/sizeof checks[0];i++)
        run_flow(&checks[i],sim_geom_find("240x320"),SIM_MAP_K,"Plus DM32");
    printf("Plus any NAC/talkgroup editor: %d flows, %d failures\n",flows_run,fails);
    return fails!=0;
}
