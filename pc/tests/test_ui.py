"""The window offscreen with a fake process runner, fake ports, a fake network and releases signed with a test key:
no tool, port or network is ever used."""
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest

os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')

import fixtures as fx  # noqa: E402
import rtfixtures as rt  # noqa: E402
from PyQt5.QtWidgets import QApplication  # noqa: E402

from ddm32_app import dataupdate, disclaimer, parsers, paths  # noqa: E402
from ddm32_app.health_tab import plain_lines, verdict  # noqa: E402
from ddm32_app.main_window import MainWindow  # noqa: E402
from ddm32_app.ports import PortInfo  # noqa: E402
from ddm32_app.update_tab import BACKUP, CHECK, DONE, INSTALL, PREP  # noqa: E402

APP = QApplication.instance() or QApplication([])


class UiBase(unittest.TestCase):
    ports = staticmethod(fx.fake_ports)
    versions = ('1.0.0', '1.1.0-beta.1')
    accepted = True
    frozen = False
    formats = None
    min_compatible = None

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.top = Path(self.tmp.name)
        self.data = self.top / 'data'
        self.repo = self.top / 'repo'
        self.repo.mkdir()
        old = os.environ.get('DDM32_DATA')
        os.environ['DDM32_DATA'] = str(self.data)
        self.addCleanup(lambda: os.environ.__setitem__('DDM32_DATA', old) if old else None)
        rt.use_test_key(self)
        self.layout = paths.Layout.source(data=self.data, python=sys.executable, repo=self.repo)
        self.layout.force_build = True
        if self.frozen:
            self.layout.mode, self.layout.runner_exe = 'frozen', str(self.top / 'DDM32Plus.exe')
        self.layout.ensure()
        self.root = self.layout.root
        self.pub = fx.publish(self.layout.releases, list(self.versions), formats=self.formats,
                              min_compatible=self.min_compatible) if self.versions else {}
        if self.accepted:
            disclaimer.accept()
        self.answers = []          # confirm answers in order; default True
        self.asked = []
        self.helps = []
        self.disclaimer_asks = 0
        self.helper_calls = []
        fx.FakeRunner.instances.clear()

        def confirm(title, text, yes=None):
            self.asked.append((title, text))
            return self.answers.pop(0) if self.answers else True

        def ask():
            self.disclaimer_asks += 1
            if self.disclaimer_answer:
                disclaimer.accept()
            return self.disclaimer_answer
        self.disclaimer_answer = True
        self.w = MainWindow(layout=self.layout, runner_factory=fx.FakeRunner, port_lister=self.ports,
                            confirm=confirm, fetch=self.fake_fetch, download=self.fake_download, run_async=False,
                            show_help_dialog=self.helps.append, ask_disclaimer=ask,
                            start_helper=lambda p, a: self.helper_calls.append((p, a)) or True, auto_check=False)
        self.u = self.w.updater

    def tearDown(self):
        self.w.close_logs()
        self.w.deleteLater()
        self.tmp.cleanup()

    def fake_fetch(self, url, limit=None):
        raise AssertionError('no network in this test: ' + url)

    def fake_download(self, url, dest, size, progress=None):
        raise AssertionError('no network in this test: ' + url)

    def runner(self):
        return fx.FakeRunner.instances[-1]

    def arg(self, args, name):
        return args[args.index(name) + 1]

    def select(self, version):
        self.u.package_combo.setCurrentIndex(self.u.package_combo.findData(version))


class FlowSteps(UiBase):
    def do_backup(self, program=None):
        self.assertTrue(self.u.prep_btn.isEnabled())
        self.u.prep_btn.click()
        self.assertEqual(self.u.page(), BACKUP)
        self.u.backup_btn.click()
        r = self.runner()
        out = Path(self.arg(r.calls[0][1], '--out'))
        if program is not None:
            fx.write_progmem_backup(out, program)
        lines = fx.BACKUP_OK.splitlines()
        r.line.emit(lines[0])
        self.assertFalse(self.u.backup_banner.isHidden())
        self.assertEqual(self.u.backup_banner.remaining, 120)
        for l in lines[1:]:
            r.line.emit(l)
        r.finished.emit(0)
        return out

    def detect(self, d=fx.DETECT_CLASSIC):
        r = self.runner()
        self.assertEqual(r.calls[0][1][:2], ['-u', 'radiotools/radio_detect.py'])
        self.assertNotIn('--port', r.calls[0][1])            # detection opens no port
        r.feed(fx.detect_text(d), 0)

    def plan(self, plan=fx.PLAN_JSON):
        self.assertEqual(self.u.page(), CHECK)
        r = self.runner()
        self.assertIn('--plan-only', r.calls[0][1])
        r.feed(fx.plan_text(plan), 0)
        return r.calls[0][1]

    def install(self, args_check=None, receipt=None, text=None):
        self.u.check_btn.click()
        self.assertEqual(self.u.page(), INSTALL)
        self.u.install_btn.click()
        r = self.runner()
        args = r.calls[0][1]
        install_dir = Path(self.arg(args, '--out'))
        install_dir.mkdir(parents=True)
        (install_dir / 'install-receipt.json').write_text(json.dumps(receipt or {
            'writes_started': True, 'installation_verified': True, 'bootloader_unchanged': True,
            'codec_slot_verified': True, 'data_sector_crcs_unchanged': True, 'restart_sent': True}))
        r.feed(text or fx.INSTALL_OK.format(plan=fx.plan_text()), 0 if text is None else 1)
        return args


