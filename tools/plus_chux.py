# SPDX-License-Identifier: GPL-3.0-or-later
"""DDM32 Plus round 15 "channels" job (branch r15-channels, docs/plus.md "Channels, zones and DMR rows"): the owner's
reports after v1.0.0-beta.3, behind CONFIG_CHAN_EXTRAS. Called last from plus_features.prepare(); a module of its own so
the shared prepare list gains one line. Upstream files change only through the hunks below.

  1. Zone size: the zone and codeplug limits shown where channels are added, a full zone refused with its number.
"""

ON = '#if CONFIG_CHAN_EXTRAS\n'


def _if(new, old=None):
    return ON + new + ('#else\n' + old if old is not None else '') + '#endif\n'


def prepare_chux(transform, install):
    transform('config/features.mk', [
        ('CONFIG_DATAPACK ?= n\nFEATURES += DATAPACK\n',
         'CONFIG_DATAPACK ?= n\nFEATURES += DATAPACK\n'
         'CONFIG_CHAN_EXTRAS ?= n\nFEATURES += CHAN_EXTRAS\n'
         'ifeq ($(CONFIG_CHAN_EXTRAS),y)\nifneq ($(CONFIG_ZONE_EDIT)$(CONFIG_CHANNEL_EDIT),yy)\n'
         '$(error CHAN_EXTRAS needs ZONE_EDIT=y and CHANNEL_EDIT=y)\nendif\nendif\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_DATAPACK\n',
         '#ifndef CONFIG_CHAN_EXTRAS\n#define CONFIG_CHAN_EXTRAS 0\t/* DDM32 Plus r15 channels (tools/plus_chux.py) */\n#endif\n'
         '#ifndef CONFIG_DATAPACK\n'),
    ])
    prepare_zone_size(transform)


