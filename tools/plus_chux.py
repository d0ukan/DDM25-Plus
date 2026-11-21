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
         'ifeq ($(CONFIG_CHAN_EXTRAS),y)\nifneq ($(CONFIG_ZONE_EDIT)$(CONFIG_CHANNEL_EDIT)$(CONFIG_DMR_ANY),yyy)\n'
         '$(error CHAN_EXTRAS needs ZONE_EDIT=y, CHANNEL_EDIT=y and DMR_ANY=y)\nendif\nendif\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_DATAPACK\n',
         '#ifndef CONFIG_CHAN_EXTRAS\n#define CONFIG_CHAN_EXTRAS 0\t/* DDM32 Plus r15 channels (tools/plus_chux.py) */\n#endif\n'
         '#ifndef CONFIG_DATAPACK\n'),
    ])
    prepare_zone_size(transform)
    prepare_dmr_rows(transform)
    prepare_dmon(transform)


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



def prepare_dmr_rows(transform):
    """2. The channel editor's DMR rows (owner: "RX group list 'Any' doesn't work sensibly"). Any was digital monitor in
    disguise: it set the call layer's hear_all, so the channel also played private calls between other stations, Home
    showed DMon whatever the user's toggle said, and the user's toggle could not turn it off. Any is now its own
    filter, dmr_listen_filter_t.any_group: every group call on the channel's slot (the talk target, the reply and all
    call as before), nothing else; digital monitor stays the user's. In the editor: Any reads "All TGs TS1" (or
    "All TGs" with timeslot Any); the picker offers it right after None ("Any: all TGs") instead of after
    the 76 lists; picking Any or None says in one line what the channel hears; a channel made DMR gets the first group
    contact as its TX contact (it can't be saved without one) and colour code 1 / timeslot 1 if it had none; an RX only
    DMR channel saves without a TX contact; Direct mode on says what it does."""
    transform('core/dmr/call/dmr_call.h', [
        ('\tbool any_ts;\t\t// DM32 Plus: either timeslot, no slot lock (the first heard holds the call)\n#endif\n',
         '\tbool any_ts;\t\t// DM32 Plus: either timeslot, no slot lock (the first heard holds the call)\n#endif\n'
         + _if('\tbool any_group;\t\t// DDM32 Plus: every group call on the slot (RX group list Any), not others\' private calls\n')),
    ])
    transform('core/dmr/call/dmr_call.c', [
        ('\tif (f->hear_all || tg == DMR_ID_ALL_CALL)\n',
         _if('\tif (f->hear_all || f->any_group || tg == DMR_ID_ALL_CALL)\n', '\tif (f->hear_all || tg == DMR_ID_ALL_CALL)\n')),
    ])
    transform('core/radio/radio_sm.c', [
        ('\tcfg.listen.hear_all = cfg.listen.hear_all || ch->listen_list == CODEPLUG_LIST_ANY;\n',
         _if('\tcfg.listen.any_group = ch->listen_list == CODEPLUG_LIST_ANY;\t// DDM32 Plus: not digital monitor (hear_all)\n',
             '\tcfg.listen.hear_all = cfg.listen.hear_all || ch->listen_list == CODEPLUG_LIST_ANY;\n')),
    ])
    transform('core/ui/ui_menus.c', [
        # "All TGs TS1" / "All TGs"
        ('static void chinfo_item(uint16_t i, ui_item_t *it)\n',
         _if('// DDM32 Plus: what RX group list Any hears, with the slot\n'
             'static void sb_list_any(sbuf_t *b, uint8_t ts)\n{\n'
             '\tif (ts == 1u || ts == 2u) {\n\t\tsb_str(b, TR("All TGs TS"));\n\t\tsb_chr(b, (char)(\'0\' + ts));\n'
             '\t} else {\n\t\tsb_str(b, TR("All TGs"));\n\t}\n}\n\n')
         + 'static void chinfo_item(uint16_t i, ui_item_t *it)\n'),
        ('\t\tif (ch->listen_list == CODEPLUG_LIST_ANY) {\n\t\t\tsb_str(&b, TR("Any (all groups)"));\n\t\t\tbreak;\n\t\t}\n',
         '\t\tif (ch->listen_list == CODEPLUG_LIST_ANY) {\n'
         + _if('\t\t\tsb_list_any(&b, ch->timeslot);\n', '\t\t\tsb_str(&b, TR("Any (all groups)"));\n')
         + '\t\t\tbreak;\n\t\t}\n'),
        # A shorter label, so "All TGs TS1" fits beside it: "RX list"
        ('\tstr_copy(it->label, sizeof it->label, tr(labels[r]));\n',
         '\tstr_copy(it->label, sizeof it->label, tr(labels[r]));\n'
         + _if('\tif (r == CE_LIST)\n\t\tstr_copy(it->label, sizeof it->label, TR("RX list"));\n')),
        # The picker: None, Any, then the lists (raw 1 is Any, raw k + 1 list k)
        ('\tif (ui_frame()->arg != 0 && raw == CODEPLUG_LISTS_MAX + 1u) {\n\t\tname = TR("Any (all groups)");\n'
         '\t\traw = 0;\n\t}\n',
         _if('\tif (ui_frame()->arg != 0 && raw == 1u) {\n\t\tname = TR("Any: all TGs");\n\t\traw = 0;\n'
             '\t} else if (ui_frame()->arg != 0 && raw > 1u) {\n\t\traw--;\n\t}\n',
             '\tif (ui_frame()->arg != 0 && raw == CODEPLUG_LISTS_MAX + 1u) {\n\t\tname = TR("Any (all groups)");\n'
             '\t\traw = 0;\n\t}\n')),
        ('\tif (ui_frame()->arg != 0 && ced_ch.listen_list == CODEPLUG_LIST_ANY)\n'
         '\t\treturn (uint16_t)(CODEPLUG_LISTS_MAX + 1u);\n',
         _if('\tif (ui_frame()->arg != 0)\n'
             '\t\treturn ced_ch.listen_list == CODEPLUG_LIST_ANY ? 1u : ced_ch.listen_list ? (uint16_t)(ced_ch.listen_list + 1u) : 0u;\n',
             '\tif (ui_frame()->arg != 0 && ced_ch.listen_list == CODEPLUG_LIST_ANY)\n'
             '\t\treturn (uint16_t)(CODEPLUG_LISTS_MAX + 1u);\n')),
        ('\telse if (raw == CODEPLUG_LISTS_MAX + 1u)\n\t\tced_ch.listen_list = CODEPLUG_LIST_ANY;\n#endif\n'
         '\telse\n\t\tced_ch.listen_list = raw;\n\tui_pop();\n',
         '\telse if (!CONFIG_CHAN_EXTRAS && raw == CODEPLUG_LISTS_MAX + 1u)\n\t\tced_ch.listen_list = CODEPLUG_LIST_ANY;\n'
         '#endif\n\telse\n\t\tced_ch.listen_list = raw;\n'
         + _if('\tif (ui_frame()->arg != 0) {\t// DDM32 Plus: raw 1 is Any, raw k + 1 list k\n'
               '\t\tced_ch.listen_list = raw == 1u ? CODEPLUG_LIST_ANY : raw ? (uint16_t)(raw - 1u) : 0u;\n'
               '\t\tui_pop();\n'
               '\t\tif (raw <= 1u)\t// what the channel hears, in one line\n'
               '\t\t\tui_toast(raw ? TR("Any: every TG on this slot") : TR("None: only the TX contact\'s TG"));\n'
               '\t\treturn;\n\t}\n')
         + '\tui_pop();\n'),
        # Made DMR: a TX contact and a colour code / slot to start from
        ('\t\t\tc->mode = c->mode == RADIO_MODE_DMR ? RADIO_MODE_FM : RADIO_MODE_DMR;\n\t\tbreak;\n',
         '\t\t\tc->mode = c->mode == RADIO_MODE_DMR ? RADIO_MODE_FM : RADIO_MODE_DMR;\n'
         + _if('\t\tif (c->mode == RADIO_MODE_DMR)\n\t\t\tced_dmr_defaults(c);\n')
         + '\t\tbreak;\n'),
        ('\tif (CONFIG_DMR && c->mode == RADIO_MODE_DMR && c->contact == 0)\n\t\treturn TR("Pick a TX contact first");\n',
         _if('\tif (CONFIG_DMR && c->mode == RADIO_MODE_DMR && c->contact == 0 && !c->rx_only)\t// DDM32 Plus: RX only needs none\n',
             '\tif (CONFIG_DMR && c->mode == RADIO_MODE_DMR && c->contact == 0)\n')
         + '\t\treturn TR("Pick a TX contact first");\n'),
        ('// The first problem that keeps the copy from being saved, as a toast; NULL when it can be saved\n',
         _if('// DDM32 Plus: a channel just made DMR: the first group contact as its TX contact when it has none (it can\'t be\n'
             '// saved without one), colour code 1 and timeslot 1 when it has none of either\n'
             'static void ced_dmr_defaults(channel_t *c)\n{\n'
             '#if CONFIG_DMR\n'
             '\tstatic contact_t k;\t// static: the UI task\'s stack is short\n'
             '\tfor (uint16_t i = 1; c->contact == 0 && i <= CODEPLUG_CONTACTS_MAX; i++)\n'
             '\t\tif (codeplug_contact(i, &k) && k.type == CONTACT_GROUP)\n\t\t\tc->contact = i;\n'
             '#endif\n'
             '\tif (c->color_code > 15u && c->color_code != CODEPLUG_CC_ANY)\n\t\tc->color_code = 1;\n'
             '\tif (c->timeslot == 0u)\n\t\tc->timeslot = 1;\n}\n\n')
         + '// The first problem that keeps the copy from being saved, as a toast; NULL when it can be saved\n'),
        ('\tcase CE_DMO: c->dmo = !c->dmo; break;\n',
         _if('\tcase CE_DMO:\n\t\tc->dmo = !c->dmo;\n\t\tif (c->dmo)\n\t\t\tui_toast(TR("Direct: TX on the RX frequency"));\n\t\tbreak;\n',
             '\tcase CE_DMO: c->dmo = !c->dmo; break;\n')),
    ])


