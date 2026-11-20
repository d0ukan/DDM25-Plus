"""Geri dön: put the radio back from the user's OWN backup.

- "Önceki sürüme dön": the backup taken right before the last install (whatever the radio ran then: stock, Classic or
  an earlier DDM25 Plus).
- "Stok yazılıma dön": a backup of this radio while it still ran the stock firmware (kept when a backup is detected as
  stock; another folder may be chosen). Stock firmware is never shipped or downloaded: the backup is the only source.
The tool follows the backup's layout (radio_detect.py, no port): stock -> install_firstboot.py --restore-stock
--use-saved-backup (the restore proven on hardware), Classic layout -> install_plus.py --restore. Both rewrite every
application and codec slot sector from the backup and read the application back before the restart. The bootloader
and the data chip (calibration, channels, settings) are never written: channels saved by DDM25 Plus stay as they are.
"""
import json
from pathlib import Path

from PyQt5.QtCore import Qt, pyqtSignal
from PyQt5.QtWidgets import QFileDialog, QHBoxLayout, QLabel, QProgressBar, QPushButton, QVBoxLayout, QWidget

from . import disclaimer, parsers, paths
from .portstate import PortPicker
from .runner import ProcessRunner
from .update_tab import GREEN, RED, AMBER, _esc, colored, default_confirm, detection_text, verified_backup
from .widgets import Details, LogPane, RadioOnBanner, big_button, bigger, plain_label
from .wizard import OK, RestoreFlow

RESTORE_WAIT = 600


