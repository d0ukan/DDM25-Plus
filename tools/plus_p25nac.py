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
- Home's VFO row shows the TX NAC when it differs: "NAC $293>$659";
- the NAC rows' decimal entry and "= 659" (CONFIG_NAC_DEC) and # as Any / Same as RX (CONFIG_P25_ANY), in
  plus_features.py;
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

    # ---- Home's VFO row: "NAC $293>$659" when the TX NAC differs ----
    transform('core/ui/ui_screens.c', [
        ('\tstatic char buf[72];\n\tuint8_t m = vfox_mode_of(&v->ch);\n',
         _if('\tstatic char buf[96];\t// three forms, the TX NAC in each\n', '\tstatic char buf[72];\n')
         + '\tuint8_t m = vfox_mode_of(&v->ch);\n'),
        ('\t\t\t\t\tsb_chr(&b, "0123456789ABCDEF"[(c->p25_nac >> (4 * (2 - d))) & 15u]);\n\t\t\t}\n\t\t\tsb_chr(&b, \' \');\n',
         '\t\t\t\t\tsb_chr(&b, "0123456789ABCDEF"[(c->p25_nac >> (4 * (2 - d))) & 15u]);\n\t\t\t}\n'
         + _if('\t\t\tuint16_t tx = codeplug_p25_tx_nac(c);\n'
               '\t\t\tif (c->p25_tx_nac_override && tx != c->p25_nac) {\t// DM32 Plus: a TX NAC of its own, "NAC $293>$659"\n'
               '\t\t\t\tsb_chr(&b, \'>\');\n\t\t\t\tif (form < 2)\n\t\t\t\t\tsb_chr(&b, \'$\');\n'
               '\t\t\t\tfor (unsigned d = 0; d < 3; d++)\n'
               '\t\t\t\t\tsb_chr(&b, "0123456789ABCDEF"[(tx >> (4 * (2 - d))) & 15u]);\n\t\t\t}\n')
         + '\t\t\tsb_chr(&b, \' \');\n'),
    ])

    # ---- The simulator (host UI tests): the "txnac" key (the TX NAC itself; 0 = Same as RX) ----
    transform('tests/host/sim/ui_sim.c', [
        ('\telse if (strcmp(key, "nac") == 0) snprintf(got, sizeof got, "%X", view.ch.p25_nac);\n',
         '\telse if (strcmp(key, "nac") == 0) snprintf(got, sizeof got, "%X", view.ch.p25_nac);\n'
         + _if('\telse if (strcmp(key, "txnac") == 0) snprintf(got, sizeof got, "%X",\n'
               '\t\t\t\t\t\t      view.ch.p25_tx_nac_override ? view.ch.p25_tx_nac_override - 1u : 0u);\n')),
    ])
