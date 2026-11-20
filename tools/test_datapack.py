# SPDX-License-Identifier: GPL-3.0-or-later
"""Host tests of the user data packs (tools/plus_datapack.py, docs/r14.md 7), run by tools/test_classic.py after its
own (the build tree prepared, ASan / UBSan flags given): the store on the simulated data chip (the region guard, the
slots, power cuts), the RadioID pack's lookup and the About line,
the air band pack over the shipped airfield table."""
import subprocess


def run(args, **kw):
    return subprocess.run([str(a) for a in args], check=True, **kw)


def main(ROOT, APP, OUT, flags, host):
    tp = ROOT / 'tests/plus'
    core = APP / 'core'
    pack = ['-DCONFIG_DATAPACK=1', '-DCONFIG_AIRFIELDS=1', '-DCONFIG_APRS=1']
    # The store: guard, slots, sequence, broken slots, a power cut at every flash operation
    run(['clang', *flags, *pack, '-I' + str(host), tp / 'test_datapack.c', core / 'storage/datapack.c',
         core / 'storage/storage.c', core / 'ui/fmt.c', host / 'sim/sim_flash.c',
         *(core / 'ui' / f for f in ('air_pack.c', 'airfield.c', 'airfield_data.c')), core / 'aprs/aprs_fmt.c', '-lm',
         '-o', OUT / 'test_datapack'], cwd=ROOT)
    run([OUT / 'test_datapack'], cwd=ROOT)
