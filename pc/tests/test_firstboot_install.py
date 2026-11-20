# SPDX-License-Identifier: GPL-3.0-or-later
"""The program-flash writer guard (InstallPort) on the pinned FakeRadio, and the stock restore."""
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import rtfixtures as fx
from radiotools import install_firstboot as f
from radiotools import dm32_readonly_backup as b

@fx.need_upstream
class InstallerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):cls.m=b.load_upstream()
    def setup_radio(self,corrupt=False,skip_original=False,bulk=False):
        old=bytes(range(256))*4096
        new=bytearray(old);new[f.START:f.START+4096]=b'\xa5'*4096
        new[f.START+4096:f.START+8192]=b'\xff'*4096
        data=b'private-data'*1024;data=data[:8192]
        class Corrupt(self.m.FakeRadio):
            def write(self,frame):
                super().write(frame)
                if self.running and len(frame)>16 and frame[:2]==b'\x02\x05' and frame[16]==2:
                    image=bytearray(self.images['progmem']);image[f.START]&=0xfe
                    self.images['progmem']=bytes(image)
        fake=(Corrupt if corrupt else self.m.FakeRadio)({'progmem':old,'q128':data})
        fake.running=True
        fake.write_op=bulk
        guard=f.InstallPort(fake,self.m,old,bytes(new),skip_original=skip_original,bulk=bulk);guard.monitor=True
        guard.limits={'progmem':f.SIZE,'q128':len(data)}
        radio=self.m.Radio(guard,log=lambda *a:None,retries=0)
        radio.profile=dict(self.m.C7_BOARDS['dm32']);radio.v6={'flags':0}
        radio.sector_op=bulk
        return fake,guard,radio,old,bytes(new),data
    def test_bulk_uses_exact_sector_frames_and_final_app_verification(self):
        fake,g,r,old,new,data=self.setup_radio(skip_original=True,bulk=True)
        g.authorize_saved_backup();f.write_plan(r,g)
        self.assertEqual(len(g.plan),2);self.assertEqual([op for mem,op,a in fake.writes],[14,14])
        self.assertEqual(fake.images['progmem'],new);self.assertEqual(fake.images['q128'],data)
        out=io.BytesIO();r.dump(out,'progmem',f.START,f.SIZE);g.authorize_reset_application(out.getvalue())
        self.assertEqual(out.getvalue(),new[f.START:])
    def test_bulk_bad_readback_stops_without_reerase_or_restart(self):
        fake,g,r,old,new,data=self.setup_radio(skip_original=True,bulk=True);fake.bad_write=1
        g.authorize_saved_backup()
        with self.assertRaisesRegex(ValueError,'CRC readback mismatch'):f.write_plan(r,g)
        self.assertEqual(len(fake.writes),1);self.assertFalse(g.closed_writes);self.assertFalse(g.reset_plan)
        self.assertEqual(fake.images['q128'],data)
    def test_real_monitor_protocol_writes_exact_app_and_preserves_other_memory(self):
        fake,g,r,old,new,data=self.setup_radio();crcs=f.crc_list(data)
        g.authorize(old,crcs,crcs);f.write_plan(r,g)
        out=io.BytesIO();r.dump(out,'progmem',0,f.SIZE)
        self.assertEqual(out.getvalue(),new);self.assertEqual(fake.images['q128'],data)
        self.assertEqual(new[:f.START],old[:f.START])
        self.assertTrue(all(mem=='progmem' and a>=f.START for mem,op,a in fake.writes))
        self.assertEqual(g.index,len(g.plan));self.assertTrue(g.closed_writes)
        with self.assertRaises(ValueError):g.write(f.spi_packet(b'\x06'))
        g.authorize_reset(out.getvalue(),crcs,crcs);r.reset_chip()
        self.assertEqual(g.reset_index,7);self.assertFalse(fake.errors)
    def test_boot_modification_and_changed_live_original_are_refused(self):
        fake,g,r,old,new,data=self.setup_radio();crcs=f.crc_list(data)
        changed=bytearray(new);changed[0]^=1
        with self.assertRaises(ValueError):f.InstallPort(fake,self.m,old,bytes(changed))
        for before,actual in ((new,crcs),(old,crcs[:-1])):
            with self.assertRaises(ValueError):g.authorize(before,actual,crcs)
        self.assertFalse(g.enabled);self.assertFalse(fake.writes)
    def test_no_writes_before_precheck_and_no_boot_data_chip_erase_or_poke(self):
        fake,g,r,old,new,data=self.setup_radio()
        frames=[f.spi_packet(b'\x06'),f.spi_packet(b'\xc7'),f.spi_packet(b'\x60'),
                f.spi_packet(b'\x20\0\0\0'),f.packet(3,bytes(8)),f.packet(14,bytes(4106)),
                f.packet(5,bytes.fromhex('140a00000312')+bytes.fromhex('00040000')+bytes.fromhex('20700000'))]
        for frame in frames:
            with self.assertRaises(ValueError):g.write(frame)
        self.assertFalse(fake.writes);self.assertFalse(fake.pokes);self.assertFalse(fake.wel)
    def test_page_content_and_order_are_exact_and_not_retryable(self):
        fake,g,r,old,new,data=self.setup_radio();crcs=f.crc_list(data);g.authorize(old,crcs,crcs)
        g.write(g.plan[0])
        with self.assertRaises(ValueError):g.write(g.plan[0])
        g.write(g.plan[1]);g.write(g.plan[2])
        bad=bytearray(g.plan[3]);bad[-1]^=1;bad[4]=sum(bad[6:])&255
        with self.assertRaises(ValueError):g.write(bad)
        self.assertEqual(g.index,3)
        with self.assertRaises(ValueError):g.finish_writes()
        with self.assertRaises(ValueError):g.authorize_reset(new,crcs,crcs)
    def test_sector_readback_failure_stops_and_does_not_authorize_reset(self):
        fake,g,r,old,new,data=self.setup_radio(corrupt=True);crcs=f.crc_list(data)
        g.authorize(old,crcs,crcs)
        with self.assertRaisesRegex(ValueError,'Sector readback mismatch'):f.write_plan(r,g)
        self.assertFalse(g.closed_writes);self.assertFalse(g.reset_plan)
        self.assertTrue(all(a<f.START+4096 for mem,op,a in fake.writes))
        self.assertEqual(fake.images['progmem'][:f.START],old[:f.START])
        self.assertEqual(fake.images['q128'],data)
    def test_wrong_full_readback_or_changed_data_prevents_restart(self):
        fake,g,r,old,new,data=self.setup_radio();crcs=f.crc_list(data)
        g.authorize(old,crcs,crcs);f.write_plan(r,g)
        for actual,actual_data in ((old,crcs),(new,crcs[:-1])):
            with self.assertRaises(ValueError):g.authorize_reset(actual,actual_data,crcs)
        with self.assertRaises(ValueError):r.reset_chip()
        self.assertFalse(fake.pokes)
    def test_owner_skip_blocks_all_pre_reads_and_permits_only_new_application_verification(self):
        fake,g,r,old,new,data=self.setup_radio(skip_original=True)
        g.authorize_saved_backup()
        for action in (lambda:r.read_flash('progmem',f.START,16),lambda:r.sums('progmem',f.START,1),
                       lambda:r.read_flash('q128',0,16)):
            with self.assertRaises(ValueError):action()
        f.write_plan(r,g)
        for action in (lambda:r.read_flash('progmem',0,16),lambda:r.sums('progmem',0,1),
                       lambda:r.sums('q128',0,1)):
            with self.assertRaises(ValueError):action()
        out=io.BytesIO();r.dump(out,'progmem',f.START,f.SIZE)
        self.assertEqual(out.getvalue(),new[f.START:]);g.authorize_reset_application(out.getvalue());r.reset_chip()
        self.assertEqual(g.reset_index,7);self.assertEqual(fake.images['q128'],data)
    def test_saved_backup_mode_still_requires_exact_application_before_reset(self):
        fake,g,r,old,new,data=self.setup_radio(skip_original=True);g.authorize_saved_backup()
        with self.assertRaises(ValueError):g.authorize_reset_application(new[f.START:])
        f.write_plan(r,g)
        with self.assertRaises(ValueError):g.authorize_reset_application(old[f.START:])
        self.assertFalse(g.reset_plan)
    def restore_case(self,current):
        stock=bytes(range(256))*4096;data=b'private-data'*1024
        with patch.object(f,'saved_images',lambda _: {'progmem-pass1.bin':stock,'q128-pass1.bin':data}):
            basis,target,saved_data=f.restore_inputs(Path('unused'))
        self.assertEqual(target,stock);self.assertEqual(saved_data,data)
        fake=self.m.FakeRadio({'progmem':current,'q128':data});fake.running=True
        g=f.InstallPort(fake,self.m,basis,target,skip_original=True);g.monitor=True
        g.limits={'progmem':f.SIZE,'q128':len(data)}
        r=self.m.Radio(g,log=lambda *a:None,retries=0)
        r.profile=dict(self.m.C7_BOARDS['dm32']);r.v6={'flags':0};r.sector_op=False
        self.assertEqual(g.todo,list(range(f.START,f.SIZE,f.SECTOR)))
        g.authorize_saved_backup();f.write_plan(r,g)
        out=io.BytesIO();r.dump(out,'progmem',f.START,f.SIZE);g.authorize_reset_application(out.getvalue())
        self.assertEqual(fake.images['progmem'],stock);self.assertEqual(fake.images['q128'],data)
        self.assertTrue(all(mem=='progmem' and a>=f.START for mem,op,a in fake.writes))
        r.reset_chip();self.assertEqual(g.reset_index,7)
    def test_restore_rewrites_whole_application_after_install(self):
        stock=bytes(range(256))*4096
        installed=stock[:f.START]+b'\x5a'*8192+b'\xff'*(f.SIZE-f.START-8192)
        self.restore_case(installed)
    def test_restore_recovers_interrupted_install(self):
        stock=bytes(range(256))*4096
        broken=bytearray(stock);broken[f.START:f.START+3*f.SECTOR]=b'\xff'*(3*f.SECTOR)
        broken[f.START+5*f.SECTOR:f.START+5*f.SECTOR+100]=b'\x00'*100
        self.restore_case(bytes(broken))

    # ---- the public command line: stock restore only ----
    def stock_backup(self, td):
        stock = fx.pattern(f.SIZE, 23)
        data = fx.make_data()
        folder = Path(td) / 'stock-backup'
        fx.write_backup(folder, stock, data)
        return folder, stock, data

    def run_main(self, *args, radio=None):
        try:
            return fx.run_tool_main(f, 'install_firstboot.py', args, radio)
        finally:
            self.output = fx.run_tool_main.output

    def test_only_restore_stock_is_offered(self):
        with tempfile.TemporaryDirectory() as td:
            folder, stock, data = self.stock_backup(td)
            for args in (('--backup', folder, '--out', Path(td) / 'o'),
                         ('--backup', folder, '--out', Path(td) / 'o', '--restore-stock'),
                         ('--backup', folder, '--out', Path(td) / 'o', '--use-saved-backup'),
                         ('--backup', folder, '--out', Path(td) / 'o', '--restore-stock', '--use-saved-backup',
                          '--wait', 0)):
                with self.subTest(args=args), self.assertRaises(SystemExit) as caught:
                    self.run_main(*args)
                self.assertEqual(caught.exception.code, 2)
            plan = json.loads(self.run_main('--backup', folder, '--out', Path(td) / 'o', '--restore-stock',
                                            '--use-saved-backup', '--plan-only'))
            self.assertEqual(plan['mode'], 'restore_stock')
            self.assertEqual(plan['changed_sectors'], (f.SIZE - f.START) // f.SECTOR)
            self.assertTrue(plan['boot_area_unchanged'])
            self.assertFalse(plan['data_chip_written'])
            self.assertFalse((Path(td) / 'o').exists())

    def test_simulated_stock_restore(self):
        with tempfile.TemporaryDirectory() as td:
            folder, stock, data = self.stock_backup(td)
            installed = stock[:f.START] + fx.make_program()[f.START:]       # a Classic-layout install over it
            radio = fx.fake_radio(self.m, installed, data)
            out = Path(td) / 'restore'
            self.run_main('--backup', folder, '--out', out, '--restore-stock', '--use-saved-backup', '--wait', 5,
                          radio=radio)
            self.assertIn('PASS: stock application restored', self.output)
            self.assertEqual(radio.images['progmem'], stock)
            self.assertEqual(radio.images['q128'], data)
            self.assertTrue(all(mem == 'progmem' and a >= f.START for mem, op, a in radio.writes))
            r = json.loads((out / 'install-receipt.json').read_text())
            self.assertTrue(r['installation_verified'] and r['restart_sent'])
            self.assertIs(r['preinstall_reads_requested'], False)
            self.assertEqual(r['planned_write_frames'], r['sent_write_frames'])

    def test_restore_refuses_other_flash_chips(self):
        with tempfile.TemporaryDirectory() as td:
            folder, stock, data = self.stock_backup(td)
            radio = fx.fake_radio(self.m, stock, data, data_jedec='ef4019')
            with self.assertRaisesRegex(ValueError, 'flash IDs differ'):
                self.run_main('--backup', folder, '--out', Path(td) / 'r', '--restore-stock', '--use-saved-backup',
                              '--wait', 5, radio=radio)
            self.assertFalse(radio.writes)


if __name__ == '__main__':
    unittest.main()