class UpdateFlowTest(FlowSteps):
    def test_prep_picks_newest_signed_and_cable(self):
        self.assertEqual(self.u.page(), PREP)
        self.assertEqual(self.u.wizard.package.version, '1.1.0-beta.1')
        self.assertIn('1.1.0-beta.1', self.u.version_label.text())
        self.assertEqual(self.w.port_state.port, 'COM7')
        self.assertIn('Telsiz kablosu bulundu (COM7)', self.u.port_picker.label.text())
        self.assertTrue(self.u.port_picker.combo.isHidden())
        self.assertTrue(self.u.download_btn.isHidden())          # already downloaded and verified
        self.assertEqual(self.u.sha_label.text(), rt.sm.sha256(fx.release_app('1.1.0-beta.1')))

    def test_full_flow_over_classic(self):
        out = self.do_backup()
        args = fx.FakeRunner.instances[0].calls[0][1]
        self.assertEqual(args[:2], ['-u', 'radiotools/dm32_readonly_backup.py'])
        self.assertEqual((self.arg(args, '--port'), self.arg(args, '--wait')), ('COM7', '120'))
        self.assertTrue(str(out).startswith(str(self.data)))     # user data outside the program folder
        self.assertTrue((self.layout.backups / f'backup-{out.name.split("-")[-1]}.log').is_file())
        self.assertEqual(len(fx.FakeRunner.instances), 2)        # detection follows by itself (no port)
        self.detect()
        self.assertIn('DDM32 Plus var', self.u.detect_status.text())
        self.assertEqual(len(fx.FakeRunner.instances), 3)        # then the plan (no port)
        plan_args = self.plan()
        self.assertNotIn('--from-stock', plan_args)
        self.assertEqual(Path(self.arg(plan_args, '--plus')), self.layout.releases / '1.1.0-beta.1')
        text = self.u.check_status.text()
        self.assertIn('179 sektör', text)
        self.assertIn('Codec bölümü değişmez', text)
        self.assertEqual(len(fx.FakeRunner.instances), 3)
        self.u.check_btn.click()
        self.assertIn('beta', self.u.warn_label.text())
        self.answers = [False]
        self.u.install_btn.click()
        self.assertEqual(len(fx.FakeRunner.instances), 3)        # refused: nothing started
        title, q = self.asked[-1]
        for part in ('1.1.0-beta.1', '179', out.name, disclaimer.SHORT, 'Güncelleme: yalnızca uygulama'):
            self.assertIn(part, q)
        args = self.install()
        self.assertNotIn('--plan-only', args)
        self.assertNotIn('--from-stock', args)
        self.assertEqual(self.arg(args, '--wait'), '600')
        self.assertEqual(self.u.page(), DONE)
        s = paths.load_settings()
        self.assertEqual(s['installed_version'], '1.1.0-beta.1')
        self.assertEqual(s['restore_previous_backup'], str(out))
        self.assertEqual(self.w.home.state, 'current')

    def test_first_install_from_stock(self):
        self.do_backup()
        self.detect(fx.DETECT_STOCK)
        self.assertIn('stok yazılım var: .047', self.u.detect_status.text())
        plan_args = self.plan(fx.PLAN_STOCK_JSON)
        self.assertIn('--from-stock', plan_args)
        self.assertIn('codec bölümü, 230 sektör', self.u.check_status.text())
        args = self.install()
        self.assertIn('--from-stock', args)
        q = self.asked[-1][1]
        self.assertIn('İlk kurulum (stoktan)', q)
        self.assertIn(disclaimer.SHORT, q)
        self.assertEqual(self.u.page(), DONE)
        self.assertEqual(paths.load_settings()['stock_backups'], [self.u.wizard.backup_dir.__str__()])

    def test_untested_stock_version_warns(self):
        self.do_backup()
        self.detect(fx.DETECT_STOCK_049)
        self.assertIn('denenmedi', self.u.detect_status.text())
        self.plan(dict(fx.PLAN_STOCK_JSON, stock_version='049', stock_version_tested=False))
        self.u.check_btn.click()
        self.assertIn('yalnızca stok .047 üzerinde denendi', self.u.warn_label.text())

    def test_unsupported_radio_stops_before_plan(self):
        self.do_backup()
        self.detect(fx.DETECT_UNKNOWN)
        self.assertIn('Desteklenmeyen', self.u.detect_status.text())
        self.assertEqual(len(fx.FakeRunner.instances), 2)        # no plan
        self.assertFalse(self.u.check_btn.isEnabled())

    def test_unsafe_plan_blocks_install(self):
        self.do_backup()
        self.detect()
        self.plan(dict(fx.PLAN_JSON, codec_slot_unchanged=False))
        self.assertIn('güvenli görünmüyor', self.u.check_status.text())
        self.assertFalse(self.u.check_btn.isEnabled())
        self.do_plan_mismatch()

    def do_plan_mismatch(self):
        # a stock plan for a Classic radio is not accepted either
        self.u.replan_btn.click()
        self.detect()
        self.plan(fx.PLAN_STOCK_JSON)
        self.assertFalse(self.u.check_btn.isEnabled())

    def test_downgrade_shows_warning(self):
        program = rt.make_program(app=self.pub['1.1.0-beta.1'][0])
        self.select('1.0.0')
        self.do_backup(program)
        self.detect()
        self.plan()
        self.u.check_btn.click()
        self.assertIn('Eski bir sürüme geçiyorsunuz (1.1.0-beta.1 → 1.0.0)', self.u.warn_label.text())
        self.assertNotIn('biçimi geriye gidiyor', self.u.warn_label.text())

    def test_failed_install_after_writes_not_retried(self):
        self.do_backup()
        self.detect()
        self.plan()
        self.install(receipt={'writes_started': True, 'error': 'OSError: cable'}, text='INSTALLING: 179\nOSError: cable')
        self.assertEqual(len(fx.FakeRunner.instances), 4)
        self.assertIn('yazma başladıktan sonra durdu', self.u.install_status.text())
        self.assertFalse(self.u.install_btn.isEnabled())
        self.assertFalse(self.u.to_restore_btn.isHidden())
        self.u.to_restore_btn.click()
        self.assertIs(self.w.tabs.currentWidget(), self.w.restore)
        self.assertEqual(len(fx.FakeRunner.instances), 4)

    def test_install_timeout_back_to_check_by_hand(self):
        self.do_backup()
        self.detect()
        self.plan()
        self.install(receipt={'writes_started': False}, text=fx.INSTALL_TIMEOUT.format(plan=fx.plan_text()))
        self.assertIn('Hiçbir şey yazılmadı', self.u.install_status.text())
        self.assertFalse(self.u.install_btn.isEnabled())
        self.u.to_check_btn.click()
        self.assertEqual(self.u.page(), CHECK)
        self.assertFalse(self.u.replan_btn.isHidden())
        n = len(fx.FakeRunner.instances)
        self.u.replan_btn.click()
        self.assertEqual(len(fx.FakeRunner.instances), n + 1)
        self.assertIn('radiotools/radio_detect.py', self.runner().calls[0][1])

    def test_cancel_while_waiting(self):
        self.u.prep_btn.click()
        self.u.backup_btn.click()
        self.runner().line.emit(fx.BACKUP_OK.splitlines()[0])
        self.assertFalse(self.u.backup_cancel.isHidden())
        self.u.backup_cancel.click()
        self.assertTrue(self.runner().killed)

    def test_radio_lock(self):
        self.u.prep_btn.click()
        self.u.backup_btn.click()
        self.w._guard()
        self.assertTrue(self.w.port_state.locked)
        self.assertFalse(self.w.health.read_btn.isEnabled())
        self.assertNotEqual(self.w.busy_text(), '')
        self.runner().feed(fx.BACKUP_TIMEOUT, 1)
        self.w._guard()
        self.assertTrue(self.w.health.read_btn.isEnabled())
        self.assertEqual(self.w.busy_text(), '')


