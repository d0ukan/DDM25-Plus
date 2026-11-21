# SPDX-License-Identifier: GPL-3.0-or-later
"""Host tests of the searchfix job (CONFIG_SEARCH_SURE, tools/plus_scan.py, docs/plus.md "Search reliability"), run by
tools/test_classic.py after tools/test_scan.py (the build tree prepared, ASan / UBSan flags given):

- the engines step by step (tests/plus/test_searchsure.c), with and without the all digital search's kind: the floor
  learned, the run's middle (a strong carrier's peak, a plateau, a half step), the skirt, a carrier gone off the air
  not skipped, the long DMR window, skips within half a step, the channel grid, a wide flat run, the steps a second;
- the scenario model (tests/plus/test_search_sure.c) built before (SEARCH_SURE=0: the beta.7 engines) and after,
  200 transmitters a scenario (the owner's 436.275 test among them): found in 60 s, the time to find, probes a minute,
  steps a second; the table printed for docs/plus.md and the figures checked;
- the real radio state machine's P25, DMR and all digital search suites built with the switch (as
  tools/test_classic.py's test_radio_allsearch, OUT/classic_dispatch.inc as it wrote it)."""
import shlex
import subprocess


def run(args, **kw):
    return subprocess.run([str(a) for a in args], check=True, **kw)


SWITCHES = ['-DCONFIG_SEARCH_FAST=1', '-DCONFIG_SEARCH_SURE=1']


def engine_tests(ROOT, APP, OUT, flags):
    for kind in (0, 1):
        exe = OUT / f'test_searchsure_{kind}'
        run(['clang', *flags, '-DCONFIG_P25_SEARCH=1', '-DCONFIG_DMR_SEARCH=1', *SWITCHES,
             f'-DCONFIG_ALL_SEARCH={kind}', f'-DCONFIG_YSF={kind}', f'-DCONFIG_NXDN={kind}',
             ROOT / 'tests/plus/test_searchsure.c', APP / 'core/radio/p25search.c', APP / 'core/radio/dmrsearch.c',
             '-o', exe], cwd=ROOT)
        run([exe], cwd=ROOT)


def search_model(ROOT, APP, OUT, flags):
    res = {}
    for sure in (0, 1):
        exe = OUT / f'test_search_sure_{sure}'
        run(['clang', *flags, '-DCONFIG_P25_SEARCH=1', '-DCONFIG_DMR_SEARCH=1', '-DCONFIG_SEARCH_FAST=1',
             f'-DCONFIG_SEARCH_SURE={sure}', '-DCONFIG_ALL_SEARCH=1', '-DCONFIG_YSF=1', '-DCONFIG_NXDN=1',
             ROOT / 'tests/plus/test_search_sure.c', APP / 'core/radio/p25search.c', APP / 'core/radio/dmrsearch.c',
             '-lm', '-o', exe], cwd=ROOT)
        out = run([exe], cwd=ROOT, capture_output=True, text=True).stdout
        for line in out.splitlines():
            w = line.split()
            if w[:1] == ['MODEL']:
                res[(w[1], w[2])] = [float(x) for x in w[3:]]
            elif line.startswith('FAIL'):
                raise SystemExit(line)
    print('Searchfix model (200 transmitters a scenario, 60 s each; before -> after): found, median / 90th pct s to '
          'find (99 = not within 60 s), probes a minute, steps a second')
    names = [k[1] for k in res if k[0] == 'before']
    for n in names:
        b, a = res[('before', n)], res[('sure', n)]
        print(f'  {n:18s} found {b[0]:3.0f} -> {a[0]:3.0f}  median {b[1]:4.1f} -> {a[1]:4.1f}  p90 {b[2]:4.1f} -> '
              f'{a[2]:4.1f}  probes/min {b[3]:4.1f} -> {a[3]:4.1f}  steps/s {b[4]:4.1f} -> {a[4]:4.1f}')
        if n.endswith('433200'):
            # outside the owner's typed range 436.0-436.5: never found, by either (the range is applied as typed)
            assert b[0] == 0 and a[0] == 0, (n, b, a)
            continue
        # never fewer finds than before (3 of 200: the random overs against the pass), never slower to the median
        assert a[0] >= b[0] - 3, (n, b, a)
        assert a[1] <= b[1] + 1.0, (n, b, a)
        # never a sweep slower than before over a band without the transmitter
        assert a[4] >= b[4] - 0.5, (n, b, a)
    sure = {n: res[('sure', n)] for n in names}
    # the owner's case: the other radio in the room on 436.275, the range typed as 436.000(.010)-436.500
    for n in ('dmr-owner-436275', 'dmr-owner-offgrid', 'all-owner-436275'):
        assert sure[n][0] >= 190 and sure[n][1] <= 4.0, (n, sure[n])
    # a receiver whose quiet reading is high: the floor learned (before: every step a probe, the sweep stood still)
    for n in ('p25-highfloor', 'dmr-highfloor', 'all-highfloor'):
        assert sure[n][0] >= 185 and sure[n][4] >= 13.0, (n, sure[n])
    # a signal between two steps (6.25 kHz raster): found on the half step (before: never)
    assert sure['p25-half'][0] >= 195 and sure['dmr-half'][0] >= 165, (sure['p25-half'], sure['dmr-half'])
    # a quiet step: P25 one poll, DMR three (the speed of SEARCH_FAST kept)
    assert sure['p25-overs'][4] >= 39.0 and sure['dmr-direct'][4] >= 13.0, (sure['p25-overs'], sure['dmr-direct'])


