#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""DDM32 Plus DOTA Transfer (docs/dota.md): channels, zones and the DMR contacts / RX group lists they use, sent from
one radio to another over FM (AFSK 1200 in AX.25 UI frames, CTCSS) or DMR (unconfirmed UDP over the data service).
Called last from plus_features.prepare(); a module of its own so the shared prepare list gains one line. Our code is
in integration/radiofoundry/dota/ (installed into core/radio/ and core/ui/); upstream files change only through the
hunks below. Switch CONFIG_DOTA (needs APRS, APRS_RX, DMR and ZONE_EDIT)."""

DOTA = 'integration/radiofoundry/dota/'

# (installed path, our file)
FILES = [
    ('core/radio/dota_proto.h', 'dota_proto.h'),
    ('core/radio/dota_proto.c', 'dota_proto.c'),
]


def prepare_dota(transform, install):
    for dst, src in FILES:
        install(dst, DOTA + src)

    # The switch, after Clock tools' (the end of the Plus switches)
    transform('config/features.mk', [
        ('CONFIG_CLOCK_TOOLS ?= n\nFEATURES += CLOCK_TOOLS\n',
         'CONFIG_CLOCK_TOOLS ?= n\nFEATURES += CLOCK_TOOLS\n'
         'CONFIG_DOTA ?= n\nFEATURES += DOTA\n'
         # needs APRS, APRS_RX, DMR and ZONE_EDIT: off where a board's build lacks one (the registry asks every board)
         'ifneq ($(CONFIG_APRS)$(CONFIG_APRS_RX)$(CONFIG_DMR)$(CONFIG_ZONE_EDIT),yyyy)\n'
         'override CONFIG_DOTA := n\nendif\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_CLOCK_TOOLS\n#define CONFIG_CLOCK_TOOLS 0\n#endif\n',
         '#ifndef CONFIG_CLOCK_TOOLS\n#define CONFIG_CLOCK_TOOLS 0\n#endif\n'
         '#ifndef CONFIG_DOTA\n#define CONFIG_DOTA 0\t/* DDM32 Plus DOTA Transfer (docs/dota.md) */\n#endif\n'),
    ])