class DowngradeTest(FlowSteps):
    formats = {'1.0.0': (1, 1), '1.1.0-beta.1': (2, 3)}
    min_compatible = {'1.1.0-beta.1': '1.1.0-beta.1'}

    def test_risky_downgrade_needs_a_second_confirmation(self):
        program = rt.make_program(app=self.pub['1.1.0-beta.1'][0])
        self.select('1.0.0')
        self.do_backup(program)
        self.detect()
        self.plan()
        self.u.check_btn.click()
        text = self.u.warn_label.text()
        for part in ('Ayar biçimi geriye gidiyor (2 → 1)', 'Kanal biçimi geriye gidiyor (3 → 1)', 'P25 kanalı',
                     'en az 1.1.0-beta.1'):
            self.assertIn(part, text)
        n = len(fx.FakeRunner.instances)
        self.answers = [True, False]
        self.u.install_btn.click()
        self.assertEqual(self.asked[-1][0], 'Riskli sürüm değişikliği')
        self.assertIn(disclaimer.SHORT, self.asked[-1][1])
        self.assertEqual(len(fx.FakeRunner.instances), n)          # second confirmation refused: nothing runs
        self.answers = [True, True]
        self.u.install_btn.click()
        self.assertEqual(len(fx.FakeRunner.instances), n + 1)
        self.assertEqual(Path(self.arg(self.runner().calls[0][1], '--plus')), self.layout.releases / '1.0.0')


