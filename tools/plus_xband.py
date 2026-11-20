#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""DDM32 Plus round 14 "xband" job (docs/plus.md "Last calls"; ROADMAP rows 43 and 46). Called last from
plus_features.prepare(); a module of its own so the shared prepare list gains one line. Our code is in
integration/radiofoundry/lastcall/ (installed into core/ui/); upstream files change only through the hunks below.

  LAST_CALLS  (item 43) Tools > Last call becomes a list of the last 8 calls from every mode (DMR, P25, YSF, NXDN, dPMR,
              FM caller IDs) and every watched channel (the multi-watch letter A-F, dual watch's A / B): letter, mode,
              caller, talkgroup or unit, how long ago. The newest P25 call still plays again (CALL_REPLAY's frames,
              unchanged); the other modes keep no audio (owner, 2026-11-20). Fed where the heard list is fed. Needs
              CALL_LOG and CALL_REPLAY.
"""

LASTCALL = 'integration/radiofoundry/lastcall/'

LC_ON = '#if CONFIG_LAST_CALLS\n'


def prepare_last_calls(transform, install):
    for f in ('lastcall.c', 'lastcall.h', 'ui_lastcall.c', 'ui_lastcall.h'):
        install('core/ui/' + f, LASTCALL + f)

    # The switch, after DOTA's (the end of the Plus switches)
    transform('config/features.mk', [
        ('CONFIG_DOTA ?= n\nFEATURES += DOTA\n',
         'CONFIG_DOTA ?= n\nFEATURES += DOTA\n'
         'CONFIG_LAST_CALLS ?= n\nFEATURES += LAST_CALLS\n'
         'ifeq ($(CONFIG_LAST_CALLS),y)\nifneq ($(CONFIG_CALL_LOG)$(CONFIG_CALL_REPLAY),yy)\n'
         '$(error LAST_CALLS needs CALL_LOG=y and CALL_REPLAY=y)\nendif\nendif\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_DOTA\n',
         '#ifndef CONFIG_LAST_CALLS\n#define CONFIG_LAST_CALLS 0\t/* DDM32 Plus last calls (docs/plus.md) */\n#endif\n'
         '#ifndef CONFIG_DOTA\n'),
    ])

    # --- The screen: Tools > Last call opens the list; the replay moved into its OK (ui_lastcall.c lc_select) ---
    transform('core/ui/ui_nav.h', [
        ('\tSCR_COUNT\n} ui_scr_t;',
         '\tSCR_LASTCALL,\t\t\t// DDM32 Plus: Tools > Last call, the last calls (CONFIG_LAST_CALLS)\n'
         '\tSCR_COUNT\n} ui_scr_t;'),
    ])
    transform('core/ui/ui_menus.c', [
        ('extern const ui_scr_def_t ui_clock_screen;\t// ui_clock.c (DDM32 Plus)',
         'extern const ui_scr_def_t ui_clock_screen;\t// ui_clock.c (DDM32 Plus)\n'
         'extern const ui_scr_def_t ui_lastcall_screen;\t// ui_lastcall.c (DDM32 Plus)'),
        ('#if CONFIG_CLOCK_TOOLS\n\t[SCR_CLOCK] = &ui_clock_screen,\n#endif\n',
         '#if CONFIG_CLOCK_TOOLS\n\t[SCR_CLOCK] = &ui_clock_screen,\n#endif\n'
         + LC_ON + '\t[SCR_LASTCALL] = &ui_lastcall_screen,\n#endif\n'),
        ('#if CONFIG_CALL_REPLAY\n\tcase T_REPLAY: {\n',
         LC_ON + '\tcase T_REPLAY: ui_push(SCR_LASTCALL, 0); break;\t// DDM32 Plus: the list (ui_lastcall.c)\n'
         '#elif CONFIG_CALL_REPLAY\n\tcase T_REPLAY: {\n'),
    ])

    # --- The UI task's hooks: where the heard list is fed (plus_toolbox.py's lines, there whatever HEARD is) ---
    transform('core/ui/ui_app.c', [
        ('#if CONFIG_HEARD\n#include "ui/ui_heard.h"\n#endif\n',
         '#if CONFIG_HEARD\n#include "ui/ui_heard.h"\n#endif\n' + LC_ON + '#include "ui/ui_lastcall.h"\n#endif\n'),
        ('#if CONFIG_HEARD\n\tui_heard_recent(r, true);\t// DDM32 Plus heard list (ui_heard.c)\n#endif\n',
         '#if CONFIG_HEARD\n\tui_heard_recent(r, true);\t// DDM32 Plus heard list (ui_heard.c)\n#endif\n'
         + LC_ON + '\tui_lastcall_recent(r, true);\t// DDM32 Plus last calls (ui_lastcall.c)\n#endif\n'),
        ('#if CONFIG_HEARD\n\t\t\tui_heard_recent(&recent[0], false);\t// the same station\'s call goes on\n#endif\n',
         '#if CONFIG_HEARD\n\t\t\tui_heard_recent(&recent[0], false);\t// the same station\'s call goes on\n#endif\n'
         + LC_ON + '\t\t\tui_lastcall_recent(&recent[0], false);\n#endif\n'),
        ('#if CONFIG_HEARD\n\t\t\t\t\tui_heard_recent(&recent[i], false);\t// the P25 call goes on\n#endif\n',
         '#if CONFIG_HEARD\n\t\t\t\t\tui_heard_recent(&recent[i], false);\t// the P25 call goes on\n#endif\n'
         + LC_ON + '\t\t\t\t\tui_lastcall_recent(&recent[i], false);\n#endif\n'),
        ('#if CONFIG_HEARD\n\tui_heard_view(v);\t// DDM32 Plus: what the calls below are heard on\n#endif\n',
         '#if CONFIG_HEARD\n\tui_heard_view(v);\t// DDM32 Plus: what the calls below are heard on\n#endif\n'
         + LC_ON + '\tui_lastcall_view(v);\t// and on which watched channel (ui_lastcall.c)\n#endif\n'),
        ('#if CONFIG_HEARD\n\tui_heard_reset();\n#endif\n',
         '#if CONFIG_HEARD\n\tui_heard_reset();\n#endif\n' + LC_ON + '\tui_lastcall_reset();\n#endif\n'),
    ])

    # The watched channel the radio is tuned to (the ring's letter, dual watch's pane), for the list's letter. The
    # ring's own mw_rx follows a tick later (ui_dual_tick runs after Recent is fed), so this asks the view itself
    transform('core/ui/ui_dual.c', [
        # (right after multi-watch's own part, whatever follows it: air watch's)
        ('#if CONFIG_MULTI_WATCH\n#include "ui_mwatch.inc"\t// DM32 Plus\n#endif\n',
         '#if CONFIG_MULTI_WATCH\n#include "ui_mwatch.inc"\t// DM32 Plus\n#endif\n'
         + LC_ON +
         '// DDM32 Plus last calls: the watched channel v is tuned to: \'A\'..\'F\' (the ring), \'A\' / \'B\' (dual watch), 0 none\n'
         'char ui_dual_letter_here(const radio_view_t *v);\n'
         'char ui_dual_letter_here(const radio_view_t *v)\n{\n'
         '\tif (!dual()->on || v->hotspot)\n\t\treturn 0;\n'
         '#if CONFIG_MULTI_WATCH\n\tif (mw_ring()) {\n\t\tuint8_t k = mw_tuned(v);\n'
         '\t\treturn k < UI_MW_MAX ? mw_char(k) : 0;\n\t}\n#endif\n'
         '\tuint8_t t = tuned_pane(v);\n\treturn t == 0 ? \'A\' : t == 1 ? \'B\' : 0;\n}\n#endif\n'),
    ])


def prepare_xband(transform, install):
    prepare_last_calls(transform, install)
