# SPDX-License-Identifier: GPL-3.0-or-later
"""Host tests of the round 15 scan job (tools/plus_scan.py, docs/plus.md "Search ranges and speed"), run by
tools/test_classic.py after its own (the build tree prepared, ASan / UBSan flags given):

- item 1, the search range editor through upstream's UI simulator, English and Turkish (tests/plus/test_ui_scan.c,
  tests/plus/ui_sim_scan.c): presets, typing, steps, moving, refusals, the request's step, the range kept;
- the radio core with the switches on: upstream's radio suite and the Classic / Plus P25 suites (the P25 search's
  state machine among them), then the DMR search's, built with this job's switches (tests/classic/test_radio.inc;
  OUT/classic_dispatch.inc as tools/test_classic.py wrote it)."""
import shlex
import subprocess


def run(args, **kw):
    return subprocess.run([str(a) for a in args], check=True, **kw)


def ui_tests(ROOT, APP, OUT, host):
    tp = ROOT / 'tests/plus'
    ui = APP / 'core/ui'
    run(['make', 'out/gen/lang_tables.h'], cwd=host, stdout=subprocess.DEVNULL)
    command = run(['make', '-Bn', 'uiFlowTest', 'SANITIZE=1'], cwd=host, capture_output=True, text=True).stdout
    argv = shlex.split(next(line for line in command.splitlines() if ' -o uiFlowTest ' in line))
    argv[argv.index('uiFlowTest.c')] = str(tp / 'test_ui_scan.c')
    argv[argv.index('sim/ui_sim.c')] = str(tp / 'ui_sim_scan.c')
    argv[argv.index('-o') + 1] = str(OUT / 'test_ui_scan')
    run([*argv, '-I' + str(host), '-I' + str(tp), '-DCONFIG_P25_CHANNELS=1', '-DCONFIG_P25_SEARCH=1',
         '-DCONFIG_DMR_SEARCH=1', '-DCONFIG_DMR_ANY=1', '-DCONFIG_FIND_SAVE=1', '-DCONFIG_SEARCH_RANGE=1',
         '-DCONFIG_LANG_TR=1',
         *(str(ui / f) for f in ('ui_p25search.c', 'ui_dmrsearch.c', 'ui_srange.c', 'ui_scratch.c', 'findsave.c',
                                 'ui_findsave.c'))], cwd=host)
    run([OUT / 'test_ui_scan'], cwd=host)


SCAN_SWITCHES = ['-DCONFIG_SEARCH_RANGE=1']


def radio_tests(ROOT, APP, OUT, host):
    fixture = (host / 'radioCoreTest.c').read_text().replace('int main(void)', 'int upstream_radio_main(void)')
    fixture = fixture.replace('const board_t board = {', 'const board_t board = {\n\t.native_modem=&classic_ops,')
    wrapper = OUT / 'radioCoreScanJob.c'
    wrapper.write_text('#include "native_modem/native_lab.h"\nextern const nt_ops classic_ops;\n' + fixture +
                       '\n#define main classic_radio_main\n#include "' + str(ROOT / 'tests/classic/test_radio.inc') +
                       '"\n#undef main\n#include "' + str(ROOT / 'tests/plus/test_scan_radio.inc') + '"\n')
    command = run(['make', '-Bn', 'radioCoreTest', 'SANITIZE=1'], cwd=host, capture_output=True, text=True).stdout
    argv = shlex.split(next(line for line in command.splitlines() if ' -o radioCoreTest ' in line))
    argv[argv.index('radioCoreTest.c')] = str(wrapper)
    argv += ['-I' + str(OUT), '-I' + str(host), '-I' + str(ROOT / 'tests/native/stubs'), '-I' + str(APP / 'core'),
             '-I' + str(ROOT / 'src/modem'), '-I' + str(ROOT / 'integration/radiofoundry'),
             '-DCONFIG_P25_CHANNELS=1', '-DCONFIG_P25_NATIVE=1', '-DCONFIG_P25=1', '-DCONFIG_DEV_TX=1',
             str(ROOT / 'src/modem/native_test.c'), str(ROOT / 'integration/radiofoundry/native/native_lab.c'),
             *SCAN_SWITCHES]
    radio = APP / 'core/radio'
    # The Plus P25 suite with the P25 search (as tools/test_classic.py's test_radio_scan)
    argv[argv.index('-o') + 1] = str(OUT / 'test_radio_scanjob_p25')
    run([*argv, '-DCONFIG_P25_SCAN=1', '-DCONFIG_SPECTRUM=1', '-DCONFIG_FOX=1', '-DCONFIG_CW_BEACON=1',
         '-DCONFIG_P25_SEARCH=1', '-DCONFIG_P25_CALLER=1', '-DCONFIG_P25_ANY=1', '-I' + str(ROOT / 'tests/classic'),
         *(str(radio / f) for f in ('spectrum.c', 'fox.c', 'cw_beacon.c', 'p25search.c'))], cwd=host)
    run([OUT / 'test_radio_scanjob_p25'], cwd=host)
    # The DMR search's (as test_radio_dmrsearch)
    argv[argv.index('-o') + 1] = str(OUT / 'test_radio_scanjob_dmr')
    run([*argv, '-DCONFIG_DMR_ANY=1', '-DCONFIG_DMR_SEARCH=1', '-I' + str(ROOT / 'tests/plus'),
         str(radio / 'dmrsearch.c')], cwd=host)
    run([OUT / 'test_radio_scanjob_dmr'], cwd=host)


def main(ROOT, APP, OUT, flags, host, only=None):
    if not only or 'radio' in only:
        radio_tests(ROOT, APP, OUT, host)
    if not only or 'ui' in only:
        ui_tests(ROOT, APP, OUT, host)