class NotDownloadedTest(UiBase):
    def setUp(self):
        super().setUp()
        # only the signed list is known: the newest version is not downloaded yet
        import shutil
        shutil.rmtree(self.layout.releases / '1.1.0-beta.1')
        self.w.rel.reload_packages()

    def test_download_needed_and_verified(self):
        self.select('1.1.0-beta.1')
        self.assertFalse(self.u.download_btn.isHidden())
        self.assertFalse(self.u.prep_btn.isEnabled())
        self.assertIn('İndir ve doğrula', self.u.version_label.text())
        fv = self.w.rel.catalog.find('1.1.0-beta.1')
        good = self.zip_for('1.1.0-beta.1')
        self.fix_entry(fv, good)

        def download(url, dest, size, progress=None):
            Path(dest).write_bytes(self.payload)
            return dest
        self.w.rel.download_fn = download
        self.payload = b'tampered' + good
        self.u.download_btn.click()
        self.assertIn('İndirme başarısız', self.u.version_label.text())
        self.assertFalse((self.layout.releases / '1.1.0-beta.1').exists())
        self.payload = good
        self.u.download_btn.click()
        self.assertTrue(self.u.prep_btn.isEnabled())
        self.assertTrue(self.u.download_btn.isHidden())

    def zip_for(self, version):
        import io
        import zipfile
        src = self.top / 'zsrc'
        rt.write_plus(src, fx.release_app(version), signed_for=False)
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, 'w') as z:
            for n in ('rfoundry_dm32.bin', 'rfoundry_dm32.json', 'rfoundry_dm32.map'):
                z.writestr(n, (src / n).read_bytes())
        return buf.getvalue()

    def fix_entry(self, fv, zip_bytes):
        """Re-sign the cached manifest with the real zip's hash and size (as the release tool would)"""
        m = json.loads((fv.manifest_dir / rt.sm.MANIFEST_NAME).read_text(encoding='utf-8'))
        for e in m['firmware']:
            if e['version'] == fv.version:
                e['sha256'], e['size'] = rt.sm.sha256(zip_bytes), len(zip_bytes)
        rt.write_signed(fv.manifest_dir, m)
        self.w.rel.catalog = __import__('ddm32_app.releases', fromlist=['x']).cached_catalog(self.layout.releases)
        self.w.rel.reload_packages()


class DisclaimerGateTest(UiBase):
    accepted = False

    def test_no_radio_button_before_acceptance(self):
        self.disclaimer_answer = False
        self.w.first_start()
        self.assertEqual(self.disclaimer_asks, 1)
        self.w._guard()
        self.assertFalse(self.u.prep_btn.isEnabled())
        self.assertFalse(self.w.health.read_btn.isEnabled())
        self.assertFalse(self.w.live.connect_btn.isEnabled())
        self.assertFalse(self.w.lock.take('flash'))
        self.u.start_backup()
        self.w.health.read_once()
        self.w.live.start()
        self.assertEqual(fx.FakeRunner.instances, [])
        self.assertFalse(self.w.home.gate_label.isHidden())
        self.disclaimer_answer = True
        self.w.home.gate_btn.click()
        self.assertTrue(disclaimer.accepted())
        self.assertTrue(self.u.prep_btn.isEnabled())
        self.assertTrue(self.w.health.read_btn.isEnabled())
        self.assertTrue(self.w.home.gate_label.isHidden())

    def test_new_text_asks_again(self):
        paths.save_setting('disclaimer', {'version': 'older-text', 'accepted': True})
        self.assertFalse(disclaimer.accepted())
        disclaimer.accept()
        self.assertEqual(paths.load_settings()['disclaimer']['version'], disclaimer.VERSION)
        self.assertTrue(disclaimer.accepted())

    def test_dialog_needs_the_tick(self):
        d = disclaimer.make_dialog()
        self.assertFalse(d.accept_btn.isEnabled())
        self.assertEqual(d.check.text(), 'Okudum, anladım ve kabul ediyorum')
        d._accept()
        self.assertNotEqual(d.result(), d.Accepted)
        d.check.setChecked(True)
        self.assertTrue(d.accept_btn.isEnabled())
        self.assertIn('SORUMLULUK', d.text.toPlainText())
        d.toggle_lang()
        self.assertIn('DISCLAIMER', d.text.toPlainText())
        self.assertIn('AS IS', d.text.toPlainText())
        d.deleteLater()


