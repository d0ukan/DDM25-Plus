#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""DDM32 Plus round 15 "scan" job (docs/plus.md "Search ranges and speed"): the owner's requests after using
v1.0.0-beta.3. Called last from plus_features.prepare(); a module of its own so the shared prepare list gains one line.
Our code is in integration/radiofoundry/srange/ (installed into core/radio and core/ui), plus changes to our own
screens and engines (spectrum/ui_p25search.c, ui_dmrsearch.c, p25search.*, dmrsearch.*); upstream files change only
through the hunks below.

  SEARCH_RANGE  (item 1) Tools > P25 search and DMR search: a manual range typed in an editor (start, end, a step of
                2.5 kHz to 1 MHz, the range moved by a step or 1 MHz), remembered with the step while the radio is on.
                The step rides on the search request's start frequency (radio/srange.h). Needs P25_SEARCH or
                DMR_SEARCH.
"""

SRANGE = 'integration/radiofoundry/srange/'


def prepare_scan(transform, install):
    install('core/radio/srange.h', SRANGE + 'srange.h')
    for f in ('ui_srange.c', 'ui_srange.h'):
        install('core/ui/' + f, SRANGE + f)

    # The switch, after DMR_SEARCH's own lines (both searches' switches are set by then)
    transform('config/features.mk', [
        ('$(error DMR_SEARCH needs DMR=y and DMR_ANY=y)\nendif\nendif\n',
         '$(error DMR_SEARCH needs DMR=y and DMR_ANY=y)\nendif\nendif\n'
         'CONFIG_SEARCH_RANGE ?= n\nFEATURES += SEARCH_RANGE\n'
         'ifeq ($(CONFIG_SEARCH_RANGE),y)\nifeq ($(CONFIG_P25_SEARCH)$(CONFIG_DMR_SEARCH),nn)\n'
         '$(error SEARCH_RANGE needs P25_SEARCH=y or DMR_SEARCH=y)\nendif\nendif\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_DMR_SEARCH\n#define CONFIG_DMR_SEARCH 0\n#endif\n',
         '#ifndef CONFIG_DMR_SEARCH\n#define CONFIG_DMR_SEARCH 0\n#endif\n'
         '#ifndef CONFIG_SEARCH_RANGE\n#define CONFIG_SEARCH_RANGE 0\t/* DDM32 Plus search ranges (tools/plus_scan.py) */\n'
         '#endif\n'),
    ])

    # Item 1: * on the two search screens opens their range editor, not quick settings
    transform('core/ui/ui_app.c', [
        ('!(CONFIG_MAP_COVERAGE && stack[sp].scr == SCR_MAP)) {',
         '!(CONFIG_MAP_COVERAGE && stack[sp].scr == SCR_MAP) &&\n'
         '\t\t\t!(CONFIG_SEARCH_RANGE && (stack[sp].scr == SCR_P25SEARCH || stack[sp].scr == SCR_DMRSEARCH))) {'),
    ])
    # Item 1: the searches sweep with the step the request's start names (radio/srange.h); untagged: 12.5 kHz
    transform('core/radio/radio_sm.c', [
        ('#if CONFIG_DMR_SEARCH\n#include "radio/dmrsearch.h"\n#endif\n',
         '#if CONFIG_DMR_SEARCH\n#include "radio/dmrsearch.h"\n#endif\n'
         '#if CONFIG_SEARCH_RANGE\n#include "radio/srange.h"\t// DDM32 Plus: the search\'s step rides on its start\n#endif\n'),
        ('\t\tboard.radio->rx(p25s_start((uint32_t)req->arg, req->arg2, 12500u, now_ms), RADIO_BW_12K5, &search_no_tone);\n',
         '#if CONFIG_SEARCH_RANGE\n'
         '\t\tboard.radio->rx(p25s_start(srange_lo_of((uint32_t)req->arg), req->arg2,\n'
         '\t\t\t\t\t   srange_step_hz(srange_tag_of((uint32_t)req->arg)), now_ms), RADIO_BW_12K5, &search_no_tone);\n'
         '#else\n'
         '\t\tboard.radio->rx(p25s_start((uint32_t)req->arg, req->arg2, 12500u, now_ms), RADIO_BW_12K5, &search_no_tone);\n'
         '#endif\n'),
        ('\t\tuint32_t hz = dmrs_start((uint32_t)req->arg, req->arg2, 12500u, now_ms);\n',
         '#if CONFIG_SEARCH_RANGE\n'
         '\t\tuint32_t hz = dmrs_start(srange_lo_of((uint32_t)req->arg), req->arg2,\n'
         '\t\t\t\t\t  srange_step_hz(srange_tag_of((uint32_t)req->arg)), now_ms);\n'
         '#else\n'
         '\t\tuint32_t hz = dmrs_start((uint32_t)req->arg, req->arg2, 12500u, now_ms);\n'
         '#endif\n'),
    ])
