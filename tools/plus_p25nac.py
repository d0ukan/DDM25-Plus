# SPDX-License-Identifier: GPL-3.0-or-later
"""DDM32 Plus: extras on top of Classic r13's separate RX and TX NAC (CONFIG_P25_TXNAC, docs/plus.md "Separate RX NAC
and TX NAC"; owner's item 38). Called last from plus_features.prepare(); its own module so the shared prepare list gains
one line.

Classic r13 (upstream PR #14) owns the channel's TX NAC: channel_t.p25_tx_nac_override (0 = "Same as RX", else the TX
NAC + 1), stored in the RFKV P25 parameter block with its explicit-versus-inherited presence, the TX rule in
codeplug_p25_valid, the channel editor's "RX NAC" / "TX NAC" rows and the worker's TX NAC (p25_lab_set_channel). Plus's
own storage of the r7 base (0x5000 | NAC in an 0xA5 record's TX tone word) is gone. What Plus adds on top:
- the VFO's TX NAC (RAM only: settings_t has no room left; integration/radiofoundry/vfox), with the editor's TX NAC row
  in VFO settings;
- Home's VFO row shows the TX NAC when it differs, both NACs in decimal since round 15: "NAC 659>1625";
- the NAC rows' decimal entry and display "659 ($293)" (CONFIG_NAC_DEC) and # as Any / Same as RX (CONFIG_P25_ANY),
  in plus_features.py;
- the CH_MODE 2 import keeps a TX NAC unlike the RX one (plus_features.py prepare_p25_mode2)."""

TAG = 'CONFIG_P25_TXNAC'


def _if(new, old=None):
    return f'#if {TAG}\n{new}' + (f'#else\n{old}' if old is not None else '') + '#endif\n'


