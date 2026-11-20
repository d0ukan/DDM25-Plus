#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""DDM32 Plus round 14 APRS job (docs/plus.md, ROADMAP rows 47 and 49): Mic-E position beacons and the APRS station
page. Called last from plus_features.prepare(); a module of its own so the shared prepare list gains one line. Our
code is in integration/radiofoundry/aprsx/ (installed into core/ui/), plus small changes to our own files (map/ui_map.c,
aprsmsg/ui_aprsmsg.c, common/ui_scratch.h); upstream files change only through the hunks below.

  APRS_MICE    (item 47) Settings > APRS: the beacon format (Standard / Mic-E) and the Mic-E message (M0-M6, En route
               by default; the emergency stays the SOS's own). Manual, automatic, SmartBeaconing and ISS beacons follow
               it; the APRS-IS copy carries the Mic-E destination. Needs APRS_MSG (the encoder).
  APRS_STATION (item 49) Tools > APRS > Stations heard, OK: the station's page (symbol, distance and bearing, position,
               altitude, speed, comment, heard, its messages); OK Go-To follows it, 0 the map centred on it, # a
               message. Needs APRS_RX, MAP and NAV_FOLLOW.
"""

APRSX = 'integration/radiofoundry/aprsx/'


def prepare_aprs(transform, install):
    # --- Item 47: Mic-E position beacons (CONFIG_APRS_MICE) ---
    transform('config/features.mk', [
        ('override CONFIG_DOTA := n\nendif\n',
         'override CONFIG_DOTA := n\nendif\n'
         # needs APRS_MSG (the Mic-E encoder): off where a board's build lacks it (the registry asks every board)
         'CONFIG_APRS_MICE ?= n\nFEATURES += APRS_MICE\n'
         'ifneq ($(CONFIG_APRS)$(CONFIG_APRS_MSG),yy)\noverride CONFIG_APRS_MICE := n\nendif\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_DOTA\n#define CONFIG_DOTA 0\t/* DDM32 Plus DOTA Transfer (docs/dota.md) */\n#endif\n',
         '#ifndef CONFIG_DOTA\n#define CONFIG_DOTA 0\t/* DDM32 Plus DOTA Transfer (docs/dota.md) */\n#endif\n'
         '#ifndef CONFIG_APRS_MICE\n#define CONFIG_APRS_MICE 0\t/* DDM32 Plus Mic-E beacons (docs/plus.md, round 14) */\n'
         '#endif\n'),
    ])
    # The setting: aprs_cfg_t.mice, kept in three spare bits of the APRS settings (older firmware masks them off)
    transform('core/aprs/aprs.h', [
        ('} aprs_cfg_t;',
         '#if CONFIG_APRS_MICE\n'
         '\tuint8_t mice;\t\t// DDM32 Plus: 0 standard position beacons, 1..7 Mic-E with message M0..M6 (mice - 1)\n'
         '#endif\n} aprs_cfg_t;'),
        ('// "Send now": a position beacon at once (on the configured paths)\n',
         '#if CONFIG_APRS_MICE\n'
         '// DDM32 Plus Settings > APRS (any task): the Mic-E code (aprs_cfg_t.mice), and a new one used and saved at once\n'
         '// (past M6: the standard format). Light on the stack: no copy of the configuration\n'
         'uint8_t aprs_mice(void);\nvoid aprs_set_mice(uint8_t code, uint32_t now_ms);\n#endif\n'
         '// "Send now": a position beacon at once (on the configured paths)\n'),
    ])
    transform('core/settings/settings.h', [
        ('#define SETTINGS_APRS2_DMR_GPS\t\t0x20u',
         '// DDM32 Plus Mic-E beacons (CONFIG_APRS_MICE, aprs_cfg_t.mice): 0 the standard position, 1..7 Mic-E with message\n'
         '// M0..M6 (code - 1), in bits older firmware masks off: aprs_flags2 bits 6-7 the code\'s bits 0-1, aprs_misc bit 7\n'
         '// its bit 2. A record without them, or with aprs_stored 0, reads 0\n'
         '#define SETTINGS_APRS2_MICE\t\t0xC0u\n#define SETTINGS_APRS_MISC_MICE\t\t0x80u\n'
         '#define SETTINGS_APRS_MICE_DEFAULT\t2u\t// Mic-E turned on: M1 En route (the owner\'s choice, 2026-11-20)\n'
         'static inline uint8_t settings_aprs_mice(const settings_t *s)\n{\n'
         '\treturn s->aprs_stored != 1 ? 0u : (uint8_t)(((s->aprs_flags2 & SETTINGS_APRS2_MICE) >> 6) |\n'
         '\t\t\t\t\t\t ((s->aprs_misc & SETTINGS_APRS_MISC_MICE) >> 5));\n}\n'
         'static inline void settings_set_aprs_mice(settings_t *s, uint8_t code)\n{\n'
         '\ts->aprs_flags2 = (uint8_t)((s->aprs_flags2 & ~SETTINGS_APRS2_MICE) | ((code & 3u) << 6));\n'
         '\ts->aprs_misc = (uint8_t)((s->aprs_misc & ~SETTINGS_APRS_MISC_MICE) | ((code & 4u) << 5));\n}\n'
         '#define SETTINGS_APRS2_DMR_GPS\t\t0x20u'),
    ])
    transform('core/aprs/aprs.c', [
        ('\tc->ambiguity = amb <= 4 ? amb : 0;\n}\n',
         '\tc->ambiguity = amb <= 4 ? amb : 0;\n#if CONFIG_APRS_MICE\n\tc->mice = settings_aprs_mice(s);\n#endif\n}\n'),
        ('\ts->aprs_stored = 1;\n}\n',
         '#if CONFIG_APRS_MICE\n\tsettings_set_aprs_mice(s, c->mice);\t// DDM32 Plus: after aprs_flags2 and aprs_misc\n#endif\n'
         '\ts->aprs_stored = 1;\n}\n'),
        ('bool aprs_send_now(void)\n{\n',
         '#if CONFIG_APRS_MICE\n'
         'uint8_t aprs_mice(void)\n{\n\treturn a.cfg.mice;\t// one byte: no lock\n}\n\n'
         'void aprs_set_mice(uint8_t code, uint32_t now_ms)\n{\n'
         '\tsettings_t *s = settings_begin();\t// the settings first, then the lock (as aprs_set_cfg)\n'
         '\tLOCK();\n\ta.cfg.mice = code <= APRS_MICE_EMERGENCY ? code : 0u;\n\tcfg_to_settings(&a.cfg, s);\n'
         '\tchanged();\n\tUNLOCK();\n\tsettings_end(now_ms);\n}\n#endif\n\n'
         'bool aprs_send_now(void)\n{\n'),
        ('\tif (v.ambiguity > 4) {\n\t\tv.ambiguity = 4;\n\t}\n',
         '\tif (v.ambiguity > 4) {\n\t\tv.ambiguity = 4;\n\t}\n'
         '#if CONFIG_APRS_MICE\n\tif (v.mice > APRS_MICE_EMERGENCY) {\n\t\tv.mice = 0;\t// past M6: the standard format\n\t}\n#endif\n'),
        # APRS-IS gets the frame's own destination (a Mic-E position carries its latitude there)
        ('static void own_to_is(const char *info, size_t n)\n{\n',
         '#if CONFIG_APRS_MICE\n'
         '// DDM32 Plus: to dst (APRS_TOCALL, or our Mic-E beacon\'s destination)\n'
         'static void own_to_is_to(const char *dst, const char *info, size_t n)\n#else\n'
         'static void own_to_is(const char *info, size_t n)\n#endif\n{\n'),
        ('\tstatic const char mid[] = ">" APRS_TOCALL ",TCPIP*:";\n\tif (k + sizeof(mid) - 1u + n > sizeof(line)) {\n'
         '\t\treturn;\n\t}\n\tmemcpy(line + k, mid, sizeof(mid) - 1u);\n\tk += sizeof(mid) - 1u;\n',
         '#if CONFIG_APRS_MICE\n\tsize_t d = strlen(dst);\n\tif (k + d + 9u + n > sizeof(line)) {\n\t\treturn;\n\t}\n'
         '\tline[k++] = \'>\';\n\tmemcpy(line + k, dst, d);\n\tk += d;\n\tmemcpy(line + k, ",TCPIP*:", 8u);\n\tk += 8u;\n#else\n'
         '\tstatic const char mid[] = ">" APRS_TOCALL ",TCPIP*:";\n\tif (k + sizeof(mid) - 1u + n > sizeof(line)) {\n'
         '\t\treturn;\n\t}\n\tmemcpy(line + k, mid, sizeof(mid) - 1u);\n\tk += sizeof(mid) - 1u;\n#endif\n'),
        ('\thost_is_line(line, k + n);\n}\n',
         '\thost_is_line(line, k + n);\n}\n'
         '#if CONFIG_APRS_MICE\n#define own_to_is(info, n)\town_to_is_to(APRS_TOCALL, (info), (n))\n#endif\n'),
        # The beacon: Mic-E with the message chosen (never the emergency, SOS's own), else the standard report
        ('// --- Beacons (lock held) ---\n\nstatic size_t build_beacon(const fix_t *f)\n',
         '// --- Beacons (lock held) ---\n\n'
         '#if CONFIG_APRS_MICE\n'
         '// DDM32 Plus Mic-E beacons: the destination of the beacon build_beacon made last (APRS_TOCALL or the Mic-E one)\n'
         'static char bdst[APRS_MICE_DST_LEN + 1];\n#endif\n\n'
         'static size_t build_beacon(const fix_t *f)\n'),
        ('\tr.comment = a.comment;\n\treturn aprs_build_position(&r, a.info, sizeof(a.info));\n',
         '\tr.comment = a.comment;\n'
         '#if CONFIG_APRS_MICE\n'
         '\tif (a.cfg.mice != 0 && a.cfg.mice <= APRS_MICE_EMERGENCY) {\t// M0..M6\n'
         '\t\treturn aprs_mice_build(&r.pos, (uint8_t)(a.cfg.mice - 1u), a.comment, bdst, a.info, sizeof(a.info));\n'
         '\t}\n\tstrcpy(bdst, APRS_TOCALL);\n#endif\n'
         '\treturn aprs_build_position(&r, a.info, sizeof(a.info));\n'),
        ('\tif (a.cfg.via != APRS_VIA_DMR && a.block == APRS_OK && queue_frame(K_BEACON, aprs_path_text(a.cfg.path), a.info, n)) {\n',
         '#if CONFIG_APRS_MICE\n'
         '\tif (a.cfg.via != APRS_VIA_DMR && a.block == APRS_OK &&\n'
         '\t    queue_frame_to(K_BEACON, bdst, aprs_path_text(a.cfg.path), a.info, n)) {\n#else\n'
         '\tif (a.cfg.via != APRS_VIA_DMR && a.block == APRS_OK && queue_frame(K_BEACON, aprs_path_text(a.cfg.path), a.info, n)) {\n'
         '#endif\n'),
        ('\t\town_to_is(a.info, n);\n\t\tany = true;\n',
         '#if CONFIG_APRS_MICE\n\t\town_to_is_to(bdst, a.info, n);\n#else\n\t\town_to_is(a.info, n);\n#endif\n\t\tany = true;\n'),
        ('\tif (n != 0 && queue_frame(K_ISS, APRS_ISS_PATH, a.info, n)) {\n',
         '#if CONFIG_APRS_MICE\n\tif (n != 0 && queue_frame_to(K_ISS, bdst, APRS_ISS_PATH, a.info, n)) {\n#else\n'
         '\tif (n != 0 && queue_frame(K_ISS, APRS_ISS_PATH, a.info, n)) {\n#endif\n'),
    ])
    # Settings > APRS (the last group, without a digit: upstream's tests open the others by theirs): the beacon format,
    # and the Mic-E message while the format is Mic-E
    transform('core/ui/ui_svc.h', [
        ('\tUI_SET_COUNT\n',
         '#if CONFIG_APRS_MICE\n'
         '\tUI_SET_APRS_MICE,\t// DDM32 Plus: APRS beacons as Mic-E positions (aprs_cfg_t.mice != 0)\n'
         '\tUI_SET_APRS_MICE_MSG,\t// ... their message, 0..6 = M0..M6 (aprs_cfg_t.mice - 1; M1 En route by default)\n'
         '#endif\n\tUI_SET_COUNT\n'),
    ])
    transform('core/ui/ui_svc_stub.c', [
        ('\t[UI_SET_FM_ID] = 1,\t// DM32 Plus\n#endif\n',
         '\t[UI_SET_FM_ID] = 1,\t// DM32 Plus\n#endif\n'
         '#if CONFIG_APRS_MICE\n\t[UI_SET_APRS_MICE_MSG] = 1,\t// DDM32 Plus: En route\n#endif\n'),
    ])
    transform('core/ui/ui_svc_storage.c', [
        ('#if CONFIG_KEY_ASSIGN\n#include "input/input.h"\n',
         '#if CONFIG_APRS_MICE\n#include "aprs/aprs.h"\n#endif\n#if CONFIG_KEY_ASSIGN\n#include "input/input.h"\n'),
        ('#if CONFIG_FM_ID\n\tcase UI_SET_FM_ID:\t// DM32 Plus: the bit is set for "off" (0 = on, older records too)\n',
         '#if CONFIG_APRS_MICE\n'
         '\tcase UI_SET_APRS_MICE:\t\t// DDM32 Plus: the APRS engine\'s (saved in spare APRS settings bits, settings.h)\n'
         '\t\treturn aprs_mice() != 0;\n'
         '\tcase UI_SET_APRS_MICE_MSG:\n'
         '\t\treturn aprs_mice() != 0 ? aprs_mice() - 1 : (int32_t)SETTINGS_APRS_MICE_DEFAULT - 1;\n'
         '#endif\n'
         '#if CONFIG_FM_ID\n\tcase UI_SET_FM_ID:\t// DM32 Plus: the bit is set for "off" (0 = on, older records too)\n'),
        # Mic-E turned on keeps a message chosen before, else M1 En route; the engine saves it and uses it at once
        ('#if CONFIG_FM_ID\n\tcase UI_SET_FM_ID:\t// DM32 Plus; ui_svc_apply hands it to radio_fmid.c\n',
         '#if CONFIG_APRS_MICE\n'
         '\tcase UI_SET_APRS_MICE:\t\t// DDM32 Plus\n'
         '\t\taprs_set_mice((uint8_t)(value == 0 ? 0u : aprs_mice() != 0 ? aprs_mice() : SETTINGS_APRS_MICE_DEFAULT),\n'
         '\t\t\t      hal_millis());\n\t\treturn true;\n'
         '\tcase UI_SET_APRS_MICE_MSG:\n'
         '\t\tif (value < 0 || value > 6) {\n\t\t\treturn false;\n\t\t}\n'
         '\t\taprs_set_mice((uint8_t)(value + 1), hal_millis());\n\t\treturn true;\n#endif\n'
         '#if CONFIG_FM_ID\n\tcase UI_SET_FM_ID:\t// DM32 Plus; ui_svc_apply hands it to radio_fmid.c\n'),
    ])
    transform('core/ui/ui_menus.c', [
        ('static const char *const n_ptype[] = { TRN("Off"), "Motorola", "Anytone" };\n',
         'static const char *const n_ptype[] = { TRN("Off"), "Motorola", "Anytone" };\n'
         '#if CONFIG_APRS_MICE\n'
         '// DDM32 Plus Settings > APRS: the beacon format, the Mic-E message (M0..M6; the emergency is the SOS\'s own)\n'
         'static const int16_t v_micefmt[] = { 0, 1 };\n'
         'static const char *const n_micefmt[] = { TRN("Standard"), "Mic-E" };\n'
         'static const int16_t v_micemsg[] = { 0, 1, 2, 3, 4, 5, 6 };\n'
         'static const char *const n_micemsg[] = { TRN("Off duty"), TRN("En route"), TRN("In service"), TRN("Returning"),\n'
         '\t\t\t\t\t TRN("Committed"), TRN("Special"), TRN("Priority") };\n#endif\n'),
        ('\n#define GROUP(nm, a)\t\t{ nm, nm, a, sizeof a / sizeof a[0] }\n',
         '\n#if CONFIG_APRS_MICE\nstatic const set_item_t s_aprs[] = {\t// DDM32 Plus\n'
         '\tLIST(TRN("Beacon format"), "Mic-E position compressed", UI_SET_APRS_MICE, v_micefmt, n_micefmt),\n'
         '\tLIST(TRN("Mic-E message"), "status en route off duty", UI_SET_APRS_MICE_MSG, v_micemsg, n_micemsg),\n'
         '};\n#endif\n'
         '\n#define GROUP(nm, a)\t\t{ nm, nm, a, sizeof a / sizeof a[0] }\n'),
        ('GROUP(TRN("Advanced"), s_advanced),\n};\n',
         'GROUP(TRN("Advanced"), s_advanced),\n'
         '#if CONFIG_APRS_MICE\n\tGROUP("APRS", s_aprs),\t// DDM32 Plus: last, without a digit (the others keep theirs)\n#endif\n};\n'),
        ('static bool set_hidden(const set_item_t *s)\n{\n',
         'static bool set_hidden(const set_item_t *s)\n{\n'
         '#if CONFIG_APRS_MICE\n\tif (s->type != ST_ACTION && s->id == UI_SET_APRS_MICE_MSG)\n'
         '\t\treturn ui_svc_setting_get(UI_SET_APRS_MICE) == 0;\t// DDM32 Plus: the message only with Mic-E\n#endif\n'),
    ])

    # --- Item 49: the APRS station page (CONFIG_APRS_STATION) ---
    for f in ('ui_aprsx.c', 'ui_aprsx.h'):
        install('core/ui/' + f, APRSX + f)
    transform('config/features.mk', [
        ('override CONFIG_APRS_MICE := n\nendif\n',
         'override CONFIG_APRS_MICE := n\nendif\n'
         # needs the heard list (APRS_RX), the map's pick (MAP) and Go-To's follow (NAV_FOLLOW): off where one is missing
         'CONFIG_APRS_STATION ?= n\nFEATURES += APRS_STATION\n'
         'ifneq ($(CONFIG_APRS)$(CONFIG_APRS_RX)$(CONFIG_MAP)$(CONFIG_NAV_FOLLOW),yyyy)\n'
         'override CONFIG_APRS_STATION := n\nendif\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_APRS_MICE\n#define CONFIG_APRS_MICE 0\t/* DDM32 Plus Mic-E beacons (docs/plus.md, round 14) */\n'
         '#endif\n',
         '#ifndef CONFIG_APRS_MICE\n#define CONFIG_APRS_MICE 0\t/* DDM32 Plus Mic-E beacons (docs/plus.md, round 14) */\n'
         '#endif\n#ifndef CONFIG_APRS_STATION\n#define CONFIG_APRS_STATION 0\t/* DDM32 Plus APRS station page (round 14) */\n'
         '#endif\n'),
    ])
    transform('core/ui/ui_nav.h', [
        ('\tSCR_COUNT\n} ui_scr_t;',
         '\tSCR_APRS_STATION,\t\t// DDM32 Plus: a heard APRS station\'s page (CONFIG_APRS_STATION, ui_aprsx.c)\n'
         '\tSCR_COUNT\n} ui_scr_t;'),
    ])
    transform('core/ui/ui_menus.c', [
        ('extern const ui_scr_def_t ui_dota_screen, ui_dota_pick_screen;\t// ui_dota.c (DDM32 Plus)',
         'extern const ui_scr_def_t ui_dota_screen, ui_dota_pick_screen;\t// ui_dota.c (DDM32 Plus)\n'
         'extern const ui_scr_def_t ui_aprsx_screen;\t// ui_aprsx.c (DDM32 Plus)'),
        ('\t[SCR_DOTA] = &ui_dota_screen,\n\t[SCR_DOTA_PICK] = &ui_dota_pick_screen,\n#endif\n',
         '\t[SCR_DOTA] = &ui_dota_screen,\n\t[SCR_DOTA_PICK] = &ui_dota_pick_screen,\n#endif\n'
         '#if CONFIG_APRS_STATION\n\t[SCR_APRS_STATION] = &ui_aprsx_screen,\n#endif\n'),
    ])
    transform('core/ui/ui_app.c', [
        ('#include "ui/ui_dota.h"\n#endif\n',
         '#include "ui/ui_dota.h"\n#endif\n#if CONFIG_APRS_STATION\n#include "ui/ui_aprsx.h"\n#endif\n'),
        ('\tif (ui_dota_tick(stack[sp].scr, now))\n\t\tdirty = true;\n#endif\n',
         '\tif (ui_dota_tick(stack[sp].scr, now))\n\t\tdirty = true;\n#endif\n'
         '#if CONFIG_APRS_STATION\n\t// DDM32 Plus: the APRS station page, drawn again once a second (ui_aprsx.c)\n'
         '\tif (ui_aprsx_tick(stack[sp].scr, now))\n\t\tdirty = true;\n#endif\n'),
    ])
    # Tools > APRS > Stations heard: OK opens the station's page (Go-To is one key there)
    transform('core/ui/ui_aprs.c', [
        ('\tif (ui_svc_aprs_station((uint8_t)i, &s)) {\n\t\tui_follow_start(s.call);\n\t}\n',
         '\tif (ui_svc_aprs_station((uint8_t)i, &s)) {\n#if CONFIG_APRS_STATION\n'
         '\t\tui_aprsx_open(s.call);\t// DDM32 Plus: its page (ui_aprsx.c)\n#else\n'
         '\t\tui_follow_start(s.call);\n#endif\n\t}\n'),
        ('#if CONFIG_NAV_FOLLOW\n#include "ui/follow.h"\n',
         '#if CONFIG_NAV_FOLLOW\n#include "ui/follow.h"\n#if CONFIG_APRS_STATION\n#include "ui/ui_aprsx.h"\n#endif\n'),
        ('.prefix = TRN("Heard"), .hint = TRN("OK follow  Back"),\n',
         '.prefix = TRN("Heard"),\n#if CONFIG_APRS_STATION\n\t.hint = TRN("OK details  Back"),\n#else\n'
         '\t.hint = TRN("OK follow  Back"),\n#endif\n'),
    ])