class VersionsTest(UiBase):
    versions = ('1.0.0', '1.1.0-beta.1', '0.9.0')

    def api(self, releases):
        return json.dumps(releases).encode()

    def test_list_marks_beta_and_switches(self):
        v = self.w.versions
        self.assertEqual([r[0] for r in v.rows], ['1.1.0-beta.1', '1.0.0', '0.9.0'])
        self.assertEqual(v.table.item(0, 1).text(), 'Beta')
        self.assertEqual(v.table.item(1, 1).text(), 'Kararlı')
        self.assertIn('indirildi', v.table.item(1, 2).text())
        v.table.selectRow(2)
        v.switch_btn.click()
        self.assertIs(self.w.tabs.currentWidget(), self.u)
        self.assertEqual(self.u.wizard.package.version, '0.9.0')
        self.assertEqual(self.u.page(), PREP)

    def test_check_github(self):
        m_good = rt.manifest([rt.firmware_entry(fx.release_app('2.0.0'), version='2.0.0', release='v2.0.0')],
                             release='v2.0.0')
        good, good_sig = rt.signed(m_good)
        bad, bad_sig = rt.signed(m_good, rt.OTHER_SECRET)
        files = {}

        def release(tag, data, sig):
            files[rt.url(tag, 'ddm32-manifest.json')] = data
            files[rt.url(tag, 'ddm32-manifest.json.sig')] = sig.encode()
            return {'tag_name': tag, 'draft': False, 'prerelease': False, 'assets': [
                {'name': 'ddm32-manifest.json', 'browser_download_url': rt.url(tag, 'ddm32-manifest.json')},
                {'name': 'ddm32-manifest.json.sig', 'browser_download_url': rt.url(tag, 'ddm32-manifest.json.sig')}]}
        listing = [release('v2.0.0', good, good_sig), release('v2.1.0', bad, bad_sig),
                   {'tag_name': 'v0.1.0', 'assets': []}, {'tag_name': 'draft', 'draft': True}]
        from ddm32_app import releases
        files[releases.API_URL] = self.api(listing)
        self.w.rel.fetch = lambda url, limit=None: files[url]
        self.w.versions.check_btn.click()
        self.assertIn('Denetlendi', self.w.versions.status.text())
        self.assertEqual(self.w.rel.catalog.source, 'github')
        self.assertEqual([f.version for f in self.w.rel.catalog.firmware], ['2.0.0'])
        rejected = dict(self.w.rel.catalog.rejected)
        self.assertIn('signature', rejected['v2.1.0'])
        self.assertIn('no signed manifest', rejected['v0.1.0'])
        self.assertIn('v2.1.0', self.w.versions.rejected_label.text())
        self.assertEqual([r[0] for r in self.w.versions.rows][0], '2.0.0')
        self.assertTrue((self.layout.releases / 'manifests' / 'v2.0.0' / 'ddm32-manifest.json').is_file())
        self.assertFalse((self.layout.releases / 'manifests' / 'v2.1.0').exists())

    def test_check_failure_keeps_cache(self):
        def offline(url, limit=None):
            from ddm32_app.releases import NetError
            raise NetError('offline')
        self.w.rel.fetch = offline
        self.w.versions.check()
        self.assertIn('Denetlenemedi', self.w.versions.status.text())
        self.assertEqual(len(self.w.rel.catalog.firmware), 3)

    def test_update_banner_and_auto_check_setting(self):
        paths.save_setting('installed_sha256', rt.sm.sha256(fx.release_app('1.0.0')))
        self.w.rel.changed.emit()
        self.assertIn('Yeni sürüm var: DDM32 Plus 1.1.0-beta.1', self.w.versions.banner.text())
        self.assertEqual(self.w.home.state, 'update')
        self.w.versions.beta_box.setChecked(False)          # stable only: 1.0.0 is the newest stable
        self.w.home.refresh()
        self.assertEqual(self.w.home.state, 'current')
        self.w.versions.auto_box.setChecked(False)
        self.assertFalse(paths.load_settings()['auto_check'])


