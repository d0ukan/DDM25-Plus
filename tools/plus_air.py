#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""DDM32 Plus round 14 air items (docs/plus.md "Round 14: air band and spectrum", branch r14-air). Called last from
plus_features.prepare(); a module of its own so the shared prepare list gains one line. Our code is in
integration/radiofoundry/air/ (installed into core/radio/ and core/ui/); upstream files and the shared Plus files
change only through the hunks below.

  AIR_WATCH  (item 44b) Tools > Airfields, # on an airfield's page: up to 6 of its frequencies on the multi-watch ring,
             A..F each its own VFO (AM: the air band), hopping as multi-watch does; stopping it brings the user's watch
             back as it was. Needs MULTI_WATCH, VFO_MODES and AIRFIELDS.
  SPECTRUM_WF3D (item 45) Tools > Spectrum's third view: the waterfall's rows as a ridge plot in perspective (* cycles
             waterfall, 3D, bars alone); the waterfall's own heap rows, no new RAM. Needs SPECTRUM_WF.
"""

AIR = 'integration/radiofoundry/air/'
TOOLBOX = 'integration/radiofoundry/toolbox/'


def prepare_air(transform, install):
    install('core/radio/radio_airwatch.inc', AIR + 'radio_airwatch.inc')
    install('core/ui/ui_airwatch.inc', AIR + 'ui_airwatch.inc')
    install('core/ui/wf3d.c', TOOLBOX + 'wf3d.c')
    install('core/ui/wf3d.h', TOOLBOX + 'wf3d.h')

    # The switch, after DOTA's (the end of the Plus switches); off where a build lacks what it needs
    transform('config/features.mk', [
        ('CONFIG_DOTA ?= n\nFEATURES += DOTA\n',
         'CONFIG_DOTA ?= n\nFEATURES += DOTA\n'
         'CONFIG_AIR_WATCH ?= n\nFEATURES += AIR_WATCH\n'
         'ifneq ($(CONFIG_MULTI_WATCH)$(CONFIG_VFO_MODES)$(CONFIG_AIRFIELDS),yyy)\n'
         'override CONFIG_AIR_WATCH := n\nendif\n'
         'CONFIG_SPECTRUM_WF3D ?= n\nFEATURES += SPECTRUM_WF3D\n'
         'ifneq ($(CONFIG_SPECTRUM_WF),y)\noverride CONFIG_SPECTRUM_WF3D := n\nendif\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_DOTA\n#define CONFIG_DOTA 0\t/* DDM32 Plus DOTA Transfer (docs/dota.md) */\n#endif\n',
         '#ifndef CONFIG_DOTA\n#define CONFIG_DOTA 0\t/* DDM32 Plus DOTA Transfer (docs/dota.md) */\n#endif\n'
         '#ifndef CONFIG_AIR_WATCH\n#define CONFIG_AIR_WATCH 0\t/* DDM32 Plus air watch (item 44b) */\n#endif\n'
         '#ifndef CONFIG_SPECTRUM_WF3D\n#define CONFIG_SPECTRUM_WF3D 0\t/* DDM32 Plus 3D waterfall (item 45) */\n#endif\n'),
    ])

    # ---- The radio core (radio_airwatch.inc): two requests, the flag, nothing noted while it runs ----
    transform('core/radio/radio.h', [
        ('bool radio_mwatch_take(radio_dual_ref_t ref[], uint8_t *n, uint8_t *sel);\n',
         'bool radio_mwatch_take(radio_dual_ref_t ref[], uint8_t *n, uint8_t *sel);\n'
         '#if CONFIG_AIR_WATCH\n'
         '// DDM32 Plus air watch (radio_airwatch.inc): VFO idx (0..5, a ring letter\'s own) takes an air band frequency;\n'
         '// the first since the last end keeps the user\'s state as a power off does. The end brings it back\n'
         'void radio_req_air_vfo(uint8_t idx, uint32_t hz);\n'
         'void radio_req_air_end(void);\n'
         '#endif\n'),
    ])
    transform('core/radio/radio_sm.h', [
        ('\tRREQ_MWATCH,\t\t// DM32 Plus multi-watch: the ring radio_req_mwatch left (radio_mwatch_take)\n',
         '\tRREQ_MWATCH,\t\t// DM32 Plus multi-watch: the ring radio_req_mwatch left (radio_mwatch_take)\n'
         '#if CONFIG_AIR_WATCH\n'
         '\tRREQ_AIR_VFO,\t\t// DDM32 Plus air watch: arg VFO 0..5, arg2 its frequency (radio_airwatch.inc)\n'
         '\tRREQ_AIR_END,\t\t// ... the end: the user\'s VFOs and position as kept\n'
         '#endif\n'),
        ('\tuint8_t dual_look;\t\t// ... the one looked at last (the looks take the others in turn)\n',
         '\tuint8_t dual_look;\t\t// ... the one looked at last (the looks take the others in turn)\n'
         '#if CONFIG_AIR_WATCH\n'
         '\tbool air;\t\t\t// DDM32 Plus air watch: the VFOs hold an airfield\'s frequencies, nothing is noted\n'
         '#endif\n'),
    ])
    transform('core/radio/radio_task.c', [
        ('bool radio_mwatch_take(radio_dual_ref_t ref[], uint8_t *n, uint8_t *sel)\n{\n',
         '#if CONFIG_AIR_WATCH\n'
         '// DDM32 Plus air watch: up to six of these and the ring at once; waits a little for room rather than lose one\n'
         'void radio_req_air_vfo(uint8_t idx, uint32_t hz)\n{\n'
         '\tpost_pair(RREQ_AIR_VFO, idx, hz, pdMS_TO_TICKS(20));\n}\n\n'
         'void radio_req_air_end(void)\n{\n'
         '\tpost_pair(RREQ_AIR_END, 0, 0, pdMS_TO_TICKS(20));\n}\n'
         '#endif\n'
         'bool radio_mwatch_take(radio_dual_ref_t ref[], uint8_t *n, uint8_t *sel)\n{\n'),
    ])
    transform('core/radio/radio_sm.c', [
        ('static void persist(radio_sm_t *sm, uint32_t now_ms)\n{\n\tif (sm->hotspot) {\n',
         'static void persist(radio_sm_t *sm, uint32_t now_ms)\n{\n'
         '#if CONFIG_AIR_WATCH\n'
         '\tif (sm->air) {\n'
         '\t\treturn;\t\t// DDM32 Plus air watch: the user\'s state stays as noted before it (radio_airwatch.inc)\n'
         '\t}\n'
         '#endif\n'
         '\tif (sm->hotspot) {\n'),
        ('void radio_sm_request(radio_sm_t *sm, const radio_req_t *req, uint32_t now_ms)\n{\n',
         '#if CONFIG_AIR_WATCH\n#include "radio/radio_airwatch.inc"\t// DDM32 Plus air watch (item 44b)\n#endif\n\n'
         'void radio_sm_request(radio_sm_t *sm, const radio_req_t *req, uint32_t now_ms)\n{\n'),
        ('#if CONFIG_MULTI_WATCH\n\tcase RREQ_MWATCH:\n\t\tmw_request(sm, now_ms);\n\t\tbreak;\n#endif\n',
         '#if CONFIG_MULTI_WATCH\n\tcase RREQ_MWATCH:\n\t\tmw_request(sm, now_ms);\n\t\tbreak;\n#endif\n'
         '#if CONFIG_AIR_WATCH\n\tcase RREQ_AIR_VFO:\n\tcase RREQ_AIR_END:\n\t\tair_request(sm, req, now_ms);\n'
         '\t\tbreak;\n#endif\n'),
    ])

    # ---- The UI (ui_airwatch.inc at the end of ui_dual.c, after ui_mwatch.inc) ----
    transform('core/ui/ui_svc.h', [
        ('bool ui_svc_mwatch(const ui_dual_ref_t ref[], uint8_t n, uint8_t sel);\n',
         'bool ui_svc_mwatch(const ui_dual_ref_t ref[], uint8_t n, uint8_t sel);\n'
         '#if CONFIG_AIR_WATCH\n'
         '// DDM32 Plus air watch: VFO idx (0..5) takes hz, the user\'s state kept (radio_req_air_vfo); the end brings it back\n'
         'void ui_svc_air_vfo(uint8_t idx, uint32_t hz);\n'
         'void ui_svc_air_end(void);\n'
         '#endif\n'),
    ])
    transform('core/ui/ui_svc_stub.c', [
        ('WEAK bool ui_svc_mwatch(const ui_dual_ref_t ref[], uint8_t n, uint8_t sel) { (void)ref; (void)n; (void)sel; return false; }\n',
         'WEAK bool ui_svc_mwatch(const ui_dual_ref_t ref[], uint8_t n, uint8_t sel) { (void)ref; (void)n; (void)sel; return false; }\n'
         '#if CONFIG_AIR_WATCH\n'
         'WEAK void ui_svc_air_vfo(uint8_t idx, uint32_t hz) { (void)idx; (void)hz; }\n'
         'WEAK void ui_svc_air_end(void) {}\n'
         '#endif\n'),
    ])
    transform('core/ui/ui_svc_radio.c', [
        ('bool ui_svc_mwatch(const ui_dual_ref_t ref[], uint8_t n, uint8_t sel)\n{\n',
         '#if CONFIG_AIR_WATCH\n'
         'void ui_svc_air_vfo(uint8_t idx, uint32_t hz)\n{\n\tradio_req_air_vfo(idx, hz);\n}\n\n'
         'void ui_svc_air_end(void)\n{\n\tradio_req_air_end();\n}\n'
         '#endif\n\n'
         'bool ui_svc_mwatch(const ui_dual_ref_t ref[], uint8_t n, uint8_t sel)\n{\n'),
    ])
    transform('core/ui/ui_dual.c', [
        ('static void mw_follow_zones(uint32_t from, uint32_t to);\n#endif\n#else\n',
         'static void mw_follow_zones(uint32_t from, uint32_t to);\n#endif\n'
         '#if CONFIG_AIR_WATCH\n'
         'static bool mw_air;\t\t// DDM32 Plus air watch (ui_airwatch.inc): the ring is an airfield\'s, nothing is saved\n'
         'static bool mw_air_end(void);\n'
         '#define MW_AIR_OFF\tTR("Air watch off")\t// its string here: tools/i18n.py reads .c and .h files only\n'
         '#endif\n'
         '#else\n'),
        ('static void state_save(void)\n{\n\tconst ui_dual_t *d = dual();\n',
         'static void state_save(void)\n{\n'
         '#if CONFIG_AIR_WATCH\n'
         '\tif (mw_air)\n\t\treturn;\t// DDM32 Plus air watch: the user\'s watch stays as saved before it\n'
         '#endif\n'
         '\tconst ui_dual_t *d = dual();\n'),
        ('\tif (!d->on)\n\t\treturn resume_poll(d, v) || adopt_poll(d, v);\n',
         '#if CONFIG_AIR_WATCH\n'
         '\tif (!d->on && mw_air)\n'
         '\t\treturn mw_air_end();\t// DDM32 Plus: the air watch stopped: the user\'s watch as it was\n'
         '#endif\n'
         '\tif (!d->on)\n\t\treturn resume_poll(d, v) || adopt_poll(d, v);\n'),
        ('#if CONFIG_MULTI_WATCH\n#include "ui_mwatch.inc"\t// DM32 Plus\n#endif\n',
         '#if CONFIG_MULTI_WATCH\n#include "ui_mwatch.inc"\t// DM32 Plus\n#endif\n'
         '#if CONFIG_AIR_WATCH\n#include "ui_airwatch.inc"\t// DDM32 Plus air watch (item 44b)\n#endif\n'),
    ])
