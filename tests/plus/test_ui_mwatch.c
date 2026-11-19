/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus multi-watch (docs/plus.md "Multi-watch"): Settings > Watch channels 3..6 through the real UI in the
 * upstream simulator, built like tests/plus/test_ui_any.c with -DCONFIG_MULTI_WATCH=1. First upstream's own flows run
 * with it built in (Watch channels 2: dual watch's flows, screens and press budgets unchanged; tools/test_classic.py
 * checks they fail exactly where the build without fails), then the ring's: on with 4 (four rows, the next channels
 * of A's zone, the ring's period), the selection moved with the A/B key, # 1 and a long OK on # 6..9 (PTT and the
 * radio follow), C selected and back, D's channel changed from # 7, a call on C (its row
 * receives, the caller), PTT on the selected one, power off and on, 6 and back to 2, a P25 channel's warning. */
#define main upstream_ui_main
#include "uiFlowTest.c"
#undef main
#include "ui/ui_svc.h"

#define MW4 "mwatch:4 quick 7 "
int main(void)
{
    upstream_ui_main();   /* its own result: tools/test_classic.py compares its failures with the build without */
    int before = fails, run_before = flows_run;
    const flow_t checks[] = {
        /* 2 watch channels: dual watch's two panes, no ring */
        { "MW 2 is dual watch", "mwatch:2", "quick 7", "dual=1 mwn=0 mwradion=2 sel=A bpos=4", 2, false },
        /* 4: four rows, A where the radio is, B..D the next channels of its zone; the ring estimate */
        { "MW on with 4", "mwatch:4", "quick 7",
          "dual=1 mwn=4 mwradion=4 mwsel=A mwradiosel=A apos=3 mwposb=4 mwposc=5 mwposd=6 shows=Ring_0.6_s "
          "shows=COUNTY_DMR shows=County_fire shows=Metro_DMR shows=Hill_top shows=PTT", 2, false },
        /* Right twice: C talks (the radio's selection too), and the UI works on C's channel */
        { "MW select C", MW4, "abkey abkey", "mwsel=C mwradiosel=C uipos=5 toast=Talking_on_C", 2, false },
        { "MW the A/B key round to A", MW4 "abkey abkey abkey", "abkey", "mwsel=A mwradiosel=A uipos=3", 1, false },
        { "MW # 1 talks on the next", MW4 "abkey", "ctx 1", "mwsel=C toast=Talking_on_C", 2, false },
        { "MW long OK on Change D talks on D", MW4, "ctx down down down down down down long:ok",
          "mwsel=D mwradiosel=D uipos=6", 8, false },
        /* # lists Change C and D after Stop (6, 7) */
        { "MW # lists every letter", MW4, "ctx", "screen=dual shows=Change_C shows=Change_D shows=Talk_on_B", 1, false },
        /* D's channel from # 7: the picker on its zone, the next channel */
        { "MW change D", MW4, "ctx 7 down ok", "screen=home mwn=4 mwposd=7 mwsel=A shows=North_link", 4, false },
        { "MW the picker names the others", MW4, "ctx 7", "shows=Watch_on shows=Metro_DMR", 2, false },
        /* Up on the selected C passes D's channel over */
        { "MW Up skips the other letters", MW4 "abkey abkey", "up", "mwsel=C mwposc=7 uipos=7", 1, false },
        /* A call on C: its row receives, the caller is named, the line under the rows says so */
        { "MW a call on C", MW4 "tune:c", "rx:3101234,91,g", "mwrx=C shows=Receiving_on_C shows=K1ABC_Joe", 0, false },
        { "MW the call ends", MW4 "tune:c rx:3101234,91,g", "rxend wait:2000", "mwrx=- shows=Heard", 0, false },
        /* PTT on the selected one (C), the ring goes on after */
        { "MW PTT on C", MW4 "abkey abkey", "pttdown wait:500 pttup wait:500", "dual=1 mwn=4 mwsel=C pos=5", 1, false },
        /* Back twice stops it on the selected one */
        { "MW stop on C", MW4 "abkey abkey", "back back", "dual=0 pos=5 toast=Watch_off:_on_C", 2, false },
        /* Power off and on: the ring again, the selected one under its letter */
        { "MW after power off", MW4 "abkey abkey reboot", "", "dual=1 mwn=4 mwsel=C mwposc=5", 0, false },
        /* 6 channels, then 2 again while it runs */
        { "MW 6 channels", "mwatch:6 quick 7", "", "mwn=6 mwpose=7 mwposf=8 shows=Ring_0.9_s", 0, false },
        { "MW back to 2", MW4 "abkey abkey", "mwatch:2", "dual=1 mwn=0 mwradion=2 sel=B bpos=5", 0, false },
        /* Settings > Talk & listen > Watch channels */
        { "MW the setting", "", "ok 7 3 down down down down down down down down down down down",
          "shows=Watch_channels", 14, false },
    };
    for (unsigned i = 0; i < sizeof checks / sizeof checks[0]; i++)
        run_flow(&checks[i], sim_geom_find("240x320"), SIM_MAP_K, "Plus DM32");
    printf("Plus multi-watch UI: %d flows, %d failures\n", flows_run - run_before, fails - before);
    return fails != before;
}