class SelfUpdateUiTest(UiBase):
    frozen = True

    def test_program_update_runs_helper(self):
        import io
        import zipfile
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, 'w') as z:
            z.writestr('DDM32Plus/DDM32Plus.exe', b'MZ new')
        zbytes = buf.getvalue()
        m = rt.manifest([], app=rt.app_entry('9.0.0', zbytes, 'v9.0.0'), release='v9.0.0')
        rt.write_signed(self.layout.releases / 'manifests' / 'v9.0.0', m)
        from ddm32_app import releases
        self.w.rel.catalog = releases.cached_catalog(self.layout.releases)
        self.w.rel.download_fn = lambda url, dest, size, progress=None: Path(dest).write_bytes(zbytes)
        self.w.rel.changed.emit()
        v = self.w.versions
        self.assertIn('Yeni program sürümü var: 9.0.0', v.app_label.text())
        self.assertTrue(v.app_btn.isEnabled())
        self.answers = [True]
        closed = []
        v.quit_for_update.disconnect()
        v.quit_for_update.connect(lambda: closed.append(1))
        v.app_btn.click()
        self.assertEqual(len(self.helper_calls), 1)
        program, args = self.helper_calls[0]
        self.assertTrue(program.endswith('DDM32Plus.exe'))
        self.assertIn('--self-update', args)
        self.assertTrue(Path(self.arg(args, '--staging')).joinpath('DDM32Plus.exe').is_file())
        self.assertEqual(closed, [1])
        self.assertIn(disclaimer.SHORT, self.asked[-1][1])

    def test_helper_result_is_shown_once(self):
        import json as _json
        (self.layout.updates / 'last-update.json').write_text(_json.dumps({'result': 'rolled_back'}))
        shown = []
        self.assertIn('eski sürüme dönüldü', self.w.poll_update_result(shown.append))
        self.assertIsNone(self.w.poll_update_result(shown.append))
        self.assertEqual(len(shown), 1)
        self.assertFalse(self.w.update_timer.isActive())

    def test_tampered_program_zip_is_refused(self):
        m = rt.manifest([], app=rt.app_entry('9.0.0', b'expected', 'v9.0.0'), release='v9.0.0')
        rt.write_signed(self.layout.releases / 'manifests' / 'v9.0.0', m)
        from ddm32_app import releases
        self.w.rel.catalog = releases.cached_catalog(self.layout.releases)
        self.w.rel.download_fn = lambda url, dest, size, progress=None: Path(dest).write_bytes(b'tampered')
        self.w.rel.changed.emit()
        self.assertFalse(self.w.versions.update_app() and True)
        self.assertEqual(self.helper_calls, [])
        self.assertIn('güncellenemedi', self.w.versions.status.text())


