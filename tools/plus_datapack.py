#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""DDM32 Plus user data packs (CONFIG_DATAPACK, docs/r14.md 7, docs/plus.md "Data packs"): data the user builds from
sources they download themselves, kept on the data chip in 0xE70000-0xEFFFFF (the one region AGENTS.md rule 2 opens to
the firmware), two slots a pack type, the header written last. Called last from plus_features.prepare(). Our code is in
integration/radiofoundry/datapack/ (installed into core/storage/); upstream files change only through the hunks
below."""

PACK = 'integration/radiofoundry/datapack/'

# (installed path, our file)
FILES = [
    ('core/storage/datapack.h', 'datapack.h'),
    ('core/storage/datapack.c', 'datapack.c'),
]

ON = '#if CONFIG_DATAPACK\n'


def prepare_datapack(transform, install):
    for dst, src in FILES:
        install(dst, PACK + src)

    transform('config/features.mk', [
        ('CONFIG_CLOCK_TOOLS ?= n\nFEATURES += CLOCK_TOOLS\n',
         'CONFIG_CLOCK_TOOLS ?= n\nFEATURES += CLOCK_TOOLS\n'
         'CONFIG_DATAPACK ?= n\nFEATURES += DATAPACK\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_CLOCK_TOOLS\n#define CONFIG_CLOCK_TOOLS 0\n#endif\n',
         '#ifndef CONFIG_CLOCK_TOOLS\n#define CONFIG_CLOCK_TOOLS 0\n#endif\n'
         '#ifndef CONFIG_DATAPACK\n#define CONFIG_DATAPACK 0\t/* DDM32 Plus user data packs (docs/r14.md 7) */\n#endif\n'),
    ])
    # Which pack is in force: checked at boot, once the storage layer is up (a power cut's sector rewrite finished)
    transform('core/app/app.c', [
        ('#include "storage/storage.h"\n',
         '#include "storage/storage.h"\n' + ON + '#include "storage/datapack.h"\n#endif\n'),
        ('\tstorage_init();\t\t\t// finishes a codeplug sector rewrite a power cut interrupted\n',
         '\tstorage_init();\t\t\t// finishes a codeplug sector rewrite a power cut interrupted\n'
         + ON + '\tdpk_scan();\t\t\t// DDM32 Plus: the user data packs in force (docs/r14.md 7)\n#endif\n'),
    ])
