# SPDX-License-Identifier: GPL-3.0-or-later
"""Host tests of the round 15 "channels" job (tools/plus_chux.py, docs/plus.md "Channels, zones and DMR rows"), run by
tools/test_classic.py after its own (the build tree prepared, ASan / UBSan flags given):

- the zone and codeplug limits and Delete channel on the real codeplug over a simulated flash (tests/plus/test_zone_cap.c);
- Home's channel stepping through an 80-channel zone in the real radio state machine (tests/plus/test_zone_radio.inc);
- the screens' flows in upstream's simulator, English and Turkish, with the layout check (tests/plus/test_ui_chux.c)."""
import shlex
import subprocess


def run(args, **kw):
    return subprocess.run([str(a) for a in args], check=True, **kw)


def radio_test(APP, OUT, host, name, inc, defs=(), subs=()):
    """upstream's radioCoreTest with a Plus .inc after it (its main renamed), built as make builds it; subs: (old, new)
    replacements in the fixture"""
    fixture = (host / 'radioCoreTest.c').read_text().replace('int main(void)', 'int upstream_radio_main(void)')
    for old, new in subs:
        assert fixture.count(old) == 1, old
        fixture = fixture.replace(old, new)
    wrapper = OUT / (name + '.c')
    wrapper.write_text(fixture + '\n#include "' + str(inc) + '"\n')
    command = run(['make', '-Bn', 'radioCoreTest', 'SANITIZE=1'], cwd=host, capture_output=True, text=True).stdout
    argv = shlex.split(next(line for line in command.splitlines() if ' -o radioCoreTest ' in line))
    argv[argv.index('radioCoreTest.c')] = str(wrapper)
    argv[argv.index('-o') + 1] = str(OUT / name)
    run([*argv, '-I' + str(host), *defs], cwd=host)
    run([OUT / name], cwd=host)


def main(ROOT, APP, OUT, flags, host):
    tp = ROOT / 'tests/plus'
    # tools/test_classic.py's CODEPLUG: the codeplug and its RFKV mode records
    CODEPLUG = [APP / 'core/codeplug' / f for f in ('codeplug.c', 'codeplug_kv.c', 'codeplug_tag.c', 'codeplug_records.c',
                                                    'codeplug_talk.c')] + [APP / 'core/p25/p25_codeplug.c']
    zedit = [APP / 'core/codeplug' / f for f in ('zone_edit.c', 'zone_shift.c', 'zone_settle.c')]
    run(['clang', *flags, '-Wshadow', '-DCONFIG_CHAN_EXTRAS=1', tp / 'test_zone_cap.c', *CODEPLUG, *zedit,
         APP / 'core/settings/settings.c', APP / 'core/storage/storage.c', host / 'sim/sim_flash.c',
         '-o', OUT / 'test_zone_cap'], cwd=ROOT)
    run([OUT / 'test_zone_cap'], cwd=ROOT)
    # The screens, English and Turkish
    command = run(['make', '-Bn', 'uiFlowTest', 'SANITIZE=1'], cwd=host, capture_output=True, text=True).stdout
    argv = shlex.split(next(line for line in command.splitlines() if ' -o uiFlowTest ' in line))
    argv[argv.index('uiFlowTest.c')] = str(tp / 'test_ui_chux.c')
    argv[argv.index('-o') + 1] = str(OUT / 'test_ui_chux')
    run([*argv, '-I' + str(host), '-I' + str(tp), '-DCONFIG_P25_CHANNELS=1', '-DCONFIG_P25_ANY=1', '-DCONFIG_DMR_ANY=1',
         '-DCONFIG_NAC_DEC=1', '-DCONFIG_LANG_TR=1', '-DCONFIG_CHAN_EXTRAS=1'], cwd=host)
    run([OUT / 'test_ui_chux'], cwd=host)
    # 90 channels in the fake codeplug (upstream's suite uses the first 8): a zone of 80 different ones
    radio_test(APP, OUT, host, 'test_zone_radio', tp / 'test_zone_radio.inc', 
               subs=[('#define NCH 8\n', '#define NCH 90\n')])