def radio_tests(ROOT, APP, OUT, host):
    fixture = (host / 'radioCoreTest.c').read_text().replace('int main(void)', 'int upstream_radio_main(void)')
    fixture = fixture.replace('const board_t board = {', 'const board_t board = {\n\t.native_modem=&classic_ops,')
    wrapper = OUT / 'radioCoreSearchfix.c'
    wrapper.write_text('#include "native_modem/native_lab.h"\nextern const nt_ops classic_ops;\n' + fixture +
                       '\n#include "' + str(ROOT / 'tests/classic/test_radio.inc') + '"\n')
    command = run(['make', '-Bn', 'radioCoreTest', 'SANITIZE=1'], cwd=host, capture_output=True, text=True).stdout
    argv = shlex.split(next(line for line in command.splitlines() if ' -o radioCoreTest ' in line))
    argv[argv.index('radioCoreTest.c')] = str(wrapper)
    argv[argv.index('-o') + 1] = str(OUT / 'test_radio_searchfix')
    radio = APP / 'core/radio'
    argv += ['-I' + str(OUT), '-I' + str(host), '-I' + str(ROOT / 'tests/native/stubs'), '-I' + str(APP / 'core'),
             '-I' + str(ROOT / 'src/modem'), '-I' + str(ROOT / 'integration/radiofoundry'),
             '-DCONFIG_P25_CHANNELS=1', '-DCONFIG_P25_NATIVE=1', '-DCONFIG_P25=1', '-DCONFIG_DEV_TX=1',
             str(ROOT / 'src/modem/native_test.c'), str(ROOT / 'integration/radiofoundry/native/native_lab.c'),
             '-DCONFIG_SEARCH_RANGE=1', *SWITCHES,
             '-DCONFIG_P25_SCAN=1', '-DCONFIG_SPECTRUM=1', '-DCONFIG_FOX=1', '-DCONFIG_CW_BEACON=1',
             '-DCONFIG_P25_SEARCH=1', '-DCONFIG_P25_CALLER=1', '-DCONFIG_P25_ANY=1', '-DCONFIG_DMR_ANY=1',
             '-DCONFIG_DMR_SEARCH=1', '-DCONFIG_ALL_SEARCH=1', '-I' + str(ROOT / 'tests/classic'),
             '-I' + str(ROOT / 'tests/plus'),
             *(str(radio / f) for f in ('spectrum.c', 'fox.c', 'cw_beacon.c', 'p25search.c', 'dmrsearch.c'))]
    run(argv, cwd=host)
    run([OUT / 'test_radio_searchfix'], cwd=host)


def main(ROOT, APP, OUT, flags, host, only=None):
    if not only or 'engine' in only:
        engine_tests(ROOT, APP, OUT, flags)
    if not only or 'model' in only:
        search_model(ROOT, APP, OUT, flags)
    if not only or 'radio' in only:
        radio_tests(ROOT, APP, OUT, host)
