# SPDX-License-Identifier: GPL-3.0-or-later
"""Host tests of the user data packs (tools/plus_datapack.py, docs/r14.md 7), run by tools/test_classic.py after its
own (the build tree prepared, ASan / UBSan flags given): the store on the simulated data chip (the region guard, the
slots, power cuts)."""
import subprocess


def run(args, **kw):
    return subprocess.run([str(a) for a in args], check=True, **kw)


def main(ROOT, APP, OUT, flags, host):
    tp = ROOT / 'tests/plus'
    core = APP / 'core'
    pack = ['-DCONFIG_DATAPACK=1']
    # The store: guard, slots, sequence, broken slots, a power cut at every flash operation
    run(['clang', *flags, *pack, '-I' + str(host), tp / 'test_datapack.c', core / 'storage/datapack.c',
         core / 'storage/storage.c', host / 'sim/sim_flash.c', '-o', OUT / 'test_datapack'], cwd=ROOT)
    run([OUT / 'test_datapack'], cwd=ROOT)
