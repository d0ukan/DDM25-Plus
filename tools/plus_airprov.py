#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""DDM32 Plus round 16 "airprov" job (docs/plus.md "Round 16: airfields by province, spectrum in colour", branch
r16-airprov): the owner's requests after using v1.0.0-beta.4. Called last from plus_features.prepare(); a module of its
own so the shared prepare list gains one line. Our code is in integration/radiofoundry/air/ (installed by
plus_misc.prepare_screens), toolbox/wfall.* and spectrum/ui_spectrum.c (installed by their own jobs); only the switches
are here.

  AIR_PROV  Tools > Airfields grouped by province, as Tools > Repeaters is (CONFIG_RPT_PROV): "All" and the provinces
            that have airfields in plate order with their counts, then that province's airfields nearest first. Each
            airfield's province comes from tools/airfields.py (the repeater list's lookup on the map's outlines, one
            byte in what was air_field_t's tail padding). Needs AIRFIELDS and MAP (the names).
  SPECTRUM_COLOR  Tools > Spectrum's waterfall and 3D view in colour on a colour panel: each level in a signal strength
            ramp (dark blue, blue, cyan, green, yellow, orange, red; the light theme its own shades), within the 16
            colour palette of the DM-32's bpp 4 panel (integration/radiofoundry/toolbox/wfall.c wf_ramp). Mono panels
            unchanged. Needs SPECTRUM_WF.
"""


def prepare_airprov(transform, install):
    # The switch, after the round 14 air items' own lines (AIRFIELDS and MAP are set by then); off where a build lacks
    # what it needs
    transform('config/features.mk', [
        ('override CONFIG_SPECTRUM_WF3D := n\nendif\n',
         'override CONFIG_SPECTRUM_WF3D := n\nendif\n'
         'CONFIG_AIR_PROV ?= n\nFEATURES += AIR_PROV\n'
         'ifneq ($(CONFIG_AIRFIELDS)$(CONFIG_MAP),yy)\noverride CONFIG_AIR_PROV := n\nendif\n'
         'CONFIG_SPECTRUM_COLOR ?= n\nFEATURES += SPECTRUM_COLOR\n'
         'ifneq ($(CONFIG_SPECTRUM_WF),y)\noverride CONFIG_SPECTRUM_COLOR := n\nendif\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_SPECTRUM_WF3D\n#define CONFIG_SPECTRUM_WF3D 0\t/* DDM32 Plus 3D waterfall (item 45) */\n#endif\n',
         '#ifndef CONFIG_SPECTRUM_WF3D\n#define CONFIG_SPECTRUM_WF3D 0\t/* DDM32 Plus 3D waterfall (item 45) */\n#endif\n'
         '#ifndef CONFIG_AIR_PROV\n#define CONFIG_AIR_PROV 0\t/* DDM32 Plus airfields by province (tools/plus_airprov.py) */\n'
         '#endif\n#ifndef CONFIG_SPECTRUM_COLOR\n#define CONFIG_SPECTRUM_COLOR 0\t/* DDM32 Plus spectrum in colour '
         '(tools/plus_airprov.py) */\n#endif\n'),
    ])
