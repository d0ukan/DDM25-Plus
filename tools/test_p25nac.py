# SPDX-License-Identifier: GPL-3.0-or-later
"""DDM32 Plus separate RX and TX NAC (CONFIG_P25_TXNAC; Classic r13 stores the TX NAC, docs/plus.md "Separate RX NAC and TX NAC"): host tests, run by
tools/test_classic.py (make test). Its own module so the shared test runner gains a few call lines only."""
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
APP = ROOT / 'build/radiofoundry-p25'
OUT = ROOT / 'build/classic-host'


def run(args, **kw):
    return subprocess.run([str(a) for a in args], check=True, **kw)


def codeplug(flags):
    """The channel store (Classic r13's RFKV P25 parameter block holds the TX NAC; the switch no longer changes a
    record), with Plus's modes built in; then CH_MODE 2 with the switch."""
    host = APP / 'tests/host'
    common = ['-DCONFIG_P25_MODE2=1', '-DCONFIG_YSF=1', '-DCONFIG_NXDN=1', '-DCONFIG_DPMR=1', '-DCONFIG_DMR_ANY=1']
    sources = [*(APP / 'core' / f for f in ('codeplug/codeplug.c', 'codeplug/codeplug_kv.c', 'codeplug/codeplug_tag.c',
                                          'codeplug/codeplug_records.c', 'p25/p25_codeplug.c', 'codeplug/codeplug_talk.c')),
               APP / 'core/storage/storage.c', host / 'sim/sim_flash.c']
    run(['clang', *flags, *common, '-DCONFIG_P25_TXNAC=1', ROOT / 'tests/plus/test_p25nac.c', *sources,
         '-o', OUT / 'test_p25nac'], cwd=ROOT)
    run([OUT / 'test_p25nac'], cwd=ROOT)
    run(['clang', *flags, *common, '-DCONFIG_P25_TXNAC=1', ROOT / 'tests/plus/test_p25_mode2.c', *sources,
         '-o', OUT / 'test_p25_mode2_txnac'], cwd=ROOT)
    r = subprocess.run([str(OUT / 'test_p25_mode2_txnac')], cwd=ROOT, capture_output=True, text=True)
    print('\n'.join(l for l in r.stdout.splitlines() if not l.startswith('rec ')))
    if r.returncode != 0:
        raise SystemExit('test_p25_mode2 (CONFIG_P25_TXNAC=1) failed')


def worker(ysf_flags, codec):
    """The transmitted NID through the actual channel worker (fake modem and microphone)."""
    flags = [f for f in ysf_flags if f not in ('-DCONFIG_YSF=1', '-DCONFIG_DMR=1')] + ['-DCONFIG_P25_TXNAC=1']
    out = OUT / 'p25nac-worker'
    out.mkdir(exist_ok=True)
    objects = []
    for source in [*(ROOT / 'src/p25' / f'{n}.c' for n in ('p25', 'p25_lcx', 'voice', 'phy_stream', 'selftest',
                                                          'audio_fifo')),
                   *sorted((ROOT / 'third_party/p25cai/src').glob('*.c')),
                   ROOT / 'integration/radiofoundry/cpu_profile.c', APP / 'core/debuglink/dl_fmt.c',
                   APP / 'core/radio/beep.c', ROOT / 'tests/plus/test_p25nac_worker.c']:
        obj = out / (('cai-' if source.parent.parent.name == 'p25cai' else '') + source.stem + '.o')
        run(['clang', *flags, '-c', source, '-o', obj], cwd=ROOT)
        objects.append(obj)
    run(['clang++', '-fsanitize=address,undefined', *objects, codec, '-o', OUT / 'test_p25nac_worker'], cwd=ROOT)
    run([OUT / 'test_p25nac_worker'], cwd=ROOT)


def radio(argv, wrapper_in_argv, fixture, extra):
    """The radio core and native controller after the Classic suite, the multi-watch ring and the VFO's own suite run
    again with it built in (argv: the radio suite's compile line, wrapper_in_argv the source it names now; extra: the
    VFO build's flags and sources)."""
    host = APP / 'tests/host'
    wrapper = OUT / 'radioCoreP25nac.c'
    inc = lambda name, main: (f'#define main {main}\n#include "{ROOT / name}"\n#undef main\n')
    wrapper.write_text('#include "native_modem/native_lab.h"\nextern const nt_ops classic_ops;\n' +
                       fixture.replace('\t.native_modem=&classic_ops,', '\t.native_modem=&classic_ops,\n\t.caps=CAP_AM_RX,') +
                       '\n' + inc('tests/classic/test_radio.inc', 'classic_radio_main') +
                       inc('tests/plus/test_mwatch_radio.inc', 'mwatch_radio_main') +
                       inc('tests/plus/test_vfox_radio.inc', 'vfox_radio_main') +
                       '#include "' + str(ROOT / 'tests/plus/test_p25nac_radio.inc') + '"\n')
    argv = list(argv)
    argv[argv.index(str(wrapper_in_argv))] = str(wrapper)
    argv[argv.index('-o') + 1] = str(OUT / 'test_radio_p25nac')
    argv = [a for a in argv if a not in ('-DCONFIG_YSF_TX=0', '-DCONFIG_YSF_TX=1')]
    run([*argv, *extra, '-DCONFIG_P25_TXNAC=1', '-DCONFIG_NAC_DEC=1'], cwd=host)
    run([OUT / 'test_radio_p25nac'], cwd=host)


def ui(argv, source_in_argv, extra):
    """The editor's and VFO settings' rows in the upstream simulator (argv: test_ui_vfox's compile line)."""
    host = APP / 'tests/host'
    argv = list(argv)
    argv[argv.index(str(source_in_argv))] = str(ROOT / 'tests/plus/test_ui_p25nac.c')
    argv[argv.index('-o') + 1] = str(OUT / 'test_ui_p25nac')
    run([*argv, *extra, '-DCONFIG_P25_TXNAC=1'], cwd=host)
    run([OUT / 'test_ui_p25nac'], cwd=host)
