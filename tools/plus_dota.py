#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""DDM32 Plus DOTA Transfer (docs/dota.md): channels, zones and the DMR contacts / RX group lists they use, sent from
one radio to another over FM (AFSK 1200 in AX.25 UI frames, CTCSS) or DMR (unconfirmed UDP over the data service).
Called last from plus_features.prepare(); a module of its own so the shared prepare list gains one line. Our code is
in integration/radiofoundry/dota/ (installed into core/radio/, core/codeplug/ and core/ui/); upstream files change only
through the hunks below. Switch CONFIG_DOTA (needs APRS, APRS_RX, DMR and ZONE_EDIT)."""

DOTA = 'integration/radiofoundry/dota/'

# (installed path, our file)
FILES = [
    ('core/radio/dota_proto.h', 'dota_proto.h'),
    ('core/radio/dota_proto.c', 'dota_proto.c'),
    ('core/radio/dota_rec.h', 'dota_rec.h'),
    ('core/radio/dota_rec.c', 'dota_rec.c'),
    ('core/radio/dota_import.h', 'dota_import.h'),
    ('core/radio/dota_import.c', 'dota_import.c'),
    ('core/codeplug/codeplug_dota.inc', 'codeplug_dota.inc'),
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

    # The codeplug's staging (docs/dota.md 10, codeplug_dota.inc): codeplug_set_channel's body, able to leave the
    # in-use bit (and the serial) alone; the staging and commit calls at the end of the file
    transform('core/codeplug/codeplug.c', [
        ('bool codeplug_set_channel(uint16_t number, const channel_t *ch)\n{\n\tuint8_t r[CH_SIZE];',
         '// live false (DDM32 Plus DOTA staging, codeplug_dota.inc): the record and its tag, not its in-use bit\n'
         'static bool set_channel_as(uint16_t number, const channel_t *ch, bool live)\n{\n\tuint8_t r[CH_SIZE];'),
        ('\tif (ok && !is_used(number)) {\n\t\tset_used(number, true);\t\t// the record first, then its in-use bit\n',
         '\tif (ok && live && !is_used(number)) {\n\t\tset_used(number, true);\t\t// the record first, then its in-use bit\n'),
        ('\tif (ok) {\n\t\tfind_max();\n\t\tserial++;\n\t}\n\tstorage_unlock();\n\treturn ok;\n}\n\n'
         'bool codeplug_delete_channel(uint16_t number)\n',
         '\tif (ok && live) {\n\t\tfind_max();\n\t\tserial++;\n\t}\n\tstorage_unlock();\n\treturn ok;\n}\n\n'
         'bool codeplug_set_channel(uint16_t number, const channel_t *ch)\n{\n\treturn set_channel_as(number, ch, true);\n}\n\n'
         'bool codeplug_delete_channel(uint16_t number)\n'),
        ('\n#include "classic_standard.inc"\n',
         '\n#include "classic_standard.inc"\n'
         '#if CONFIG_DOTA\n#include "codeplug_dota.inc"\t// DDM32 Plus DOTA Transfer: staging and the commit (docs/dota.md 10)\n'
         '#endif\n'),
    ])
