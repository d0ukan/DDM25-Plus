# SPDX-License-Identifier: GPL-3.0-or-later
"""Simulated end-to-end tests for radiotools/install_plus.py (and radio_detect.py).

The pinned RAM monitor's own FakeRadio stands in for the radio behind the real installer guard
(install_firstboot.InstallPort), so the real ROM load, CONFIG, pre-read, page writes, readback and restart gating all
run without hardware. No serial port is ever opened: serial.Serial is replaced. Releases are signed with a test key.
"""
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

import rtfixtures as fx
from rtfixtures import SECTOR, SIZE, SLOT, START
from radiotools import dm32_readonly_backup as b
from radiotools import install_plus as p
from radiotools import prepare_unit_image as unit
from radiotools import radio_detect
from radiotools import signed_manifest as sm

TAG = 47
PLUS_APP = fx.pattern(0x7000, 9) + b'DDM32 Plus test build'


@fx.need_upstream
class PlusInstallTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.m = b.load_upstream()
        cls.tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.tmp.name)
        cls.program = fx.make_program()
        cls.data = fx.make_data()
        cls.backup = cls.root / 'backup'
        fx.write_backup(cls.backup, cls.program, cls.data)
        cls.plus = fx.write_plus(cls.root / 'plus', PLUS_APP)
        cls.target = cls.program[:START] + PLUS_APP + b'\xff' * (SLOT - START - len(PLUS_APP)) + cls.program[SLOT:]
        cls.runs = 0

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def setUp(self):
        fx.use_test_key(self)

    def run_main(self, *args, radio=None):
        try:
            return fx.run_tool_main(p, 'install_plus.py', args, radio)
        finally:
            self.output = fx.run_tool_main.output

    def new_out(self):
        PlusInstallTests.runs += 1
        return self.root / f'run{self.runs}'

    def receipt(self, out):
        return json.loads((out / 'install-receipt.json').read_text())

    def radio(self, program, data=None, **kw):
        return fx.fake_radio(self.m, program, self.data if data is None else data, **kw)

    # ---- codec slot ----
    def test_slot_layout_matches_upstream_parser(self):
        codec = unit.load_codec()
        parsed = codec.parse_slot(self.program[SLOT:])
        self.assertIsNotNone(parsed)
        self.assertEqual(parsed['tag'], TAG)
        self.assertEqual(p.slot_check(self.program), TAG)
        self.assertEqual(p.slot_check(fx.make_program(slot=fx.make_slot(tag=48))), 48)

    def test_slot_refusals(self):
        slot = bytearray(fx.make_slot())
        cases = {'No prepared codec slot': [fx.make_program(slot=b'\xff' * 0x1000)]}
        bad = bytearray(slot); bad[0] ^= 1
        version = bytearray(slot); version[4] = 1
        cases['No prepared codec slot'] += [fx.make_program(slot=bytes(bad)), fx.make_program(slot=bytes(version))]
        crcs = []
        for off in (0x80 + 20, 0x80 + 0x800 + 3, len(slot) - 2, 0x40):
            c = bytearray(slot); c[off] ^= 0x10
            crcs.append(fx.make_program(slot=bytes(c)))
        cases['Codec slot CRC mismatch'] = crcs
        past = bytearray(slot); struct.pack_into('<I', past, 0x48, 0x10000)
        cases['run past the slot'] = [fx.make_program(slot=bytes(past))]
        for message, programs in cases.items():
            for program in programs:
                with self.subTest(message=message), self.assertRaisesRegex(ValueError, message):
                    p.slot_check(program)
        with patch.object(p, 'saved_images', lambda _: {'progmem-pass1.bin': cases['Codec slot CRC mismatch'][0],
                                                        'q128-pass1.bin': self.data}):
            with self.assertRaisesRegex(ValueError, 'CRC mismatch'):
                p.install_inputs(Path('unused'), self.plus)
            with self.assertRaisesRegex(ValueError, 'CRC mismatch'):
                p.restore_inputs(Path('unused'))

    # ---- release checks and the signed gate ----
    def test_valid_inputs_from_saved_backup(self):
        original, target, data, tag = p.install_inputs(self.backup, self.plus)
        self.assertEqual((original, target, data, tag), (self.program, self.target, self.data, TAG))

    def test_build_refusals(self):
        cases = [
            ('reproducible DDM32 Plus build manifest', dict(plus=False)),
            ('reproducible DDM32 Plus build manifest', dict(identical=False)),
            ('Application differs', dict(app_digest='0' * 64)),
            ('Linker map differs', dict(map_digest='0' * 64)),
            ('Linker layout differs', dict(symbols={'__codec_run_lo': 0x43a04})),
            ('Linker layout differs', dict(symbols={'__codec_tab_hi': 0x18003e28})),
            ('Linker layout differs', dict(symbols={'__codec_slot': None})),
        ]
        for i, (message, kw) in enumerate(cases):
            folder = fx.write_plus(self.root / f'plus-bad{i}', PLUS_APP, **kw)
            with self.subTest(message=message, case=i), self.assertRaisesRegex(ValueError, message):
                p.plus_application(folder)
        for size in (SLOT - START + 1, SLOT - START + SECTOR):
            big = fx.pattern(size, 3)
            folder = fx.write_plus(self.root / f'plus-big{size}', big, signed_for=False)
            with self.assertRaisesRegex(ValueError, 'end before the codec slot'):
                p.plus_application(folder)
        edge = fx.pattern(SLOT - START, 3)
        folder = fx.write_plus(self.root / 'plus-edge', edge)
        self.assertEqual(p.plus_application(folder), edge)
        _, target, _, _ = p.install_inputs(self.backup, folder)
        self.assertEqual(target[SLOT:], self.program[SLOT:])

    def test_signed_gate(self):
        """The public gate: signed with the embedded key and listing this exact application"""
        other = fx.pattern(0x7000, 15)
        cases = {
            'No signed manifest': fx.write_plus(self.root / 'g-unsigned', PLUS_APP, signed_for=False),
            'not listed': fx.write_plus(self.root / 'g-other', PLUS_APP, signed_for=other),
            'signature is not valid': fx.write_plus(self.root / 'g-wrongkey', PLUS_APP, secret=fx.OTHER_SECRET),
        }
        tampered = fx.write_plus(self.root / 'g-tampered', PLUS_APP)
        data = (tampered / sm.MANIFEST_NAME).read_bytes().replace(b'"stable"', b'"beta"  ')
        (tampered / sm.MANIFEST_NAME).write_bytes(data)
        cases['signature is not valid '] = tampered
        for message, folder in cases.items():
            with self.subTest(message=message), self.assertRaisesRegex(ValueError, message.strip()):
                p.plus_application(folder)
        with patch.object(sm, 'PUBLIC_KEY_HEX', ''):
            with self.assertRaisesRegex(ValueError, 'No release signing key'):
                p.plus_application(self.plus)

    def test_argument_refusals(self):
        out = self.new_out()
        for args in (('--backup', self.backup, '--out', out),
                     ('--backup', self.backup, '--plus', self.plus, '--restore', '--out', out),
                     ('--backup', self.backup, '--restore', '--from-stock', '--out', out),
                     ('--backup', self.backup, '--plus', self.plus, '--out', out, '--wait', 0),
                     ('--backup', self.backup, '--plus', self.plus, '--out', out, '--wait', 601)):
            with self.subTest(args=args), self.assertRaises(SystemExit) as caught:
                self.run_main(*args)
            self.assertEqual(caught.exception.code, 2)
        self.assertFalse(out.exists())

    # ---- plan only ----
    def test_plan_only_opens_no_port(self):
        out = self.new_out()
        plan = json.loads(self.run_main('--backup', self.backup, '--plus', self.plus, '--out', out, '--plan-only'))
        self.assertEqual(plan['mode'], 'install_plus')
        self.assertEqual(plan['changed_sectors'], 9)
        self.assertEqual((plan['first'], plan['last']), (hex(START), hex(START + 0x8000)))
        self.assertTrue(plan['boot_area_unchanged'] and plan['codec_slot_unchanged'])
        self.assertFalse(plan['data_chip_written'])
        self.assertEqual(plan['codec_slot_stock_tag'], TAG)
        self.assertEqual(plan['target_application_sha256'], b.sha(self.target[START:]))
        self.assertFalse(out.exists())

    def test_restore_plan_only_opens_no_port(self):
        out = self.new_out()
        plan = json.loads(self.run_main('--backup', self.backup, '--restore', '--out', out, '--plan-only'))
        self.assertEqual(plan['mode'], 'restore')
        self.assertEqual(plan['changed_sectors'], (SIZE - START) // SECTOR)
        self.assertEqual((plan['first'], plan['last']), (hex(START), hex(SIZE - SECTOR)))
        self.assertTrue(plan['boot_area_unchanged'])
        self.assertFalse(out.exists())

    def test_refusals_happen_before_any_port(self):
        bad = fx.write_plus(self.root / 'plus-unlisted', PLUS_APP, signed_for=fx.pattern(0x7000, 15))
        out = self.new_out()
        with self.assertRaisesRegex(ValueError, 'not listed'):
            self.run_main('--backup', self.backup, '--out', out, '--plus', bad)
        with patch.object(p, 'saved_images', lambda _: {
                'progmem-pass1.bin': fx.make_program(slot=b'\xff' * 0x1000), 'q128-pass1.bin': self.data}):
            with self.assertRaisesRegex(ValueError, 'No prepared codec slot'):
                self.run_main('--backup', self.backup, '--out', out, '--plus', self.plus, '--plan-only')
        nochips = self.root / 'backup-nochips'
        fx.write_backup(nochips, self.program, self.data, chips=None)
        with self.assertRaisesRegex(ValueError, 'flash chip IDs'):
            self.run_main('--backup', nochips, '--out', out, '--plus', self.plus)
        self.assertFalse(out.exists())

    # ---- install over Classic ----
    def check_untouched(self, radio, below=SLOT):
        self.assertEqual(radio.images['q128'], self.data)
        self.assertTrue(all(mem == 'progmem' and START <= a < below for mem, op, a in radio.writes))
        self.assertFalse(radio.errors)

    def test_full_simulated_install(self):
        radio = self.radio(self.program)
        out = self.new_out()
        self.run_main('--backup', self.backup, '--plus', self.plus, '--out', out, '--wait', 5, radio=radio)
        self.assertIn('PASS: DDM32 Plus installed and verified', self.output)
        self.assertTrue(radio.opened and radio.closed)
        self.check_untouched(radio)
        self.assertEqual(radio.images['progmem'], self.target)
        written = sorted({a // SECTOR * SECTOR for mem, op, a in radio.writes})
        self.assertEqual(written, list(range(START, START + 0x9000, SECTOR)))
        r = self.receipt(out)
        for key in ('writes_started', 'installation_verified', 'bootloader_unchanged', 'codec_slot_verified',
                    'data_sector_crcs_unchanged', 'restart_sent', 'precheck_program_matches_backup'):
            self.assertIs(r[key], True, key)
        self.assertEqual(r['planned_write_frames'], r['sent_write_frames'])
        self.assertEqual(r['outgoing_operations']['verified_restart_register'], 7)
        last_read = max(i for i, op in enumerate(radio.ops) if op in (7, 9))
        first_poke = min(i for i, op in enumerate(radio.ops) if op == 3)
        self.assertLess(last_read, first_poke)

    def test_live_program_or_data_mismatch_aborts_before_any_write(self):
        changed = bytearray(self.program); changed[START + 0x20000] ^= 0xff
        data = bytearray(self.data); data[0x800010] ^= 1
        for program, data_now in ((bytes(changed), self.data), (self.program, bytes(data))):
            radio = self.radio(program, data_now)
            out = self.new_out()
            with self.assertRaisesRegex(ValueError, 'Live pre-write comparison failed'):
                self.run_main('--backup', self.backup, '--plus', self.plus, '--out', out, '--wait', 5, radio=radio)
            self.assertFalse(radio.writes)
            self.assertFalse(radio.pokes)
            self.assertFalse(self.receipt(out)['writes_started'])

    def test_readback_corruption_blocks_restart(self):
        for where, message in ((0x100, 'Post-write verification failed'), (SLOT + 0x90, 'Codec slot changed'),
                               (START + 0x10, 'Post-write verification failed'), (0x0f0000, 'Codec slot changed')):
            radio = self.radio(self.program, corrupt_after_writes=where)
            out = self.new_out()
            with self.subTest(where=hex(where)), self.assertRaisesRegex(ValueError, message):
                self.run_main('--backup', self.backup, '--plus', self.plus, '--out', out, '--wait', 5, radio=radio)
            self.assertFalse(radio.pokes)
            self.check_untouched(radio)
            r = self.receipt(out)
            self.assertTrue(r['writes_started'])
            self.assertFalse(r['restart_sent'] or r['installation_verified'])

    def test_wrong_flash_id_stops_before_reads_or_writes(self):
        radio = self.radio(self.program, data_jedec='ef4017')
        out = self.new_out()
        with self.assertRaisesRegex(ValueError, 'flash IDs differ'):
            self.run_main('--backup', self.backup, '--plus', self.plus, '--out', out, '--wait', 5, radio=radio)
        self.assertFalse(radio.writes)
        self.assertNotIn(9, radio.ops)
        self.assertNotIn(7, radio.ops)

    def test_full_simulated_restore(self):
        broken = bytearray(self.target)
        broken[START + 3 * SECTOR:START + 5 * SECTOR] = b'\xff' * (2 * SECTOR)
        broken[SLOT + 0x200:SLOT + 0x210] = bytes(16)
        radio = self.radio(bytes(broken))
        out = self.new_out()
        self.run_main('--backup', self.backup, '--restore', '--out', out, '--wait', 5, radio=radio)
        self.assertIn('PASS: restored', self.output)
        self.assertEqual(radio.images['progmem'], self.program)
        self.assertEqual(radio.images['q128'], self.data)
        r = self.receipt(out)
        for key in ('writes_started', 'installation_verified', 'codec_slot_verified', 'restart_sent'):
            self.assertIs(r[key], True, key)

    def test_restore_readback_corruption_blocks_restart(self):
        radio = self.radio(self.target, corrupt_after_writes=SLOT + 0x90)
        out = self.new_out()
        with self.assertRaisesRegex(ValueError, 'Application readback failed'):
            self.run_main('--backup', self.backup, '--restore', '--out', out, '--wait', 5, radio=radio)
        self.assertFalse(radio.pokes)
        self.assertFalse(self.receipt(out)['restart_sent'])


@fx.need_upstream
class FromStockTests(unittest.TestCase):
    """First install over stock: the planner, its guards and a simulated install"""

    @classmethod
    def setUpClass(cls):
        cls.m = b.load_upstream()
        cls.tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.tmp.name)
        cls.data = fx.make_data()
        cls.plus = fx.write_plus(cls.root / 'plus', PLUS_APP)
        cls.runs = 0

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def setUp(self):
        fx.use_test_key(self)
        ctx = fx.synthetic_stock_codec(unit)
        self.codec, self.code, self.tables, self.program = ctx.__enter__()
        self.addCleanup(ctx.__exit__, None, None, None)
        FromStockTests.runs += 1
        self.backup = self.root / f'stock{self.runs}'
        fx.write_backup(self.backup, self.program, self.data)

    def run_main(self, *args, radio=None):
        try:
            return fx.run_tool_main(p, 'install_plus.py', args, radio)
        finally:
            self.output = fx.run_tool_main.output

    def test_detect_stock(self):
        d = radio_detect.detect(self.program)
        self.assertEqual((d['layout'], d['firmware'], d['install_mode'], d['supported']),
                         ('stock', 'stock', 'from_stock', True))
        self.assertEqual((d['stock_profile'], d['stock_version'], d['codec_tag']), ('DM32.01.01.047.bin', '047', 47))
        self.assertTrue(d['stock_tested'])

    def test_compose_matches_upstream_merge(self):
        target, prof = unit.compose_from_stock(self.program, PLUS_APP)
        self.assertEqual(target[:START], self.program[:START])
        self.assertEqual(target[START:START + len(PLUS_APP)], PLUS_APP)
        self.assertEqual(set(target[START + len(PLUS_APP):SLOT]), {0xff})
        expected = self.codec.merge(PLUS_APP, *self.codec.extract(self.program, board='dm32'))
        self.assertEqual(target[START:START + len(expected)], expected)
        slot = self.codec.parse_slot(target[SLOT:])
        self.assertEqual((slot['tag'], slot['code_sha256'], slot['tables_sha256']),
                         (47, b.sha(self.code), b.sha(self.tables)))
        self.assertEqual(slot['relocations'], [0x40, 0x84, 0x1000])
        self.assertEqual(p.slot_check(target), 47)
        self.assertEqual(prof['name'], 'DM32.01.01.047.bin')

    def test_planner_and_guards(self):
        plan = json.loads(self.run_main('--backup', self.backup, '--plus', self.plus, '--from-stock',
                                        '--out', self.root / 'never', '--plan-only'))
        self.assertEqual(plan['mode'], 'install_plus_from_stock')
        self.assertTrue(plan['boot_area_unchanged'])
        self.assertFalse(plan['codec_slot_unchanged'])
        self.assertTrue(plan['codec_slot_prepared_from_backup'])
        self.assertFalse(plan['data_chip_written'])
        self.assertGreater(plan['slot_sectors'], 0)
        self.assertGreaterEqual(int(plan['first'], 16), START)
        self.assertEqual((plan['stock_profile'], plan['stock_version'], plan['stock_version_tested']),
                         ('DM32.01.01.047.bin', '047', True))
        self.assertFalse((self.root / 'never').exists())
        # the normal install refuses a stock radio (no slot) and the stock path refuses a Classic radio
        with self.assertRaisesRegex(ValueError, 'No prepared codec slot'):
            self.run_main('--backup', self.backup, '--plus', self.plus, '--out', self.root / 'n2', '--plan-only')
        classic = self.root / f'classic{self.runs}'
        fx.write_backup(classic, fx.make_program(), self.data)
        with self.assertRaisesRegex(ValueError, 'Classic layout'):
            self.run_main('--backup', classic, '--plus', self.plus, '--from-stock', '--out', self.root / 'n3',
                          '--plan-only')
        # an unknown stock (codec not where a known build keeps it) is refused
        moved = bytearray(self.program)
        c = fx.STOCK_CODE_AT - 0x03000000
        moved[c:c + 8] = bytes(8)
        unknown = self.root / f'unknown{self.runs}'
        fx.write_backup(unknown, bytes(moved), self.data)
        with self.assertRaisesRegex(ValueError, 'Unknown stock firmware'):
            self.run_main('--backup', unknown, '--plus', self.plus, '--from-stock', '--out', self.root / 'n4',
                          '--plan-only')
        self.assertFalse(radio_detect.detect(bytes(moved))['supported'])
        # the boot area is never part of a plan, even if a composed image claimed otherwise
        bad = bytearray(self.program); bad[0] ^= 1
        with self.assertRaisesRegex(SystemExit, 'boot area'):
            p.make_plan('install_plus_from_stock', self.program, bytes(bad), 47)
        with self.assertRaisesRegex(SystemExit, 'codec slot'):
            p.make_plan('install_plus', self.program, unit.compose_from_stock(self.program, PLUS_APP)[0], 47)

    def test_full_simulated_install_from_stock(self):
        target, _ = unit.compose_from_stock(self.program, PLUS_APP)
        radio = fx.fake_radio(self.m, self.program, self.data)
        out = self.root / f'install{self.runs}'
        self.run_main('--backup', self.backup, '--plus', self.plus, '--from-stock', '--out', out, '--wait', 5,
                      radio=radio)
        self.assertIn('PASS: DDM32 Plus installed and verified', self.output)
        self.assertEqual(radio.images['progmem'], target)
        self.assertEqual(radio.images['progmem'][:START], self.program[:START])
        self.assertEqual(radio.images['q128'], self.data)
        self.assertTrue(all(mem == 'progmem' and a >= START for mem, op, a in radio.writes))
        self.assertTrue(any(a >= SLOT for mem, op, a in radio.writes))
        self.assertEqual(p.slot_check(radio.images['progmem']), 47)
        r = json.loads((out / 'install-receipt.json').read_text())
        for key in ('installation_verified', 'bootloader_unchanged', 'codec_slot_verified',
                    'data_sector_crcs_unchanged', 'restart_sent', 'precheck_program_matches_backup'):
            self.assertIs(r[key], True, key)
        self.assertEqual(r['mode'], 'install_plus_from_stock')

    def test_from_stock_slot_corruption_blocks_restart(self):
        radio = fx.fake_radio(self.m, self.program, self.data, corrupt_after_writes=SLOT + 0x100)
        out = self.root / f'bad{self.runs}'
        with self.assertRaisesRegex(ValueError, 'did not read back'):
            self.run_main('--backup', self.backup, '--plus', self.plus, '--from-stock', '--out', out, '--wait', 5,
                          radio=radio)
        self.assertFalse(radio.pokes)
        r = json.loads((out / 'install-receipt.json').read_text())
        self.assertTrue(r['writes_started'])
        self.assertFalse(r['restart_sent'])
        self.assertEqual(radio.images['q128'], self.data)


@fx.need_upstream
class DetectTests(unittest.TestCase):
    def test_classic_layouts(self):
        d = radio_detect.detect(fx.make_program(app=fx.pattern(0x9000, 3) + b'DDM32 Plus'))
        self.assertEqual((d['layout'], d['firmware'], d['install_mode'], d['codec_tag']),
                         ('classic', 'ddm32', 'over_classic', 47))
        # A build from before the rename to DDM32 Plus (v1.0.0-beta.1) is still ours
        d = radio_detect.detect(fx.make_program(app=fx.pattern(0x9000, 3) + b'DDM25 Plus'))
        self.assertEqual(d['firmware'], 'ddm32')
        d = radio_detect.detect(fx.make_program(app=b'open25-classic-r13' + fx.pattern(0x9000, 3)))
        self.assertEqual(d['firmware'], 'classic')
        d = radio_detect.detect(fx.make_program(slot=b'\xff' * 4096))
        self.assertFalse(d['supported'])
        self.assertIn('Unknown firmware', d['reason'])

    def test_tool_prints_json_from_backup(self):
        with tempfile.TemporaryDirectory() as td:
            folder = Path(td) / 'bk'
            fx.write_backup(folder, fx.make_program(), fx.make_data())
            import contextlib
            import io
            import sys
            out = io.StringIO()
            with patch.object(sys, 'argv', ['radio_detect.py', '--backup', str(folder)]), \
                    contextlib.redirect_stdout(out), patch('serial.Serial', side_effect=AssertionError('port')):
                radio_detect.main()
            self.assertEqual(json.loads(out.getvalue())['layout'], 'classic')


if __name__ == '__main__':
    unittest.main()
