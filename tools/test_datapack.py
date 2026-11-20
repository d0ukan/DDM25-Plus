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
    images = builder_tests(ROOT, OUT)
    run([OUT / 'test_datapack', *images], cwd=ROOT)
    ui_tests(ROOT, APP, OUT, host)


def ui_tests(ROOT, APP, OUT, host):
    """Settings > About and Tools > Airfields (with the air watch) with packs in force, through upstream's UI simulator
    (tests/plus/ui_sim_dpk.c over the air items' ui_sim_air.c), English and Turkish"""
    import shlex
    tp = ROOT / 'tests/plus'
    core = APP / 'core'
    run(['make', 'out/gen/lang_tables.h'], cwd=host, stdout=subprocess.DEVNULL)
    command = run(['make', '-Bn', 'uiFlowTest', 'SANITIZE=1'], cwd=host, capture_output=True, text=True).stdout
    argv = shlex.split(next(line for line in command.splitlines() if ' -o uiFlowTest ' in line))
    argv[argv.index('uiFlowTest.c')] = str(tp / 'test_ui_datapack.c')
    argv[argv.index('sim/ui_sim.c')] = str(tp / 'ui_sim_dpk.c')
    argv[argv.index('-o') + 1] = str(OUT / 'test_ui_datapack')
    run([*argv, '-I' + str(host), '-I' + str(tp), '-DCONFIG_P25_CHANNELS=1', '-DCONFIG_AM_RX=1', '-DCONFIG_APRS=1',
         '-DCONFIG_VFO=1', '-DCONFIG_AIRFIELDS=1', '-DCONFIG_LANG_TR=1', '-DCONFIG_LARGE_SCALE=1',
         '-DCONFIG_HOME_FREQ=1', '-DCONFIG_MULTI_WATCH=1', '-DCONFIG_VFO_MODES=1', '-DCONFIG_AIR_WATCH=1',
         '-DCONFIG_DATAPACK=1',
         *(str(core / 'ui' / f) for f in ('airfield.c', 'airfield_data.c', 'ui_airfield.c', 'air_pack.c')),
         str(core / 'storage/datapack.c'), str(core / 'storage/storage.c'), str(host / 'sim/sim_flash.c'),
         str(core / 'aprs/aprs_fmt.c'), str(core / 'radio/vfox.c'), '-lm'], cwd=host)
    run([OUT / 'test_ui_datapack'], cwd=host)


CSV = '''RADIO_ID,CALLSIGN,FIRST_NAME,LAST_NAME,CITY,STATE,COUNTRY
2860105,TA2XYZ,Ayşe,Kaya,Izmir,,Turkey
2860001,ta1abc,Ahmet,Yilmaz,Ankara,,Turkey
3101234,TA3AA,Mehmet Ali,Demir,Bursa,,Turkey
2860001,TA9DUP,Dup,Dup,X,,Turkey
3109999,W1AW,Hiram,Maxim,Newington,CT,United States
'''

AD218 = '''LTAC AD 2.17 something
LTAC AD 2.18 ATS COMMUNICATION FACILITIES
Service designation  Call sign  Frequency  Hours  Remarks
ATIS  ESENBOGA INFORMATION  123.600 MHZ  H24
TWR   ESENBOGA TOWER  118.100 MHZ  H24
      121.500 MHZ emergency
APP   ANKARA APPROACH 119.100 MHZ H24
      UHF 257.800 MHZ (military, not air band)
LTAC AD 2.19 RADIO NAVIGATION
VOR 112.500 MHZ
'''


def builder_tests(ROOT, OUT):
    """tools/datapack.py on synthetic input (never real RadioID or AIP data): the payloads and headers, the CPS write
    order, the AD 2.18 parser; returns the two images for the C test, which writes and reads them as the radio does"""
    import struct
    import sys
    sys.path.insert(0, str(ROOT))
    from tools import datapack as dp
    csv = OUT / 'dp_user.csv'
    csv.write_text(CSV, encoding='utf-8')
    payload, n, skipped = dp.radioid_from_csv(str(csv), ['Turkey'])
    assert n == 3 and skipped['duplicate'] == 1, (n, skipped)
    image = dp.header(dp.RADIOID, payload, '20261120', 'RadioID TR') + payload
    h, back = dp.parse(image)
    assert h and h['crc_ok'] and h['len'] == len(payload) == 8 + 3 * 15 and back == payload, h
    assert struct.unpack_from('<IBB', payload) == (3, 15, 16)
    assert payload[8:11] == (2860001).to_bytes(3, 'little')		# sorted by ID
    try:
        dp.radioid_payload([(1, 'A', ''), (1, 'B', '')])
        raise AssertionError('a repeated ID was taken')
    except dp.PackError:
        pass
    fields = {'LTAC': dp.parse_ad218(AD218)}
    t = dp.airfields.TYPES
    assert fields['LTAC'] == [(t.index('ATIS'), 123600), (t.index('TWR'), 118100), (t.index('TWR'), 121500),
                              (t.index('APP'), 119100)], fields		# the 2.19 VOR and the UHF one left out
    fields['LTAC'] = [f for f in fields['LTAC'] if f[1] != 121500]
    air = dp.air_payload(fields)
    air_image = dp.header(dp.AIR, air, '20261120', 'TR aerodromes') + air
    assert dp.parse(air_image)[0]['crc_ok']
    bad = bytearray(image)
    bad[70] ^= 1
    assert dp.parse(bytes(bad))[0]['crc_ok'] is False and dp.parse(b'\xff' * 64)[0] is None
    # the radio's slots: the new pack goes where the pack in force is not; its sequence one more
    empty = b'\xff' * 64
    assert dp.inactive_slot(dp.RADIOID, [empty, empty]) == 0 and dp.next_seq([empty, empty]) == 1
    s1 = dp.header(dp.RADIOID, payload, '20261120', 'x', seq=4) + payload
    assert dp.inactive_slot(dp.RADIOID, [s1, empty]) == 1 and dp.next_seq([s1, empty]) == 5
    # the CPS order: payload sectors, the header's sector last
    big = dp.header(dp.RADIOID, bytes(9000), '20261120', 'x') + bytes(9000)
    plan = dp.cps_plan(0xEB0000, big)
    assert [a for a, _ in plan] == [0xEB1000, 0xEB2000, 0xEB0000] and all(len(d) == 4096 for _, d in plan)
    assert plan[-1][1][:4] == b'DPK1'
    rid = OUT / 'dp_radioid.dpk'
    rid.write_bytes(image)
    airf = OUT / 'dp_air.dpk'
    airf.write_bytes(air_image)
    assert 'payload CRC ok' in dp.info(image) and '3 IDs' in dp.info(image) and '1 airfields' in dp.info(air_image)
    print('  datapack.py: RadioID and air band packs from synthetic input, the AD 2.18 parser, the CPS order')
    return [rid, airf]
