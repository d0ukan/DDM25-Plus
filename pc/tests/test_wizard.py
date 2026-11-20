"""The install and restore flows' rules (no Qt)."""
import unittest

import fixtures as fx
from ddm32_app import parsers
from ddm32_app.wizard import FAILED, IDLE, OK, RestoreFlow, Wizard


class Pkg:
    def __init__(self, sha='a' * 64, installable=True):
        self.sha256, self.installable, self.name, self.version = sha, installable, 'DDM32 Plus 1.0.0', '1.0.0'


def backup_ok():
    return parsers.BackupResult(fx.BACKUP_OK.splitlines(), 0)


def install_pass():
    return parsers.InstallResult(fx.INSTALL_OK.format(plan=fx.plan_text()).splitlines(), 0, {'writes_started': True})


def install_timeout():
    return parsers.InstallResult(fx.INSTALL_TIMEOUT.format(plan=fx.plan_text()).splitlines(), 1,
                                 {'writes_started': False})


def install_failed_mid_write():
    return parsers.InstallResult(['INSTALLING: 179 ...', 'OSError: cable'], 1, {'writes_started': True})


def ready(w, detection=fx.DETECT_CLASSIC):
    w.set_port('COM7')
    w.set_package(Pkg())
    w.start_backup('b1')
    w.backup_finished(backup_ok())
    w.start_detect()
    w.detect_finished(dict(detection))
    return w


class WizardTest(unittest.TestCase):
    def test_initial(self):
        w = Wizard()
        self.assertEqual(w.buttons(), {'backup': False, 'plan': False, 'install': False})
        w.set_port('COM7')
        self.assertTrue(w.can_backup())

    def test_order_backup_detect_plan_install(self):
        w = Wizard()
        w.set_port('COM7')
        w.set_package(Pkg())
        self.assertFalse(w.can_detect())
        w.start_backup('b1')
        self.assertFalse(w.can_backup())
        w.backup_finished(backup_ok())
        self.assertTrue(w.can_detect())
        self.assertFalse(w.can_plan())              # the detection first
        w.start_detect()
        w.detect_finished(dict(fx.DETECT_CLASSIC))
        self.assertEqual(w.mode, 'install_plus')
        self.assertTrue(w.can_plan())
        self.assertFalse(w.can_install())
        w.start_plan()
        w.plan_finished(fx.PLAN_JSON)
        self.assertTrue(w.can_install())
        w.start_install()
        self.assertFalse(w.can_install())
        w.install_finished(install_pass())
        self.assertEqual(w.install, OK)
        self.assertFalse(w.can_plan())

    def test_stock_mode_and_plan_key(self):
        w = ready(Wizard(), fx.DETECT_STOCK)
        self.assertEqual(w.mode, 'install_plus_from_stock')
        w.start_plan()
        self.assertEqual(w.plan_key, ('b1', 'a' * 64, 'install_plus_from_stock'))
        w.plan_finished(fx.PLAN_STOCK_JSON)
        self.assertTrue(w.can_install())
        w.set_package(Pkg('b' * 64))                # another version: the plan is gone
        self.assertFalse(w.can_install())

    def test_unsupported_radio_blocks_plan(self):
        w = ready(Wizard(), fx.DETECT_UNKNOWN)
        self.assertIsNone(w.mode)
        self.assertFalse(w.can_plan())
        self.assertIn('Unknown firmware', w.message)
        w2 = Wizard()
        w2.set_port('COM7')
        w2.set_package(Pkg())
        w2.start_backup('b')
        w2.backup_finished(backup_ok())
        w2.start_detect()
        w2.detect_finished(None, 'boom')
        self.assertEqual(w2.detect, FAILED)
        self.assertFalse(w2.can_plan())

    def test_steps_refuse_out_of_order(self):
        w = Wizard()
        for fn in (w.start_detect, w.start_plan, w.start_install):
            with self.assertRaises(RuntimeError):
                fn()

    def test_finishing_never_starts_the_next_step(self):
        w = ready(Wizard())
        self.assertIsNone(w.running)
        self.assertEqual(w.plan, IDLE)
        w.start_plan()
        w.plan_finished(fx.PLAN_JSON)
        self.assertEqual(w.install, IDLE)

    def test_unsafe_plan_and_unsigned_package(self):
        w = ready(Wizard())
        w.start_plan()
        w.plan_finished(fx.PLAN_JSON, safe=False)
        self.assertEqual(w.plan, FAILED)
        self.assertFalse(w.can_install())
        w = ready(Wizard())
        w.set_package(Pkg(installable=False))
        self.assertFalse(w.can_plan())

    def test_one_plan_one_install(self):
        w = ready(Wizard())
        w.start_plan()
        w.plan_finished(fx.PLAN_JSON)
        w.start_install()
        w.install_finished(install_timeout())
        self.assertFalse(w.can_install())          # a timeout needs a new plan, by hand
        self.assertTrue(w.can_plan())

    def test_failed_install_after_writes_is_locked(self):
        w = ready(Wizard())
        w.start_plan()
        w.plan_finished(fx.PLAN_JSON)
        w.start_install()
        w.install_finished(install_failed_mid_write())
        self.assertTrue(w.locked)
        self.assertFalse(w.can_plan() or w.can_install() or w.can_detect())
        w.start_backup('b2')
        w.backup_finished(backup_ok())
        self.assertFalse(w.locked)

    def test_port_change_refused_while_radio_step_runs(self):
        w = Wizard()
        w.set_port('COM7')
        w.start_backup('b')
        with self.assertRaises(RuntimeError):
            w.set_port('COM8')


