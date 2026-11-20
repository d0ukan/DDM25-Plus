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
    ('core/ui/air_pack.h', 'air_pack.h'),
    ('core/ui/air_pack.c', 'air_pack.c'),
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

    # A DMR caller's name: the codeplug's contact, then the user's RadioID pack, then the built-in table (owner
    # build), then the CPS-uploaded database (radio_sm.c find_name)
    transform('core/radio/radio_sm.c', [
        ('#include "codeplug/dmr_iddb.h"\n',
         '#include "codeplug/dmr_iddb.h"\n' + ON + '#include "storage/datapack.h"\n#endif\n'),
        ('#if CONFIG_ID_DB\n\t} else if (!group && iddb_text(id, n->name, sizeof(n->name))) {\n',
         ON + '\t} else if (!group && dpk_id_text(id, n->name, sizeof(n->name))) {\n'
         '\t\t// DDM32 Plus: the user\'s RadioID data pack (docs/r14.md 7)\n#endif\n'
         '#if CONFIG_ID_DB\n\t} else if (!group && iddb_text(id, n->name, sizeof(n->name))) {\n'),
    ])
    # Settings > About: each pack type's line ("RadioID pack: RadioID TR, 2026-11-17, 98 KB", "none", "damaged")
    transform('core/ui/ui_menus.c', [
        ('#include "rfoundry_about.h"\n', '#include "rfoundry_about.h"\n' + ON + '#include "storage/datapack.h"\n#endif\n'),
        ('\tstatic char t[640 + 48 * CONFIG_ID_DB];', '\tstatic char t[640 + 48 * CONFIG_ID_DB + 112 * CONFIG_DATAPACK];'),
        ('\tsb_str(&b, TR("AMBE+2 codec: DVSI (stock)"));',
         ON + '\tfor (uint8_t k = 1; k <= DPK_TYPES; k++) {\t// DDM32 Plus: the user data packs (docs/r14.md 7)\n'
         '\t\tchar d[40];\n'
         '\t\tsb_str(&b, k == DPK_RADIOID ? TR("RadioID pack: ") : TR("Air band pack: "));\n'
         '\t\tsb_str(&b, dpk_about(k, d, sizeof d) ? d : dpk_state(k) == DPK_BAD ? TR("damaged, not used") : TR("none"));\n'
         '\t\tsb_chr(&b, \'\\n\');\n\t}\n#endif\n'
         '\tsb_str(&b, TR("AMBE+2 codec: DVSI (stock)"));'),
    ])