def prepare_dmon(transform):
    """3. Digital monitor (owner: "I turn it on and off and the screen still says DMon"). Home drew DMon for
    v->dmr.hear_all, which a channel with RX list Any kept on whatever the user's toggle said. With item 2 hear_all is
    the user's again (the toggle, the monitor key held); the channel's own Any gets its own chip, "AllTG" (not the
    warning colour: nothing to turn off), and DMon wins when both are on. The toggle says in one line what it does:
    "Digital monitor on: every call" / "Digital monitor off", and on a channel that hears every talkgroup anyway
    "Monitor off: channel hears all TGs"."""
    transform('core/ui/ui_screens.c', [
        ('\t\tx = gfx_text(f, x, L->chips_y, dmon, ui_col.warn) + f->w;\t// digital monitor (FM\'s is "Mon")\n',
         '\t\tx = gfx_text(f, x, L->chips_y, dmon, ui_col.warn) + f->w;\t// digital monitor (FM\'s is "Mon")\n'
         + _if('\telse if (v->ch.listen_list == CODEPLUG_LIST_ANY) {\t// DDM32 Plus: the channel hears every TG on its slot\n'
               '\t\tconst char *any = TR("AllTG");\n'
               '\t\tif (x + gfx_text_width(f, any) <= gfx_width())\n'
               '\t\t\tx = gfx_text(f, x, L->chips_y, any, ui_col.fg) + f->w;\n\t}\n')),
    ])
    transform('core/ui/ui_app.c', [
        ('\treturn on ? TR("Digital monitor on") : TR("Digital monitor off");\n',
         _if('\tif (on)\n\t\treturn TR("Digital monitor on: every call");\n'
             '\treturn view != NULL && view->ch.mode == RADIO_MODE_DMR && view->ch.listen_list == CODEPLUG_LIST_ANY ?\n'
             '\t\tTR("Monitor off: channel hears all TGs") : TR("Digital monitor off");\n',
             '\treturn on ? TR("Digital monitor on") : TR("Digital monitor off");\n')),
    ])
