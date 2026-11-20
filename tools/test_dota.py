# SPDX-License-Identifier: GPL-3.0-or-later
"""Host tests of DOTA Transfer (tools/plus_dota.py, docs/dota.md 16.1), run by tools/test_classic.py after its own (the
build tree prepared, ASan / UBSan flags given)."""
import subprocess


def run(args, **kw):
    return subprocess.run([str(a) for a in args], check=True, **kw)


def main(ROOT, APP, OUT, flags, host):
    tp = ROOT / 'tests/plus'
    core = APP / 'core'
    codeplug = [core / 'codeplug' / f for f in ('codeplug.c', 'codeplug_kv.c', 'codeplug_tag.c', 'codeplug_records.c',
                                                'codeplug_talk.c')] + [core / 'p25/p25_codeplug.c']
    dota = ['-DCONFIG_DOTA=1', '-DCONFIG_DMR_ANY=1', '-DCONFIG_YSF=1', '-DCONFIG_NXDN=1', '-DCONFIG_DPMR=1']
    # The protocol: frames, the manifest, both state machines over a lossy, duplicating, reordering fake link
    run(['clang', *flags, *dota, '-DCONFIG_APRS=1', '-I' + str(host), tp / 'test_dota_proto.c',
         core / 'radio/dota_proto.c', core / 'aprs/ax25.c', core / 'storage/storage.c',
         host / 'sim/sim_flash.c', '-o', OUT / 'test_dota_proto'], cwd=ROOT)
    run([OUT / 'test_dota_proto'], cwd=ROOT)
    # The records: every mode and field through encoder and decoder, refusals, fuzzing
    run(['clang', *flags, *dota, tp / 'test_dota_rec.c', core / 'radio/dota_rec.c', '-o', OUT / 'test_dota_rec'],
        cwd=ROOT)
    run([OUT / 'test_dota_rec'], cwd=ROOT)
