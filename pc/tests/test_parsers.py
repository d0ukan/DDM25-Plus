import unittest

import fixtures as fx
from ddm32_app import parsers


class BackupParserTest(unittest.TestCase):
    def test_ready_line(self):
        self.assertEqual(parsers.ready_wait(fx.BACKUP_OK.splitlines()[0]), ('COM7', 120))
        self.assertEqual(parsers.ready_wait('READY on COM9: turn the radio OFF, then ON normally, no buttons held. '
                                            'Waiting 600s.'), ('COM9', 600))
        self.assertIsNone(parsers.ready_wait('progmem pass 1: 25% (262144/1048576)'))

    def test_progress_is_overall(self):
        self.assertEqual(parsers.backup_progress('progmem pass 1: 0% (0/1048576)'), 0)
        p1 = parsers.backup_progress('progmem pass 1: 100% (1048576/1048576)')
        q1 = parsers.backup_progress('q128 pass 1: 100% (16777216/16777216)')
        q2 = parsers.backup_progress('q128 pass 2: 45% (7602176/16777216)')
        end = parsers.backup_progress('q128 pass 2: 100% (16777216/16777216)')
        self.assertEqual(p1, 2)
        self.assertEqual(q1, 50)
        self.assertTrue(q1 < q2 < end)
        self.assertEqual(end, 100)
        self.assertIsNone(parsers.backup_progress('VERIFIED: ...'))

    def test_verified(self):
        r = parsers.BackupResult(fx.BACKUP_OK.splitlines(), 0)
        self.assertTrue(r.verified)
        self.assertFalse(r.timeout)
        self.assertEqual(r.error, '')

    def test_verified_needs_exit_zero(self):
        self.assertFalse(parsers.BackupResult(fx.BACKUP_OK.splitlines(), 1).verified)

    def test_timeout_traceback(self):
        r = parsers.BackupResult(fx.BACKUP_TIMEOUT.splitlines(), 1)
        self.assertFalse(r.verified)
        self.assertTrue(r.timeout)
        self.assertTrue(r.error.startswith('DumpError: the radio did not start'))


class PlanParserTest(unittest.TestCase):
    def test_plan_json(self):
        plan = parsers.parse_plan(fx.plan_text())
        self.assertEqual(plan['changed_sectors'], 179)
        self.assertEqual((plan['first'], plan['last']), ('0xc000', '0xbe000'))
        self.assertTrue(parsers.plan_safe(plan))

    def test_plan_followed_by_install_output(self):
        plan = parsers.parse_plan(fx.INSTALL_OK.format(plan=fx.plan_text()))
        self.assertEqual(plan['changed_sectors'], 179)

    def test_unsigned_error(self):
        with self.assertRaises(ValueError) as cm:
            parsers.parse_plan(fx.PLAN_UNSIGNED)
        self.assertIn('not listed in the signed', str(cm.exception))

    def test_unsafe_plans(self):
        for change in ({'boot_area_unchanged': False}, {'codec_slot_unchanged': False}, {'mode': 'restore'},
                       {'changed_sectors': 0}, {'codec_slot_rewritten_from_backup': True},
                       {'data_chip_written': True}, {'first': '0x8000'}, {'first': None}, {'mode': 'other'}):
            plan = dict(fx.PLAN_JSON, **change)
            self.assertFalse(parsers.plan_safe(plan), change)
        self.assertFalse(parsers.plan_safe(fx.PLAN_JSON, 'install_plus_from_stock'))   # not the mode detected

    def test_stock_and_restore_plans(self):
        self.assertTrue(parsers.plan_safe(fx.PLAN_STOCK_JSON, 'install_plus_from_stock'))
        for change in ({'codec_slot_prepared_from_backup': False}, {'boot_area_unchanged': False},
                       {'data_chip_written': True}, {'codec_slot_rewritten_from_backup': True}, {'first': '0xb000'}):
            self.assertFalse(parsers.plan_safe(dict(fx.PLAN_STOCK_JSON, **change), 'install_plus_from_stock'), change)
        self.assertTrue(parsers.plan_safe(fx.RESTORE_PLAN, 'restore_stock'))
        self.assertTrue(parsers.plan_safe(dict(fx.RESTORE_PLAN, mode='restore'), 'restore'))
        for change in ({'changed_sectors': 200}, {'first': '0xd000'}, {'boot_area_unchanged': False}):
            self.assertFalse(parsers.plan_safe(dict(fx.RESTORE_PLAN, **change), 'restore_stock'), change)

    def test_detect_json(self):
        d = parsers.parse_detect(fx.detect_text(fx.DETECT_STOCK))
        self.assertEqual((d['layout'], d['install_mode']), ('stock', 'from_stock'))
        with self.assertRaises(ValueError):
            parsers.parse_detect('{"layout": "stock"}')

    def test_restore_progress(self):
        values = [parsers.install_progress(l, parsers.RESTORE_PHASES)
                  for l in fx.RESTORE_OK.format(plan='{}').splitlines()]
        values = [v for v in values if v is not None]
        self.assertEqual(values, sorted(values))
        self.assertEqual(values[-1], 100)


