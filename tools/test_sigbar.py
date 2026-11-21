# SPDX-License-Identifier: GPL-3.0-or-later
"""Host test of the status strip's signal bars (tools/plus_sigbar.py), run by tools/test_classic.py after its own:
rssi_bars() and strip_bars() cut from the generated core/ui/ui_screens.c, checked by tests/plus/test_sigbar.c."""
import subprocess


def main(ROOT, APP, OUT, flags, host):
    ui = (APP / 'core/ui/ui_screens.c').read_text()
    i = ui.index('static uint8_t rssi_bars(int16_t dbm)\n')
    k = ui.index('static uint8_t strip_bars(const radio_view_t *v)\n', i)
    j = ui.index('\n}\n', k) + 3
    (OUT / 'sigbar.inc').write_text(ui[i:j])
    cmd = ['clang', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
           '-Wno-unused-function', '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined',
           '-I' + str(APP / 'core'), '-I' + str(APP / 'hal'), '-I' + str(APP / 'drivers'), '-DCONFIG_DMR=1',
           '-DSIGBAR_INC="' + str(OUT / 'sigbar.inc') + '"', str(ROOT / 'tests/plus/test_sigbar.c'),
           '-o', str(OUT / 'test_sigbar')]
    subprocess.run(cmd, check=True, cwd=ROOT)
    subprocess.run([str(OUT / 'test_sigbar')], check=True, cwd=ROOT)