class RestoreUiTest(UiBase):
    def backup(self, name):
        return fx.write_progmem_backup(self.layout.backups / name, b'\xff' * 16)

    def walk(self, kind, detection, mode, tool):
        rs = self.w.restore
        self.assertTrue(rs.choose(kind))
        r = self.runner()
        self.assertEqual(r.calls[0][1][:2], ['-u', 'radiotools/radio_detect.py'])
        r.feed(fx.detect_text(detection), 0)
        r = self.runner()
        args = r.calls[0][1]
        self.assertEqual(args[:2], ['-u', f'radiotools/{tool}.py'])
        self.assertIn('--plan-only', args)
        r.feed(fx.plan_text(dict(fx.RESTORE_PLAN, mode=mode)), 0)
        self.assertTrue(rs.restore_btn.isEnabled())
        rs.restore_btn.click()
        r = self.runner()
        args = r.calls[0][1]
        self.assertNotIn('--plan-only', args)
        out = Path(self.arg(args, '--out'))
        out.mkdir(parents=True)
        (out / 'install-receipt.json').write_text(json.dumps({'writes_started': True, 'installation_verified': True,
                                                              'restart_sent': True}))
        r.feed(fx.RESTORE_OK.format(plan='{}'), 0)
        self.assertIn('Geri yüklendi', rs.result.text())
        return args

    def test_previous_version_from_classic_backup(self):
        b = self.backup('dm32-20990101')
        paths.save_setting('restore_previous_backup', str(b))
        args = self.walk('previous', fx.DETECT_CLASSIC, 'restore', 'install_plus')
        self.assertIn('--restore', args)
        q = self.asked[-1][1]
        self.assertIn(disclaimer.SHORT, q)
        self.assertIn('veri çipi', q)

    def test_stock_restore_from_own_stock_backup(self):
        b = self.backup('dm32-20990102')
        paths.save_setting('stock_backups', [str(b)])
        args = self.walk('stock', fx.DETECT_STOCK, 'restore_stock', 'install_firstboot')
        self.assertIn('--restore-stock', args)
        self.assertIn('--use-saved-backup', args)
        self.assertEqual(paths.load_settings()['installed_firmware'], 'stok')

    def test_stock_restore_refuses_non_stock_backup_and_missing_backup(self):
        rs = self.w.restore
        self.assertFalse(rs.choose('stock'))
        self.assertIn('Uygun yedek bulunamadı', rs.status.text())
        self.assertEqual(fx.FakeRunner.instances, [])
        b = self.backup('dm32-classic')
        self.assertTrue(rs.choose('stock', b))
        self.runner().feed(fx.detect_text(fx.DETECT_CLASSIC), 0)
        self.assertIn('stok yazılım değil', rs.status.text())
        self.assertEqual(len(fx.FakeRunner.instances), 1)       # no plan, no restore
        self.assertFalse(rs.restore_btn.isEnabled())

    def test_refused_confirmation_writes_nothing(self):
        b = self.backup('dm32-x')
        paths.save_setting('restore_previous_backup', str(b))
        rs = self.w.restore
        rs.choose('previous')
        self.runner().feed(fx.detect_text(fx.DETECT_CLASSIC), 0)
        self.runner().feed(fx.plan_text(dict(fx.RESTORE_PLAN, mode='restore')), 0)
        self.answers = [False]
        rs.restore_btn.click()
        self.assertEqual(len(fx.FakeRunner.instances), 2)
        self.assertIn('Onaylanmadı', rs.result.text())


class PortChoiceTest(UiBase):
    ports = staticmethod(lambda: fx.fake_ports() + [PortInfo('COM9', 'USB-SERIAL CH340', 0x1A86, 0x7523)])

    def test_choice_shown_with_two_cables(self):
        self.assertFalse(self.u.port_picker.combo.isHidden())
        self.assertEqual(self.u.port_picker.combo.count(), 2)


class NoCableTest(UiBase):
    ports = staticmethod(lambda: fx.fake_ports()[:2])

    def test_no_cable(self):
        self.assertIsNone(self.w.port_state.port)
        self.assertIn('Kablo bulunamadı', self.u.port_picker.label.text())
        self.assertFalse(self.u.prep_btn.isEnabled())


class NoReleasesTest(UiBase):
    versions = ()

    def test_nothing_to_install(self):
        self.assertIsNone(self.u.wizard.package)
        self.assertFalse(self.u.prep_btn.isEnabled())
        self.assertIn('imzalı bir sürüm yok', self.u.version_label.text())
        self.assertEqual(self.w.home.state, 'none')