def prepare_zone_size(transform):
    """1. Zone size (owner: "I can add a 27th channel to a zone, but on Home I can only go up to 26"). The limits are
    the codeplug's: 80 channels a zone (codeplug_zone_capacity; 16 in a codeplug with the old 16-channel zone records),
    68 zones, 1024 channels; nothing in the codeplug, the zone editor or the radio core stops below them
    (tests/plus/test_zone_cap.c, test_zone_radio.inc step through 80). Shown where channels are added: the zone list's
    "Add channel" row "26/80" (dimmed when full), the channel editor's Zones list "26/80" beside the zones it is not in,
    Zones > Create New Zone "5/68"; a full zone refuses with "Zone full: max 80 channels" (Add channel, Copy here,
    the channel editor's Zones; the last one's toast was untranslated)."""
    transform('core/ui/ui_menus.c', [
        # "Zone full: max 80 channels", in the zone editor's toast buffer; "26/80"
        ('\tsb_str(&b, name);\n\tui_toast(ze_msg);\n}\n#endif\n',
         '\tsb_str(&b, name);\n\tui_toast(ze_msg);\n}\n'
         + _if('// DDM32 Plus: a zone at its capacity, with the number (codeplug_zone_capacity: 80, or 16 in an old codeplug)\n'
               'static void ze_full_toast(void)\n{\n\tsbuf_t b;\n\tsb_init(&b, ze_msg, sizeof ze_msg);\n'
               '\tsb_str(&b, TR("Zone full: max "));\n\tsb_u32(&b, codeplug_zone_capacity(), 1);\n'
               '\tsb_str(&b, TR(" channels"));\n\tui_toast(ze_msg);\n}\n')
         + '#endif\n'),
        ('static bool chans_is_new(uint16_t raw)',
         _if('// DDM32 Plus: "26/80", n against a capacity\n'
             'static void sb_of(sbuf_t *b, uint32_t n, uint32_t cap)\n{\n\tsb_u32(b, n, 1);\n\tsb_chr(b, \'/\');\n'
             '\tsb_u32(b, cap, 1);\n}\n')
         + 'static bool chans_is_new(uint16_t raw)'),
        ('\t\t\tui_toast(TR("Zone full"));\n', _if('\t\t\tze_full_toast();\n', '\t\t\tui_toast(TR("Zone full"));\n')),
        ('{\n\t\tui_toast(TR("Zone full"));\n', '{\n' + _if('\t\tze_full_toast();\n', '\t\tui_toast(TR("Zone full"));\n')),
        ('\t\tui_toast("Zone full");\n', _if('\t\tze_full_toast();\n', '\t\tui_toast("Zone full");\n')),
        # The channel editor's Zones: "26/80" beside a zone the channel is not in (dimmed at the capacity)
        ('\tstr_copy(it->value, sizeof it->value, at >= 0 ? TR("In") : "-");\n',
         _if('\tif (at < 0) {\n\t\tsbuf_t b;\n\t\tsb_init(&b, it->value, sizeof it->value);\n'
             '\t\tsb_of(&b, chz_z.count, codeplug_zone_capacity());\n'
             '\t\tif (chz_z.count >= codeplug_zone_capacity())\n\t\t\tit->flags |= UI_ITEM_DIM;\n'
             '\t\treturn;\n\t}\n')
         + '\tstr_copy(it->value, sizeof it->value, at >= 0 ? TR("In") : "-");\n'),
        # The zone list's Add channel: "26/80", dimmed when full
        ('\tif (zone_edit_row(raw) != NULL) {\n\t\tstr_copy(it->label, sizeof it->label, zone_edit_row(raw));\n'
         '\t\treturn;\n\t}\n',
         '\tif (zone_edit_row(raw) != NULL) {\n\t\tstr_copy(it->label, sizeof it->label, zone_edit_row(raw));\n'
         + _if('\t\tif (raw == 0) {\n\t\t\tsbuf_t b;\n\t\t\tsb_init(&b, it->value, sizeof it->value);\n'
               '\t\t\tsb_of(&b, z->count, codeplug_zone_capacity());\n'
               '\t\t\tif (z->count >= codeplug_zone_capacity())\n\t\t\t\tit->flags |= UI_ITEM_DIM;\n\t\t}\n')
         + '\t\treturn;\n\t}\n'),
        # A zone position whose channel can't be read (Home skips it: the radio can't tune it) shows instead of
        # vanishing from the list: "27 ? Ch 123", "Can't be read"; its # menu still removes it
        ('\tuint16_t pos = (uint16_t)(raw - ZE_ADD);\n\tif (pos >= z->count || !codeplug_channel(z->channels[pos], &ch))\n'
         '\t\treturn false;\n',
         '\tuint16_t pos = (uint16_t)(raw - ZE_ADD);\n'
         + _if('\tif (pos < z->count && !codeplug_channel(z->channels[pos], &ch)) {\n'
               '\t\tsb_u32(&b, pos + 1u, 1);\n\t\tsb_str(&b, " ? Ch ");\n\t\tsb_u32(&b, z->channels[pos], 1);\n'
               '\t\treturn true;\n\t}\n')
         + '\tif (pos >= z->count || !codeplug_channel(z->channels[pos], &ch))\n\t\treturn false;\n'),
        ('\tif (raw >= z->count || !codeplug_channel(z->channels[raw], &ch))\n\t\treturn;\n',
         _if('\tif (raw < z->count && !codeplug_channel(z->channels[raw], &ch)) {\n'
             '\t\tsbuf_t ub;\n\t\tsb_init(&ub, it->label, sizeof it->label);\n\t\tsb_u32(&ub, raw + 1u, 2);\n'
             '\t\tsb_str(&ub, " ? Ch ");\n\t\tsb_u32(&ub, z->channels[raw], 1);\n'
             '\t\tstr_copy(it->value, sizeof it->value, TR("Can\'t be read"));\n\t\tit->flags |= UI_ITEM_WARN;\n'
             '\t\treturn;\n\t}\n')
         + '\tif (raw >= z->count || !codeplug_channel(z->channels[raw], &ch))\n\t\treturn;\n'),
        # Add channel on a full zone: refused at once (the list would offer channels it can't take)
        ('\t\tze_zone = zi;\n\t\tui_push(raw == 0 ? SCR_ZPICK : SCR_ZONEEDIT, zi);\n',
         '\t\tze_zone = zi;\n'
         + _if('\t\tif (raw == 0 && zone_of_frame()->count >= codeplug_zone_capacity()) {\n\t\t\tze_full_toast();\n'
               '\t\t\treturn;\n\t\t}\n')
         + '\t\tui_push(raw == 0 ? SCR_ZPICK : SCR_ZONEEDIT, zi);\n'),
        # Zones > Create New Zone: "5/68"
        ('\tif (chans_is_new(raw)) {\n\t\tstr_copy(it->label, sizeof it->label, TR("Create New Zone"));\n\t\treturn;\n\t}\n',
         '\tif (chans_is_new(raw)) {\n\t\tstr_copy(it->label, sizeof it->label, TR("Create New Zone"));\n'
         + _if('\t\tsb_of(&vb, codeplug_zones_stored() ? codeplug_zone_count() : 0u, CODEPLUG_ZONES_MAX);\n')
         + '\t\treturn;\n\t}\n'),
    ])