def prepare_p25_txnac(transform, install):
    transform('config/features.mk', [
        ('CONFIG_NAC_DEC ?= n\nFEATURES += NAC_DEC\n',
         'CONFIG_P25_TXNAC ?= n\nFEATURES += P25_TXNAC\n'
         'ifeq ($(CONFIG_P25_TXNAC),y)\nifneq ($(CONFIG_P25_CHANNELS)$(CONFIG_P25_ANY)$(CONFIG_NAC_DEC)$(CONFIG_CHANNEL_EDIT),yyyy)\n'
         '$(error P25_TXNAC needs P25_CHANNELS=y, P25_ANY=y, NAC_DEC=y and CHANNEL_EDIT=y)\nendif\nendif\n'
         'CONFIG_NAC_DEC ?= n\nFEATURES += NAC_DEC\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_NAC_DEC\n', '#ifndef CONFIG_P25_TXNAC\n#define CONFIG_P25_TXNAC 0\n#endif\n#ifndef CONFIG_NAC_DEC\n'),
    ])

    # ---- Home's VFO row: "NAC 659>1625" when the TX NAC differs (both in decimal since round 15, as the editor) ----
    transform('core/ui/ui_screens.c', [
        ('\tstatic char buf[72];\n\tuint8_t m = vfox_mode_of(&v->ch);\n',
         _if('\tstatic char buf[96];\t// three forms, the TX NAC in each\n', '\tstatic char buf[72];\n')
         + '\tuint8_t m = vfox_mode_of(&v->ch);\n'),
        ('\t\t\t\t\tsb_chr(&b, "0123456789ABCDEF"[(c->p25_nac >> (4 * (2 - d))) & 15u]);\n\t\t\t}\n\t\t\tsb_chr(&b, \' \');\n',
         '\t\t\t\t\tsb_chr(&b, "0123456789ABCDEF"[(c->p25_nac >> (4 * (2 - d))) & 15u]);\n\t\t\t}\n'
         + _if('\t\t\tuint16_t tx = codeplug_p25_tx_nac(c);\n'
               '\t\t\tif (c->p25_tx_nac_override && tx != c->p25_nac) {\t// DM32 Plus: a TX NAC of its own, "NAC 659>1625"\n'
               '\t\t\t\tsb_chr(&b, \'>\');\n\t\t\t\tsb_u32(&b, tx, 1);\t// decimal (CONFIG_NAC_DEC, which P25_TXNAC needs)\n'
               '\t\t\t}\n')
         + '\t\t\tsb_chr(&b, \' \');\n'),
        # The RX NAC in decimal too (round 15: the owner reads and types NACs in decimal)
        ('\t\t\t\tif (form < 2)\n\t\t\t\t\tsb_chr(&b, \'$\');\n\t\t\t\tfor (unsigned d = 0; d < 3; d++)\n'
         '\t\t\t\t\tsb_chr(&b, "0123456789ABCDEF"[(c->p25_nac >> (4 * (2 - d))) & 15u]);\n',
         '#if CONFIG_NAC_DEC\n\t\t\t\tsb_u32(&b, c->p25_nac, 1);\n#else\n'
         '\t\t\t\tif (form < 2)\n\t\t\t\t\tsb_chr(&b, \'$\');\n\t\t\t\tfor (unsigned d = 0; d < 3; d++)\n'
         '\t\t\t\t\tsb_chr(&b, "0123456789ABCDEF"[(c->p25_nac >> (4 * (2 - d))) & 15u]);\n#endif\n'),
    ])

    # ---- Home's VFO row: every NAC in both forms, "NAC $293/659>$659/1625" (owner 2026-11-21: hex with "$" and the
    # decimal wherever a NAC is shown). Forms: "VFO P25 NAC $293/659 TG 99" | "P25 NAC ..." | "NAC$293/659 TG99";
    # Any: "NAC Any $F7E/3966" in the wide forms, "NACAny" in the tight one. A TX NAC of its own that does not fit even
    # so gets a fourth, last-resort form with the decimals alone ("NAC659>1625 TG99", round 15's)
    transform('core/ui/ui_screens.c', [
        ('\tstatic char buf[96];\t// three forms, the TX NAC in each\n',
         '#if CONFIG_NAC_DEC\n\tstatic char buf[144];\t// four forms, both NAC forms and the TX NAC in each\n#else\n'
         '\tstatic char buf[96];\t// three forms, the TX NAC in each\n#endif\n'),
        ('#else\n\tstatic char buf[72];\n#endif\n\tuint8_t m = vfox_mode_of(&v->ch);\n',
         '#elif CONFIG_NAC_DEC\n\tstatic char buf[112];\t// three forms, both NAC forms in each\n'
         '#else\n\tstatic char buf[72];\n#endif\n\tuint8_t m = vfox_mode_of(&v->ch);\n'),
        ('\tfor (int form = 0; form < 3; form++) {\n',
         '#if CONFIG_NAC_DEC\n'
         '\tint forms = m == RADIO_MODE_P25 && c->p25_tx_nac_override && codeplug_p25_tx_nac(c) != c->p25_nac ? 4 : 3;\n'
         '\tfor (int form = 0; form < forms; form++) {\n'
         '#else\n'
         '\tfor (int form = 0; form < 3; form++) {\n'
         '#endif\n'),
        ('\t\t\t\tsb_str(&b, TR("Any"));\n\t\t\t} else {\n#if CONFIG_NAC_DEC\n\t\t\t\tsb_u32(&b, c->p25_nac, 1);\n#else\n',
         '\t\t\t\tsb_str(&b, TR("Any"));\n'
         '#if CONFIG_NAC_DEC\n\t\t\t\tif (form < 2)\n\t\t\t\t\tsb_str(&b, " $F7E/3966");\n#endif\n'
         '\t\t\t} else {\n#if CONFIG_NAC_DEC\n'
         '\t\t\t\tif (form < 3)\n\t\t\t\t\tsb_nac_both(&b, c->p25_nac);\n\t\t\t\telse\n'
         '\t\t\t\t\tsb_u32(&b, c->p25_nac, 1);\n#else\n'),
        ('\t\t\t\tsb_chr(&b, \'>\');\n\t\t\t\tsb_u32(&b, tx, 1);\t// decimal (CONFIG_NAC_DEC, which P25_TXNAC needs)\n',
         '\t\t\t\tsb_chr(&b, \'>\');\n'
         '\t\t\t\tif (form < 3)\n\t\t\t\t\tsb_nac_both(&b, tx);\t// CONFIG_NAC_DEC, which P25_TXNAC needs\n'
         '\t\t\t\telse\n\t\t\t\t\tsb_u32(&b, tx, 1);\n'),
        ('static const char *__attribute__((noinline)) vfox_home(const radio_view_t *v)\n',
         '#if CONFIG_NAC_DEC\n'
         '// DM32 Plus: a NAC in both forms, "$293/659"\n'
         'static void sb_nac_both(sbuf_t *b, unsigned nac)\n{\n\tsb_chr(b, \'$\');\n'
         '\tfor (unsigned d = 0; d < 3; d++)\n\t\tsb_chr(b, "0123456789ABCDEF"[(nac >> (4 * (2 - d))) & 15u]);\n'
         '\tsb_chr(b, \'/\');\n\tsb_u32(b, nac, 1);\n}\n#endif\n'
         'static const char *__attribute__((noinline)) vfox_home(const radio_view_t *v)\n'),
    ])

    # ---- The simulator (host UI tests): the "txnac" key (the TX NAC itself; 0 = Same as RX) ----
    transform('tests/host/sim/ui_sim.c', [
        ('\telse if (strcmp(key, "nac") == 0) snprintf(got, sizeof got, "%X", view.ch.p25_nac);\n',
         '\telse if (strcmp(key, "nac") == 0) snprintf(got, sizeof got, "%X", view.ch.p25_nac);\n'
         + _if('\telse if (strcmp(key, "txnac") == 0) snprintf(got, sizeof got, "%X",\n'
               '\t\t\t\t\t\t      view.ch.p25_tx_nac_override ? view.ch.p25_tx_nac_override - 1u : 0u);\n')),
    ])