class InstallParserTest(unittest.TestCase):
    def test_progress_phases(self):
        values = [parsers.install_progress(l) for l in fx.INSTALL_OK.format(plan='{}').splitlines()]
        values = [v for v in values if v is not None]
        self.assertEqual(values, sorted(values))
        self.assertEqual(values[-1], 100)
        self.assertEqual(parsers.install_progress('Written and sector-readback checked: 50% (90/179)'), 18 + 27)

    def test_pass_with_receipt(self):
        receipt = {'writes_started': True, 'installation_verified': True, 'bootloader_unchanged': True,
                   'codec_slot_verified': True, 'data_sector_crcs_unchanged': True, 'restart_sent': True}
        r = parsers.InstallResult(fx.INSTALL_OK.format(plan=fx.plan_text()).splitlines(), 0, receipt)
        self.assertTrue(r.passed and r.verified)
        self.assertEqual(r.flags()['data_sector_crcs_unchanged'], True)

    def test_timeout_before_writes(self):
        receipt = {'writes_started': False, 'error': 'DumpError: the radio did not start its serial boot ROM '
                                                     'within 600 s'}
        r = parsers.InstallResult(fx.INSTALL_TIMEOUT.format(plan=fx.plan_text()).splitlines(), 1, receipt)
        self.assertFalse(r.passed)
        self.assertTrue(r.timeout)
        self.assertFalse(r.writes_started)

    def test_no_receipt_counts_as_started(self):
        r = parsers.InstallResult(['Traceback', 'OSError: boom'], 1, None)
        self.assertTrue(r.writes_started)
        self.assertEqual(r.error, 'OSError: boom')


class HealthParserTest(unittest.TestCase):
    def test_health(self):
        s = parsers.health_summary(parsers.parse_health(fx.HEALTH))
        self.assertTrue(s['ok'])
        self.assertEqual(s['uptime_ms'], 82000)
        self.assertEqual((s['heap_free'], s['heap_min_free']), (6104, 4048))
        self.assertEqual(s['min_task'], 'IDLE')
        self.assertFalse(s['crash_valid'])
        self.assertEqual(s['reset_cause'], 'power')
        self.assertEqual(list(s['tasks'])[0], 'p25_lab')     # least free first
        self.assertEqual(s['tasks']['host'], 1200)
        self.assertEqual(parsers.format_uptime(82000), '0:01:22')

    def test_crash_record(self):
        text = fx.HEALTH.replace('crash.valid=0', 'crash.valid=1\ncrash.task=ui\ncrash.pc=0x0001c0de\n'
                                                  'crash.uptime_ms=5000')
        s = parsers.health_summary(parsers.parse_health(text))
        self.assertTrue(s['crash_valid'])
        self.assertEqual((s['crash_task'], s['crash_pc']), ('ui', '0x0001c0de'))

    def test_failed_view(self):
        s = parsers.health_summary(parsers.parse_health('--- view tasks (20) status 3\n'))
        self.assertFalse(s['ok'])


class BuildParserTest(unittest.TestCase):
    def test_build(self):
        r = parsers.parse_build(fx.BUILD_OUT)
        self.assertEqual(r['memory']['RAM']['used'], 207424)
        self.assertEqual(r['memory']['FLASH']['size'], 896 * 1024)
        self.assertTrue(r['sha256'].startswith('42072ce9'))
        self.assertIs(r['forced_rebuild_identical'], True)
        self.assertEqual(r['compiler_warnings'], 0)


if __name__ == '__main__':
    unittest.main()