class PagesTest(UiBase):
    def test_home_and_identity(self):
        self.assertEqual(self.w.windowTitle()[:10], 'DDM32 Plus')
        titles = [self.w.tabs.tabText(i) for i in range(self.w.tabs.count())]
        self.assertEqual(titles, ['Ana sayfa', 'Telsizi güncelle', 'Sürümler', 'Geri dön', 'Telsizin durumu',
                                  'Canlı panel', 'Kanallarım', 'Verileri güncelle'])
        self.assertEqual(self.w.home.state, 'unknown')
        self.w.home.buttons['channels'].click()
        self.assertIs(self.w.tabs.currentWidget(), self.w.codeplug)
        self.w.home.buttons['restore'].click()
        self.assertIs(self.w.tabs.currentWidget(), self.w.restore)
        from ddm32_app.widgets import about_html
        html = about_html()
        for part in ('DDM32 Plus · d0ukan · TA2DOU', 'github.com/d0ukan/DDM32-Plus', 'RadioFoundry',
                     'Open25-DM32'):
            self.assertIn(part, html)

    def test_frozen_hides_data_page(self):
        lay = paths.Layout.frozen_app(bundle=self.top, data=self.data, runner_exe='x.exe').ensure()
        w = MainWindow(layout=lay, runner_factory=fx.FakeRunner, port_lister=self.ports, run_async=False,
                       ask_disclaimer=lambda: True, auto_check=False)
        self.assertNotIn('data', w.pages)
        self.assertNotIn('data', w.home.buttons)
        self.assertIsNone(w.data)
        w.deleteLater()

    def test_help(self):
        self.w.help()
        self.assertEqual(len(self.helps), 1)
        from ddm32_app.widgets import HELP_HTML
        for topic in ('yakalanamadı', 'Kablo bulunamadı', 'Yedek eski', 'imzalı', 'Stok yazılımdan ilk kurulum',
                      'yarıda kaldı', 'LOCALAPPDATA'):
            self.assertIn(topic, HELP_HTML)

    def test_health(self):
        h = self.w.health
        self.assertFalse(h.auto.isChecked())
        h.read_btn.click()
        r = self.runner()
        self.assertEqual(r.calls[0][1], ['-u', 'radiotools/plus_txdiag.py', '--port', 'COM7', '--health'])
        r.feed(fx.HEALTH, 0)
        self.assertEqual(h.verdict, 'amber')
        self.assertEqual(h.verdict_label.text(), 'Dikkat: bellek az')
        self.assertIn('Hata kaydı yok', h.summary.text())
        self.assertEqual(h.table.rowCount(), 4)

    def test_health_verdicts(self):
        s = parsers.health_summary(parsers.parse_health(fx.HEALTH.replace('tasks.free.p25_lab=150',
                                                                           'tasks.free.p25_lab=900')))
        self.assertEqual(verdict(s), 'green')
        self.assertEqual(plain_lines(s)[0], "Telsiz 1 dk'dır açık.")
        s['crash_valid'] = True
        self.assertEqual(verdict(s), 'red')

    def test_channels_autoload_simple_columns(self):
        fx.write_backup(self.layout.backups / 'dm32-20990101', fx.sample_image())
        fx.write_backup(self.layout.backups / 'dm32-20990102', fx.ImageBuilder().bytes(), verified=False)
        c = self.w.codeplug
        self.assertTrue(c.reload_backups())
        self.assertEqual(c.models['channels'].rowCount(), 9)
        from ddm32_app import codeplug as cp
        keys = [k for k, _ in cp.CHANNEL_COLUMNS]
        row = {k: c.models['channels'].item(3, i).text() for i, k in enumerate(keys)}
        self.assertEqual(row['summary'], 'NAC $293 = 659 · TG 101')

    def test_data_update_and_tle_wsl_only(self):
        d = self.w.data
        p = d.paths
        self.assertTrue(str(p['tle']).startswith(str(self.repo)))
        p['sat_freq'].parent.mkdir(parents=True)
        p['sat_freq'].write_text('25544 ISS 437.800 145.990 67.0 0\n')
        p['tle'].write_text('# old\n' + fx.tle_set('ISS (ZARYA)', 25544, '26300.10000000'))
        answers = {dataupdate.USER_CSV_URL: b'RADIO_ID,CALLSIGN,FIRST_NAME,LAST_NAME,CITY,STATE,COUNTRY\n'
                                            b'1,A,,,,,Turkey\n2,B,,,,,Germany\n',
                   dataupdate.RPTRS_URL: b'{"rptrs": [{"country": "Turkiye"}]}',
                   dataupdate.AMATEUR_URL: fx.tle_set('ISS (ZARYA)', 25544, '26322.33455779').encode()}
        d.fetch = answers.__getitem__
        tle_before = p['tle'].read_text()
        d.all_btn.click()
        self.assertIn('ID: 1 Türk kullanıcı', d.summary.text())
        self.assertEqual(p['tle'].read_text(), tle_before)
        self.assertTrue(d.tle_write_btn.isEnabled())
        self.assertEqual(fx.FakeRunner.instances, [])
        d.tle_write_btn.click()
        program, args, _ = self.runner().calls[0]
        self.assertEqual(program, 'wsl.exe')
        self.assertIn('cd ~/ddm32-plus', args[-1])

    def test_build_plain_result(self):
        d = self.w.data
        d.build_btn.click()
        r = self.runner()
        program, args, _ = r.calls[0]
        self.assertEqual(program, 'wsl.exe')
        self.assertIn(dataupdate.wsl_path(self.repo), args[-1])
        self.assertIn('--public', args[-1])
        r.feed(fx.BUILD_OUT, 0)
        self.assertIn('imzalanıp yayımlanmalı', d.build_msg.text())

    def test_geometry_saved(self):
        self.w.resize(900, 700)
        self.w.close()
        self.assertIn('window_geometry', paths.load_settings())


if __name__ == '__main__':
    unittest.main()