def restore_result(passed=True, started=True):
    lines = fx.RESTORE_OK.format(plan='{}').splitlines() if passed else ['INSTALLING: 244', 'OSError: x']
    return parsers.InstallResult(lines, 0 if passed else 1, {'writes_started': started})


class RestoreFlowTest(unittest.TestCase):
    def flow(self, kind, detection):
        f = RestoreFlow()
        f.port = 'COM7'
        f.choose(kind, 'bk')
        f.start_detect()
        f.detect_finished(dict(detection) if detection else None)
        return f

    def test_tool_follows_the_backup(self):
        self.assertEqual(self.flow('stock', fx.DETECT_STOCK).tool[0], 'install_firstboot')
        self.assertEqual(self.flow('previous', fx.DETECT_STOCK).tool[1], ['--restore-stock', '--use-saved-backup'])
        self.assertEqual(self.flow('previous', fx.DETECT_CLASSIC).tool, ('install_plus', ['--restore'], 'restore'))
        f = self.flow('stock', fx.DETECT_CLASSIC)                # stock restore only from a stock backup
        self.assertIsNone(f.tool)
        self.assertIn('stok yazılım değil', f.message)
        self.assertIsNone(self.flow('previous', fx.DETECT_UNKNOWN).tool)
        self.assertIsNone(self.flow('previous', None).tool)

    def test_steps(self):
        f = self.flow('stock', fx.DETECT_STOCK)
        self.assertFalse(f.can_restore())
        f.start_plan()
        f.plan_finished(fx.RESTORE_PLAN)
        self.assertTrue(f.can_restore())
        f.port = None
        self.assertFalse(f.can_restore())
        f.port = 'COM7'
        f.start_restore()
        self.assertFalse(f.can_restore())
        f.restore_finished(restore_result(False))
        self.assertEqual(f.restore, FAILED)
        self.assertFalse(f.can_restore())                      # a new plan first, by hand
        self.assertTrue(f.can_plan())
        f.start_plan()
        f.plan_finished(fx.RESTORE_PLAN)
        f.start_restore()
        f.restore_finished(restore_result())
        self.assertEqual(f.restore, OK)


if __name__ == '__main__':
    unittest.main()