class RestoreTab(QWidget):
    restored = pyqtSignal(str)       # 'previous' or 'stock'

    def __init__(self, layout, lock, port_state, runner_factory=ProcessRunner, confirm=None, parent=None):
        super().__init__(parent)
        self.layout_ = layout
        self.root = layout.root
        self.lock = lock
        self.port_state = port_state
        self.runner_factory = runner_factory
        self.confirm = confirm or (lambda title, text, yes='Evet, geri yükle': default_confirm(self, title, text, yes))
        self.flow = RestoreFlow()
        self.runner = None
        self.step = None
        self.lines = []
        self.out_dir = None
        self.log = LogPane()
        self._build()
        port_state.changed.connect(self._port_changed)
        self._port_changed(port_state.port)
        self.refresh()

    def _build(self):
        outer = QVBoxLayout(self)
        t = QLabel('Geri dön')
        bigger(t, 8, True)
        outer.addWidget(t)
        outer.addWidget(plain_label(
            'Telsizi <b>kendi yedeğinizden</b> geri yükler. Stok yazılım bu programla gelmez ve indirilmez: '
            'geri dönüş yalnızca bu telsizin yedeğiyle yapılır.<br>'
            'Bootloader ve veri çipi (kalibrasyon, kanallar, ayarlar) yazılmaz.'))
        self.port_picker = PortPicker(self.port_state)
        outer.addWidget(self.port_picker)
        row = QHBoxLayout()
        self.previous_btn = big_button('Önceki sürüme dön', 'SP_ArrowBack',
                                       'Son yüklemeden hemen önce alınan yedeği geri yükler.')
        self.previous_btn.clicked.connect(lambda: self.choose('previous'))
        self.stock_btn = big_button('Stok yazılıma dön', 'SP_DialogResetButton',
                                    'Telsiz stok yazılımdayken alınan kendi yedeğinizi geri yükler.')
        self.stock_btn.clicked.connect(lambda: self.choose('stock'))
        row.addWidget(self.previous_btn)
        row.addWidget(self.stock_btn)
        outer.addLayout(row)
        self.other_btn = QPushButton('Başka bir yedek klasörü seç...')
        self.other_btn.setToolTip('Bu telsizin doğrulanmış başka bir yedeği (sadece kendi telsizinizin yedeği).')
        self.other_btn.clicked.connect(self.choose_folder)
        outer.addWidget(self.other_btn, 0, Qt.AlignLeft)
        self.status = plain_label()
        outer.addWidget(self.status)
        self.restore_btn = big_button('Geri yükle', 'SP_DialogApplyButton', 'Seçilen yedeği telsize yazar (onay sorulur).')
        self.restore_btn.clicked.connect(self.start_restore)
        outer.addWidget(self.restore_btn)
        self.banner = RadioOnBanner()
        outer.addWidget(self.banner)
        self.progress = QProgressBar()
        self.progress.setRange(0, 100)
        outer.addWidget(self.progress)
        self.result = plain_label()
        outer.addWidget(self.result)
        self.details = Details()
        self.detail_label = QLabel('-')
        self.detail_label.setWordWrap(True)
        self.detail_label.setTextInteractionFlags(Qt.TextSelectableByMouse)
        self.details.add(self.detail_label)
        self.details.add(self.log, 1)
        outer.addStretch(1)
        outer.addWidget(self.details, 1)

    # --- choosing a backup (files only) -------------------------------------------------------------------------
    def _port_changed(self, port):
        if self.flow.running != 'restore':
            self.flow.port = port or None
        self.refresh()

    def default_backup(self, kind):
        s = paths.load_settings()
        if kind == 'previous':
            cands = [s.get('restore_previous_backup')]
        else:
            cands = list(s.get('stock_backups', []))
        return next((Path(c) for c in cands if c and verified_backup(c)), None)

    def choose(self, kind, folder=None):
        if self.flow.running:
            return False
        folder = folder or self.default_backup(kind)
        if folder is None:
            self.status.setText(colored('Uygun yedek bulunamadı.', RED) + '<br>' +
                                ('Bu programla yapılmış bir yükleme yok.' if kind == 'previous' else
                                 'Stok yazılımdayken alınmış bir yedek kayıtlı değil. "Başka bir yedek klasörü '
                                 'seç..." ile kendi yedeğinizi gösterin.'))
            return False
        self.flow.choose(kind, Path(folder))
        self.result.setText('')
        self.progress.setValue(0)
        self.status.setText(f'Seçilen yedek: <b>{_esc(Path(folder).name)}</b>. İnceleniyor (port açılmaz)...')
        self.start_detect()
        return True

    def choose_folder(self):
        d = QFileDialog.getExistingDirectory(self, 'Bu telsizin doğrulanmış yedeği', str(self.layout_.backups))
        if d:
            if not verified_backup(d):
                self.status.setText(colored('Bu klasör doğrulanmış bir yedek değil.', RED))
                return
            self.choose('stock' if self.flow.kind == 'stock' else 'previous', Path(d))

    def start_detect(self):
        if not self.flow.can_detect():
            return
        self.flow.start_detect()
        self._run('detect', 'radio_detect', ['--backup', self.layout_.rel(self.flow.backup_dir)], None)

    def start_plan(self):
        if not self.flow.can_plan():
            return
        tool, extra, _ = self.flow.tool
        self.flow.start_plan()
        self.out_dir = paths.unique_folder(self.layout_.backups, f'restore-{paths.today()}')
        args = ['--port', self.flow.port or 'COM1', '--backup', self.layout_.rel(self.flow.backup_dir),
                '--out', self.layout_.rel(self.out_dir)] + extra + ['--plan-only']
        self._run('plan', tool, args, None)

    # --- restore (radio step) -----------------------------------------------------------------------------------
    def question(self):
        f = self.flow
        p = f.plan_data
        what = 'stok yazılım' if f.detection.get('layout') == 'stock' else detection_text(f.detection)
        return '\n'.join([
            'Telsiz bu yedekten geri yüklensin mi?', '',
            f'Yedek: {f.backup_dir}', f'Yedekteki yazılım: {what}',
            f'Değişecek: uygulama ve codec bölümü, {p["changed_sectors"]} sektör ({p["first"]} - {p["last"]})',
            f'Port: {f.port}', '',
            'Yalnızca bu telsizin kendi yedeğini kullanın.',
            'Bootloader ve veri çipi (kalibrasyon, kanallar, ayarlar) yazılmaz: DDM25 Plus\'ta kaydedilen kanallar '
            'olduğu gibi kalır; stok yazılım bunları farklı gösterebilir (gerekirse CPS ile yeniden yazın).',
            'Hata olursa kendiliğinden tekrar denenmez.', '', '⚠ ' + disclaimer.SHORT])

    def start_restore(self):
        if not self.flow.can_restore() or not self.lock.allowed():
            return
        if not self.confirm('Geri yüklemeyi onaylayın', self.question()):
            self.result.setText('Onaylanmadı; hiçbir şey yapılmadı.')
            return
        if not self.lock.take('flash'):
            return
        tool, extra, _ = self.flow.tool
        self.flow.start_restore()
        self.progress.setValue(0)
        self.result.setText('Telsizi <b>kapatın</b> (açıksa). Sarı yazıyı görünce açın.')
        self.out_dir = paths.unique_folder(self.layout_.backups, f'restore-{paths.today()}')
        args = ['--port', self.flow.port, '--backup', self.layout_.rel(self.flow.backup_dir),
                '--out', self.layout_.rel(self.out_dir)] + extra + ['--wait', str(RESTORE_WAIT)]
        self._run('restore', tool, args, paths.install_log_path(self.out_dir))

    # --- running a tool -----------------------------------------------------------------------------------------
    def _run(self, step, tool, args, log_path):
        self.lines = []
        self.step = step
        self.log.clear()
        program, argv, shown = self.layout_.tool_command(tool, args)
        if log_path is not None:
            self.log.open_file(log_path, ' '.join([program] + argv))
        self.log.add('$ ' + shown)
        if step == 'restore':
            self.port_state.set_locked(True)
        self.runner = self.runner_factory(self)
        self.runner.line.connect(self._line)
        self.runner.finished.connect(self._finished)
        self.refresh()
        self.runner.start(program, argv, cwd=self.root)

    def _line(self, line):
        self.lines.append(line)
        self.log.add(line)
        if self.step != 'restore':
            return
        wait = parsers.ready_wait(line)
        if wait:
            self.banner.start(wait[1])
            return
        s = line.strip()
        if parsers.RE_BOOT_ROM.match(s) or s.startswith(('INSTALLING', 'RESTORING')):
            self.banner.stop()
        pct = parsers.install_progress(line, parsers.RESTORE_PHASES)
        if pct is not None:
            self.banner.stop()
            self.progress.setValue(pct)
            self.result.setText(f'Geri yükleniyor... %{pct}. Kabloyu çekmeyin.')

    def _finished(self, code):
        self.banner.stop()
        step = self.step
        self.log.close_file()
        self.runner = None
        if step == 'restore':
            self.lock.release('flash')
            self.port_state.set_locked(False)
        text = '\n'.join(self.lines)
        if step == 'detect':
            try:
                d = parsers.parse_detect(text) if code == 0 else None
                err = '' if d else (parsers.error_text(self.lines) or f'exit code {code}')
            except ValueError as exc:
                d, err = None, str(exc)
            self.flow.detect_finished(d, err)
            if d is not None and self.flow.tool is not None:
                self.status.setText(f'Yedek: <b>{_esc(self.flow.backup_dir.name)}</b><br>' +
                                    colored(_esc(detection_text(d)).replace('Telsizde', 'Yedekte'), GREEN))
                self.start_plan()
            else:
                self.status.setText(colored(_esc(self.flow.message), RED))
        elif step == 'plan':
            try:
                plan = parsers.parse_plan(text) if code == 0 else None
                err = '' if plan else (parsers.error_text(self.lines) or f'exit code {code}')
            except ValueError as exc:
                plan, err = None, str(exc)
            mode = self.flow.tool[2] if self.flow.tool else None
            safe = plan is not None and parsers.plan_safe(plan, mode)
            self.flow.plan_finished(plan, err, safe)
            if plan and safe:
                self.status.setText(self.status.text() + '<br>' + colored(
                    f'Plan hazır: {plan["changed_sectors"]} sektör yedekten yazılır; bootloader ve veri çipi '
                    'yazılmaz.', GREEN, False))
                self.detail_label.setText(json.dumps(plan, indent=2))
            else:
                self.status.setText(colored('Plan yapılamadı: ', RED) + _esc(err or 'güvenli olmayan plan'))
        elif step == 'restore':
            receipt = None
            try:
                receipt = json.loads((self.out_dir / 'install-receipt.json').read_text())
            except (OSError, ValueError, TypeError):
                pass
            r = parsers.InstallResult(self.lines, code, receipt)
            self.flow.restore_finished(r)
            if r.passed:
                self.progress.setValue(100)
                kind = self.flow.kind
                self.result.setText(colored('Geri yüklendi ✓', GREEN) + ' Telsizin açılış ekranını kontrol edin.')
                if self.flow.detection.get('layout') == 'stock':
                    paths.save_setting('installed_sha256', '')
                    paths.save_setting('installed_version', '')
                    paths.save_setting('installed_firmware', 'stok')
                else:
                    paths.save_setting('installed_sha256', '')
                    paths.save_setting('installed_version', '')
                    paths.save_setting('installed_firmware', 'yedekten geri yüklendi')
                self.restored.emit(kind)
            else:
                color = RED if r.writes_started else AMBER
                self.result.setText(colored(_esc(self.flow.message), color) + '<br>' + _esc(r.error))
            self.detail_label.setText(f'Klasör: {self.out_dir}\nSonuç: {"PASS" if r.passed else r.error}\n' +
                                      '\n'.join(f'{k}: {v}' for k, v in r.flags().items()))
        self.refresh()

    def refresh(self):
        f = self.flow
        idle = f.running is None
        self.previous_btn.setEnabled(idle)
        self.stock_btn.setEnabled(idle)
        self.other_btn.setEnabled(idle)
        self.restore_btn.setEnabled(f.can_restore() and self.lock.free_for('flash'))
        self.restore_btn.setVisible(f.plan == OK or f.running == 'restore')

    def busy(self):
        return self.runner is not None and self.step == 'restore'
