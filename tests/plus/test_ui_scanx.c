/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus priority channel and temporary skip (docs/plus.md "Priority channel and temporary skip"): the UI flows in
 * the upstream simulator (tests/plus/ui_sim_scanx.c), built like tests/plus/test_ui_mwatch.c with
 * -DCONFIG_SCAN_PRIO=1. First upstream's own flows run with it built in (tools/test_classic.py checks they fail exactly
 * where the build without fails), then these: 0 skips where a scan stopped (and only there; without a scan 0 is Find's
 * still), # lists Clear skips and Set as priority, # 4 and # 5, the marks (the channel number's P on Home, SCAN P and
 * DUAL P on the strip), the hint, Settings > Talk & listen > Priority channel, the zone editor's Set as priority, and
 * the setting across a power cycle. */
#define main upstream_ui_main
#include "uiFlowTest.c"
#undef main
#include "ui/ui_svc.h"
#include "radio/scan.h"

extern uint8_t scx_sim_phase;
extern unsigned scx_sim_skips, scx_sim_unskips;

typedef struct {
    flow_t f;
    uint8_t phase;          /* what the scan reports once started */
    int skips, unskips;     /* the requests the flow's script must make, -1: not checked */
} scx_flow_t;

#define SET_P "ok 6 1 ctx 5 wait:3000 "   /* scanning, channel 3 the priority one (its toast gone) */

int main(void)
{
    upstream_ui_main();   /* its own result: tools/test_classic.py compares its failures with the build without */
    int before = fails, run_before = flows_run;
    const scx_flow_t checks[] = {
        /* 0 while a scan has stopped on a signal: that channel skipped (the simulator's skip moves it on) */
        { { "SX 0 skips where the scan stopped", "ok 6 1", "0",
            "toast=Skipped_until_the_scan_stops scanning=1 pos=4 screen=home", 1, false }, SCAN_PH_HOLD, 1, 0 },
        { { "SX 0 in the wait after a signal", "ok 6 1", "0", "toast=Skipped_until pos=4", 1, false }, SCAN_PH_RESUME, 1, 0 },
        /* ... while it still steps: nothing to skip yet, nothing asked */
        { { "SX 0 while the scan steps", "ok 6 1", "0", "toast=Nothing_to_skip_yet scanning=1 pos=3", 1, false },
          SCAN_PH_LISTEN, 0, 0 },
        /* Without a scan 0 is Find's as before */
        { { "SX 0 without a scan: Find", "", "0", "screen=find", 1, false }, SCAN_PH_HOLD, 0, 0 },
        /* The hint while scanning names the key */
        { { "SX the hint names 0", "down ok 6 1 wait:3000", "", "shows=0_skip", 0, false }, SCAN_PH_HOLD, -1, -1 },
        /* # while scanning: upstream's three, then Clear skips and Set as priority */
        { { "SX # lists 5", "ok 6 1", "ctx", "screen=scan shows=Clear_skips shows=Set_as_priority shows=1-5_pick", 1, false },
          SCAN_PH_HOLD, 0, 0 },
        { { "SX # 1 still skips", "ok 6 1", "ctx 1", "toast=Skipped_until pos=4", 2, false }, SCAN_PH_HOLD, 1, 0 },
        { { "SX # 4 clears the skips", "ok 6 1 0", "ctx 4", "screen=home toast=Skips_cleared scanning=1", 2, false },
          SCAN_PH_HOLD, 1, 1 },
        /* # 5: the channel on view is the priority one; the marks */
        { { "SX # 5 sets the priority channel", "ok 6 1", "ctx 5",
            "toast=Priority:_3_COUNTY_DMR shows=SCAN_P shows=3P scanning=1", 2, false }, SCAN_PH_HOLD, 0, 0 },
        { { "SX # on it says Priority off", SET_P, "ctx", "shows=Priority_off", 1, false }, SCAN_PH_HOLD, -1, -1 },
        { { "SX # 5 again: off", SET_P, "ctx 5", "toast=Priority_channel_off hides=SCAN_P hides=3P", 2, false },
          SCAN_PH_HOLD, -1, -1 },
        { { "SX the P on another channel's Home: none", SET_P "back up", "", "pos=4 hides=4P hides=3P", 0, false },
          SCAN_PH_HOLD, -1, -1 },
        { { "SX after power off", SET_P "back", "reboot", "shows=3P", 0, false }, SCAN_PH_HOLD, -1, -1 },
        { { "SX DUAL P on the strip", SET_P "back", "quick 7", "dual=1 shows=DUAL_P", 2, false },
          SCAN_PH_HOLD, -1, -1 },
        /* Settings > Talk & listen > Priority channel (the last row): OK takes the channel on view, OK again Off */
        { { "SX the setting", "", "ok 7 3 up ok", "toast=Priority:_3_COUNTY_DMR shows=Priority_channel", 5, false },
          SCAN_PH_HOLD, -1, -1 },
        { { "SX the setting off", "ok 7 3 up ok wait:3000", "ok", "toast=Priority_channel_off shows=Off", 1, false },
          SCAN_PH_HOLD, -1, -1 },
        /* The zone editor's channel actions (# on a channel of the zone list): 7 Set as priority (6 is Remove) */
        { { "SX the zone editor sets it", "", "ok 4 ctx ctx 7", "toast=Priority:_3_COUNTY_DMR", 5, false },
          SCAN_PH_HOLD, -1, -1 },
        { { "SX the zone editor says Priority off", "ok 4 ctx ctx 7 wait:3000", "ctx", "shows=Priority_off shows=1-7_pick", 1, false },
          SCAN_PH_HOLD, -1, -1 },
    };
    scx_sim_skips = scx_sim_unskips = 0;
    for (unsigned i = 0; i < sizeof checks / sizeof checks[0]; i++) {
        const scx_flow_t *c = &checks[i];
        scx_sim_phase = c->phase;
        ui_svc_setting_set(UI_SET_SCAN_PRIO, 0);   /* the simulator keeps settings from flow to flow */
        int f0 = fails;
        run_flow(&c->f, sim_geom_find("240x320"), SIM_MAP_K, "Plus DM32");
        /* the counts are the whole flow's (setup and script): the setups above ask for none but "SX # 4" (its 0) */
        if (c->skips >= 0 && (int)scx_sim_skips != c->skips) {
            printf("FAIL %s: %u skips asked, %d expected\n", c->f.name, scx_sim_skips, c->skips);
            fails++;
        }
        if (c->unskips >= 0 && (int)scx_sim_unskips != c->unskips) {
            printf("FAIL %s: %u Clear skips asked, %d expected\n", c->f.name, scx_sim_unskips, c->unskips);
            fails++;
        }
        if (fails == f0)
            printf("ok   %s\n", c->f.name);
        scx_sim_skips = scx_sim_unskips = 0;
    }
    printf("Plus priority channel and temporary skip UI: %d flows, %d failures\n", flows_run - run_before, fails - before);
    return fails != before;
}
