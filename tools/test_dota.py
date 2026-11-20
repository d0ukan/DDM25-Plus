# SPDX-License-Identifier: GPL-3.0-or-later
"""Host tests of DOTA Transfer (tools/plus_dota.py, docs/dota.md 16.1), run by tools/test_classic.py after its own (the
build tree prepared, ASan / UBSan flags given). Each phase 2 step adds its suites here."""
import subprocess


def run(args, **kw):
    return subprocess.run([str(a) for a in args], check=True, **kw)


def main(ROOT, APP, OUT, flags, host):
    tp = ROOT / 'tests/plus'
    # The protocol: frames, the manifest, both state machines over a lossy, duplicating, reordering fake link
    run(['clang', *flags, '-DCONFIG_DOTA=1', '-DCONFIG_APRS=1', '-I' + str(host), tp / 'test_dota_proto.c',
         APP / 'core/radio/dota_proto.c', APP / 'core/aprs/ax25.c', APP / 'core/storage/storage.c',
         host / 'sim/sim_flash.c', '-o', OUT / 'test_dota_proto'], cwd=ROOT)
    run([OUT / 'test_dota_proto'], cwd=ROOT)
