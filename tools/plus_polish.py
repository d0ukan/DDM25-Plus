# SPDX-License-Identifier: GPL-3.0-or-later
"""DDM32 Plus round 16 polish (branch r16-polish): the Turkish catalogue complete. Called last from
plus_features.prepare(); its own module so the shared prepare list gains one line.

- tools/i18n.py reads core's .inc files too: their TR() texts (the multi-watch ring line "Ring 0.9 s", "Talk on A",
  "Stop, stay on B", ...) were never in the catalogue, so the radio showed them in English;
- texts the screens show without TR(): upstream's zone toasts and its "In Zones" list, the caller line before the
  first P25 / YSF / NXDN link control, About's "Based on" lines and the repeater data
  source "sample";
- the simulator (host UI tests): with SIM_FIT_LOG set, every text gfx_text_fit shortens with an ellipsis is appended
  to that file ("text<TAB>shown"), to find the texts a language makes too long (docs/plus.md)."""


def prepare_polish(transform, install):
    transform('tools/i18n.py', [
        ('        for ext in ("c", "h"):\n',
         '        for ext in ("c", "h", "inc"):\t# DM32 Plus: and the .inc files core\'s .c files include\n'),
    ])
    transform('core/ui/ui_dual.c', [
        ('caller_buf, "YSF call")', 'caller_buf, TR("YSF call"))'),
        ('caller_buf, "P25 call")', 'caller_buf, TR("P25 call"))'),
    ])
    transform('core/ui/ui_screens.c', [
        ('sb_str(&nb, "call")', 'sb_str(&nb, TR("call"))'),
    ])
    transform('core/ui/ui_menus.c', [
        ('ui_toast("Zone full")', 'ui_toast(TR("Zone full"))'),
        ('ui_toast("Give it a name first")', 'ui_toast(TR("Give it a name first"))'),
        ('ui_toast("Couldn\'t save: try again")', 'ui_toast(TR("Couldn\'t save: try again"))'),
        ('.prefix = "In Zones", .hint = "OK in/out  Back", .empty = "No zones"',
         '.prefix = TRN("In Zones"), .hint = TRN("OK in/out  Back"), .empty = TRN("No zones")'),
        ('sb_str(&b, "Based on Open25-DM32 Classic\\n");',
         'sb_str(&b, TR("Based on Open25-DM32 Classic"));\n\tsb_chr(&b, \'\\n\');'),
        ('sb_str(&b, "Based on RadioFoundry\\n");',
         'sb_str(&b, TR("Based on RadioFoundry"));\n\tsb_chr(&b, \'\\n\');'),
        ('rpt_info.sample ? "sample" :', 'rpt_info.sample ? TR("sample") :'),
    ])
    transform('tests/host/sim/ui_sim.c', [
        ('void gfx_fit_trace(const char *src, const char *out)\n{\n\tsim_rec.shortened++;\n',
         'void gfx_fit_trace(const char *src, const char *out)\n{\n\tsim_rec.shortened++;\n'
         '\t// DM32 Plus: SIM_FIT_LOG=FILE appends each shortened text (the text, what was shown)\n'
         '\tconst char *fit_log = getenv("SIM_FIT_LOG");\n'
         '\tif (fit_log != NULL && *fit_log) {\n'
         '\t\tFILE *fl = fopen(fit_log, "a");\n'
         '\t\tif (fl != NULL) {\n'
         '\t\t\tfprintf(fl, "%s\\t%s\\n", src, out);\n'
         '\t\t\tfclose(fl);\n'
         '\t\t}\n'
         '\t}\n'),
    ])
