"""Telsizi güncelle: a guided flow, one screen at a time (Hazırlık, Yedek, Kontrol, Yükle, Bitti).

Hazırlık: the cable and the version (a signed release; downloaded and hash-checked on the "İndir" click).
Yedek: the verified two-pass backup (radio step, its own button).
Kontrol: what the radio runs (radio_detect.py) and the write plan (install_plus.py --plan-only), both from the backup
only, no port; they follow a verified backup by themselves. Stock firmware gets the first-install plan (the radio's
own codec prepared in the slot), the Classic layout the application-only plan.
Yükle: one confirmation with the version, the plan, the compatibility warnings and the disclaimer's short warning (a
second confirmation for a risky downgrade), then the install (radio step). Never retried automatically.
Technical details (hashes, sector ranges, folders, the raw tool output) sit behind each page's 'Ayrıntılar'.
"""
import json
from pathlib import Path

from PyQt5.QtCore import Qt, pyqtSignal
from PyQt5.QtWidgets import (QComboBox, QFileDialog, QFormLayout, QHBoxLayout, QLabel, QLineEdit, QMessageBox,
                             QProgressBar, QPushButton, QStackedWidget, QVBoxLayout, QWidget)

from . import disclaimer, parsers, paths, releases
from .portstate import PortPicker
from .runner import ProcessRunner
from .widgets import Details, LogPane, RadioOnBanner, big_button, bigger, icon, plain_label
from .wizard import FAILED, OK, Wizard

BACKUP_WAIT = 120
INSTALL_WAIT = 600
PREP, BACKUP, CHECK, INSTALL, DONE = range(5)
STEP_NAMES = ('Hazırlık', 'Yedek', 'Kontrol', 'Yükle', 'Bitti')
GREEN, RED, AMBER = '#2e7d32', '#c62828', '#8a6d00'
LEVEL_COLORS = {releases.INFO: '#1f4e79', releases.WARN: AMBER, releases.DANGER: RED}


def default_confirm(parent, title, text, yes='Evet, yükle'):
    box = QMessageBox(QMessageBox.Warning, title, text, QMessageBox.NoButton, parent)
    ok = box.addButton(yes, QMessageBox.AcceptRole)
    no = box.addButton('Vazgeç', QMessageBox.RejectRole)
    box.setDefaultButton(no)
    box.exec_()
    return box.clickedButton() is ok


def colored(text, color, bold=True):
    return f'<span style="color:{color};{"font-weight:bold;" if bold else ""}">{text}</span>'


def detection_text(d):
    """One plain Turkish sentence for radio_detect's JSON"""
    if not d:
        return 'Telsizdeki yazılım bilinmiyor.'
    if d.get('layout') == 'stock':
        v = d.get('stock_version') or '?'
        tested = '' if d.get('stock_tested') else ' (DDM25 Plus bu stok sürümde denenmedi)'
        return f'Telsizde stok yazılım var: .{v}{tested}.'
    if d.get('layout') == 'classic':
        name = {'ddm25': 'DDM25 Plus', 'classic': 'Open25-DM32 Classic'}.get(d.get('firmware'),
                                                                         'Classic düzeninde bir yazılım')
        return f'Telsizde {name} var (codec bölümü hazır).'
    return 'Desteklenmeyen yazılım: ' + (d.get('reason') or 'tanınmadı')


class Page(QWidget):
    def __init__(self, title):
        super().__init__()
        self.lay = QVBoxLayout(self)
        t = QLabel(title)
        bigger(t, 8, True)
        self.lay.addWidget(t)
        self.details = Details()

    def finish(self):
        self.lay.addStretch(1)
        self.lay.addWidget(self.details, 1)


class UpdateTab(QWidget):
    installed = pyqtSignal(str)      # version that just passed
    go_home = pyqtSignal()
    go_health = pyqtSignal()
    go_restore = pyqtSignal()

    def __init__(self, layout, lock, port_state, rel, runner_factory=ProcessRunner, confirm=None, parent=None):
        super().__init__(parent)
        self.layout_ = layout
        self.root = layout.root
        self.lock = lock
        self.port_state = port_state
        self.rel = rel
        self.runner_factory = runner_factory
        self.confirm = confirm or (lambda title, text, yes='Evet, yükle': default_confirm(self, title, text, yes))
        self.wizard = Wizard()
        self.runner = None
        self.step = None
        self.lines = []
        self.install_dir = None
        self.choices = []
        self.warnings = []
        self.download_error = None       # (version, text) of the last failed download
        self.log = LogPane()
        self._build()
        port_state.changed.connect(self._port_changed)
        rel.changed.connect(self.reload_packages)
        rel.downloaded.connect(self._downloaded)
        rel.progress.connect(self._download_progress)
        self._port_changed(port_state.port)
        self.reload_packages()
        self.show_page(PREP)

    # --- layout -------------------------------------------------------------------------------------------------
    def _build(self):
        outer = QVBoxLayout(self)
        self.header = QLabel()
        bigger(self.header, 3)
        self.header.setTextFormat(Qt.RichText)
        outer.addWidget(self.header)
        self.stack = QStackedWidget()
        outer.addWidget(self.stack, 1)
        nav = QHBoxLayout()
        self.back_btn = QPushButton('Geri')
        self.back_btn.setIcon(icon(self, 'SP_ArrowBack'))
        self.back_btn.setToolTip('Bir önceki adıma dön (telsize bir şey yapmaz).')
        self.back_btn.clicked.connect(self.go_back)
        nav.addWidget(self.back_btn)
        nav.addStretch(1)
        outer.addLayout(nav)

        # 1. Hazırlık
        p = Page('1. Hazırlık')
        p.lay.addWidget(plain_label('1. Programlama kablosunu telsize ve bilgisayara takın.<br>'
                                    '2. Telsizi <b>kapatın</b>.'))
        self.port_picker = PortPicker(self.port_state)
        bigger(self.port_picker.label, 2)
        p.lay.addWidget(self.port_picker)
        row = QHBoxLayout()
        row.addWidget(plain_label('Sürüm:', 2, False))
        self.package_combo = QComboBox()
        bigger(self.package_combo, 2)
        self.package_combo.setToolTip('İmzalı DDM25 Plus sürümleri (beta ve kararlı).')
        self.package_combo.currentIndexChanged.connect(self._package_changed)
        row.addWidget(self.package_combo, 1)
        self.download_btn = QPushButton('İndir ve doğrula')
        self.download_btn.setIcon(icon(self, 'SP_ArrowDown'))
        self.download_btn.setToolTip('Sürümü GitHub\'dan indirir; imza ve SHA-256 denetlenir (telsize bir şey '
                                     'yapılmaz).')
        self.download_btn.clicked.connect(self.download_selected)
        row.addWidget(self.download_btn)
        p.lay.addLayout(row)
        self.download_progress = QProgressBar()
        self.download_progress.setRange(0, 100)
        self.download_progress.hide()
        p.lay.addWidget(self.download_progress)
        self.version_label = plain_label()
        p.lay.addWidget(self.version_label)
        self.prep_btn = big_button('Devam', 'SP_ArrowForward', 'Yedek adımına geç (henüz telsize bir şey yapılmaz).')
        self.prep_btn.clicked.connect(lambda: self.show_page(BACKUP))
        p.lay.addWidget(self.prep_btn)
        f = QWidget()
        form = QFormLayout(f)
        self.sha_label = QLabel('-')
        self.sha_label.setTextInteractionFlags(Qt.TextSelectableByMouse)
        self.review_label = QLabel('-')
        self.review_label.setWordWrap(True)
        form.addRow('Uygulama SHA-256:', self.sha_label)
        form.addRow('İmza:', self.review_label)
        p.details.add(f)
        p.finish()
        self.stack.addWidget(p)

        # 2. Yedek
        p = Page('2. Yedek')
        p.lay.addWidget(plain_label('Telsizin tam yedeği alınır (sadece okuma, telsize hiçbir şey yazılmaz).<br>'
                                    'Telsiz <b>kapalıyken</b> butona basın. Sarı yazıyı görünce telsizi açın (açıksa önce kapatıp açın).'))
        self.backup_btn = big_button('Yedek al', 'SP_DialogSaveButton', 'Telsizi okuyup yedeğini bilgisayara kaydeder.')
        self.backup_btn.clicked.connect(self.start_backup)
        p.lay.addWidget(self.backup_btn)
        self.backup_banner = RadioOnBanner()
        p.lay.addWidget(self.backup_banner)
        self.backup_progress = QProgressBar()
        self.backup_progress.setRange(0, 100)
        p.lay.addWidget(self.backup_progress)
        self.backup_status = plain_label()
        p.lay.addWidget(self.backup_status)
        self.backup_cancel = QPushButton('Beklemeyi iptal et')
        self.backup_cancel.setToolTip('Sadece telsiz beklenirken: araç henüz telsizle konuşmadı.')
        self.backup_cancel.clicked.connect(self.cancel_wait)
        self.backup_cancel.hide()
        p.lay.addWidget(self.backup_cancel, 0, Qt.AlignLeft)
        d = QWidget()
        form = QFormLayout(d)
        self.fw_text = QLineEdit()
        self.fw_text.setText(paths.load_settings().get('installed_firmware', ''))
        self.fw_text.setToolTip('Yedeğin notuna yazılır (telsizde şu an hangi firmware var, bildiğiniz kadarıyla).')
        form.addRow('Kurulu firmware notu:', self.fw_text)
        self.backup_folder = QLabel('-')
        self.backup_folder.setTextInteractionFlags(Qt.TextSelectableByMouse)
        form.addRow('Yedek klasörü:', self.backup_folder)
        self.existing_btn = QPushButton('Mevcut yedeği seç...')
        self.existing_btn.setToolTip('Bu telsizin daha önce doğrulanmış bir yedeği (telsiz o yedekten beri '
                                     'değişmediyse).')
        self.existing_btn.clicked.connect(self.choose_existing_backup)
        form.addRow('', self.existing_btn)
        p.details.add(d)
        p.finish()
        self.stack.addWidget(p)

        # 3. Kontrol
        p = Page('3. Kontrol')
        self.detect_status = plain_label()
        p.lay.addWidget(self.detect_status)
        self.check_status = plain_label()
        p.lay.addWidget(self.check_status)
        self.replan_btn = QPushButton('Tekrar kontrol et')
        self.replan_btn.setIcon(icon(self, 'SP_BrowserReload'))
        self.replan_btn.setToolTip('Yazılacak bölümü yeniden hesaplar (port açılmaz).')
        self.replan_btn.clicked.connect(self.start_detect)
        p.lay.addWidget(self.replan_btn, 0, Qt.AlignLeft)
        self.check_btn = big_button('Devam', 'SP_ArrowForward', 'Yükleme adımına geç (henüz yazma yok).')
        self.check_btn.clicked.connect(lambda: self.show_page(INSTALL))
        p.lay.addWidget(self.check_btn)
        self.plan_details = QLabel('-')
        self.plan_details.setWordWrap(True)
        self.plan_details.setTextInteractionFlags(Qt.TextSelectableByMouse)
        p.details.add(self.plan_details)
        p.finish()
        self.stack.addWidget(p)

        # 4. Yükle
        p = Page('4. Yükle')
        p.lay.addWidget(plain_label('Yükleme birkaç dakika sürer. Bitene kadar kabloyu çekmeyin, telsizi '
                                    'kapatmayın.<br>Butona basınca bir onay sorulur, sonra telsizi kapatıp açmanız '
                                    'istenir.'))
        self.warn_label = plain_label()
        p.lay.addWidget(self.warn_label)
        self.install_btn = big_button('Yükle', 'SP_ArrowUp', 'DDM25 Plus\'ı telsize yükler (onay sorulur).')
        self.install_btn.clicked.connect(self.start_install)
        p.lay.addWidget(self.install_btn)
        self.install_banner = RadioOnBanner()
        p.lay.addWidget(self.install_banner)
        self.install_progress = QProgressBar()
        self.install_progress.setRange(0, 100)
        p.lay.addWidget(self.install_progress)
        self.install_status = plain_label()
        p.lay.addWidget(self.install_status)
        row = QHBoxLayout()
        self.install_cancel = QPushButton('Beklemeyi iptal et')
        self.install_cancel.setToolTip('Sadece telsiz beklenirken: henüz hiçbir şey yazılmadı.')
        self.install_cancel.clicked.connect(self.cancel_wait)
        self.install_cancel.hide()
        self.to_check_btn = QPushButton('Kontrol adımına dön')
        self.to_check_btn.setToolTip('Hiçbir şey yazılmadı: kontrolü ve yüklemeyi elle yeniden yapabilirsiniz.')
        self.to_check_btn.clicked.connect(lambda: self.show_page(CHECK))
        self.to_restore_btn = QPushButton('Geri dön sayfası')
        self.to_restore_btn.setToolTip('Yüklemeden önce alınan yedekle telsizi geri yükleyin.')
        self.to_restore_btn.clicked.connect(self.go_restore.emit)
        for b in (self.install_cancel, self.to_check_btn, self.to_restore_btn):
            row.addWidget(b)
            b.hide()
        row.addStretch(1)
        p.lay.addLayout(row)
        self.install_details = QLabel('-')
        self.install_details.setWordWrap(True)
        self.install_details.setTextInteractionFlags(Qt.TextSelectableByMouse)
        p.details.add(self.install_details)
        p.finish()
        self.stack.addWidget(p)

        # 5. Bitti
        p = Page('5. Bitti')
        done = QLabel('Yüklendi ✓')
        bigger(done, 16, True)
        done.setStyleSheet(f'color: {GREEN};')
        p.lay.addWidget(done)
        p.lay.addWidget(plain_label('DDM25 Plus yüklendi ve doğrulandı (VERIFIED, PASS).<br><br>'
                                    'Telsizde kontrol edin:<br>'
                                    '• Açılış ekranı, ardından ana ekran geliyor mu?<br>'
                                    '• Stoktan ilk kurulumda telsiz kanallar için "Yeni başlat" (Start new) '
                                    'sorabilir: önce yedeğinizin durduğundan emin olun.<br>'
                                    '• Bir kanal seçip dinleyin. İsterseniz "Telsizin durumu" sayfasından sağlık '
                                    'okuyun.'))
        row = QHBoxLayout()
        home = big_button('Ana sayfa', 'SP_DirHomeIcon')
        home.clicked.connect(self.go_home.emit)
        health = big_button('Telsizin durumu', 'SP_MessageBoxInformation')
        health.clicked.connect(self.go_health.emit)
        row.addWidget(home)
        row.addWidget(health)
        p.lay.addLayout(row)
        self.done_details = QLabel('-')
        self.done_details.setWordWrap(True)
        self.done_details.setTextInteractionFlags(Qt.TextSelectableByMouse)
        p.details.add(self.done_details)
        p.finish()
        self.stack.addWidget(p)

    # --- navigation ---------------------------------------------------------------------------------------------
    def page(self):
        return self.stack.currentIndex()

    def show_page(self, index):
        if self.wizard.running and index != self.page():
            return
        self.stack.setCurrentIndex(index)
        page = self.stack.currentWidget()
        page.details.add(self.log, 1)
        self.header.setText('  ›  '.join(f'<b><u>{i + 1}. {n}</u></b>' if i == index else f'{i + 1}. {n}'
                                          for i, n in enumerate(STEP_NAMES)))
        if index == INSTALL:
            self._show_warnings()
        self.refresh()

    def go_back(self):
        if self.can_go_back():
            self.show_page(self.page() - 1)

    def can_go_back(self):
        w = self.wizard
        return (w.running is None and self.page() in (BACKUP, CHECK, INSTALL) and not w.locked and
                w.install != OK)

    # --- inputs -------------------------------------------------------------------------------------------------
    def _port_changed(self, port):
        if self.wizard.running in ('backup', 'install'):
            return
        self.wizard.set_port(port)
        self.refresh()

    def reload_packages(self):
        if self.wizard.running:
            return
        current = self.selected_version()
        self.choices = self.rel.versions()
        self.package_combo.blockSignals(True)
        self.package_combo.clear()
        installed = self.rel.installed()
        for version, fv, pkg in self.choices:
            mark = 'beta' if fv and fv.is_beta else 'kararlı'
            state = 'indirildi' if pkg else 'indirilmedi'
            here = ', telsizde' if installed and fv and installed.version == version else ''
            self.package_combo.addItem(f'DDM25 Plus {version}  ({mark}, {state}{here})', version)
        self.package_combo.blockSignals(False)
        index = self.package_combo.findData(current) if current else -1
        if index < 0:
            newest = self.rel.newest()
            index = self.package_combo.findData(newest.version) if newest else (0 if self.choices else -1)
        self.package_combo.setCurrentIndex(index)
        self._package_changed(index)

    def selected_version(self):
        i = self.package_combo.currentIndex() if hasattr(self, 'package_combo') else -1
        return self.package_combo.itemData(i) if i >= 0 else None

    def select_version(self, version):
        """'Bu sürüme geç' from the version list: choose it here and start at Hazırlık"""
        if self.wizard.running:
            return False
        self.reload_packages()
        i = self.package_combo.findData(version)
        if i < 0:
            return False
        self.package_combo.setCurrentIndex(i)
        self._package_changed(i)
        self.show_page(PREP)
        return True

    def newest_package(self):
        newest = self.rel.newest()
        return self.rel.package(newest.version) if newest else None

    def _choice(self, index):
        return self.choices[index] if 0 <= index < len(self.choices) else (None, None, None)

    def _package_changed(self, index):
        if self.wizard.running:
            return
        version, fv, pkg = self._choice(index)
        self.wizard.set_package(pkg)
        self.sha_label.setText(fv.application_sha256 if fv else '-')
        if version is None:
            self.review_label.setText('Sürüm yok')
            key_missing = not releases.rt.signed_manifest().PUBLIC_KEY_HEX
            self.version_label.setText(colored('Yüklenebilecek imzalı bir sürüm yok.', RED) + '<br>' +
                                       ('Bu programa henüz imza anahtarı gömülmedi.' if key_missing else
                                        '"Sürümler" sayfasından güncellemeleri denetleyin.'))
        elif pkg is None:
            self.review_label.setText('İmzalı listede; henüz indirilmedi.')
            text = (f'Seçilen sürüm: <b>DDM25 Plus {version}</b>' + (' (beta)' if fv and fv.is_beta else '') +
                    '<br>Önce "İndir ve doğrula"ya basın.')
            if self.download_error and self.download_error[0] == version:
                text = (colored('İndirme başarısız; hiçbir şey değişmedi.', RED) + '<br>' +
                        _esc(self.download_error[1]) + '<br>' + text)
            self.version_label.setText(text)
        else:
            self.review_label.setText(pkg.status_text())
            text = f'Yüklenecek sürüm: <b>{pkg.name}</b>' + (' (beta)' if pkg.channel == 'beta' else '')
            newest = self.rel.newest()
            if newest and newest.version == version:
                text += ' (en yeni)'
            installed = self.rel.installed()
            if installed and installed.version == version:
                text += '<br>' + colored('Telsizde zaten bu sürüm var.', AMBER, False)
            notes = fv.notes('tr') if fv else ''
            if notes:
                text += f'<br><i>{_esc(notes)}</i>'
            self.version_label.setText(text)
        self.refresh()

    def download_selected(self):
        version, fv, pkg = self._choice(self.package_combo.currentIndex())
        if version is None or pkg is not None or self.rel.busy:
            return
        self.download_error = None
        self.download_progress.setValue(0)
        self.download_progress.show()
        self.version_label.setText(f'İndiriliyor: DDM25 Plus {version}...')
        self.rel.download(version)

    def _download_progress(self, done, total):
        if total:
            self.download_progress.setValue(min(100, done * 100 // total))

    def _downloaded(self, version, err):
        self.download_progress.hide()
        self.download_error = (version, err) if err else None
        self.reload_packages()

    # --- state to widgets ---------------------------------------------------------------------------------------
    def refresh(self):
        w = self.wizard
        free = self.lock.free_for('flash')
        version, fv, pkg = self._choice(self.package_combo.currentIndex())
        self.download_btn.setVisible(version is not None and pkg is None)
        self.download_btn.setEnabled(not self.rel.busy and w.running is None)
        self.prep_btn.setEnabled(w.running is None and bool(w.port) and w.package is not None and
                                 w.package.installable and self.lock.allowed())
        self.package_combo.setEnabled(w.running is None)
        self.backup_btn.setEnabled(w.can_backup() and free)
        self.backup_btn.setText('Tekrar dene' if w.backup == FAILED else 'Yedek al')
        self.existing_btn.setEnabled(w.running is None)
        self.replan_btn.setVisible(w.running is None and w.backup == OK and w.plan in (FAILED, 'idle', 'used') and
                                   w.install != OK and not w.locked)
        self.check_btn.setEnabled(w.plan == OK and w.can_install())
        self.install_btn.setEnabled(w.can_install() and free)
        self.back_btn.setEnabled(self.can_go_back())
        self.back_btn.setVisible(self.page() in (BACKUP, CHECK, INSTALL))

    # --- backup -------------------------------------------------------------------------------------------------
    def start_backup(self):
        if not self.wizard.can_backup() or not self.lock.take('flash'):
            return
        fw = self.fw_text.text().strip() or 'bilinmiyor'
        paths.save_setting('installed_firmware', fw)
        out = paths.unique_folder(self.layout_.backups, f'dm32-{paths.today()}')
        self.wizard.start_backup(out)
        self.backup_folder.setText(str(out))
        self.backup_status.setText('Hazırlanıyor... Sarı yazıyı görünce telsizi açın (açıksa önce kapatıp açın).')
        self.backup_progress.setValue(0)
        args = ['--port', self.wizard.port, '--out', self.layout_.rel(out), '--installed-firmware', fw,
                '--wait', str(BACKUP_WAIT)]
        self._run('backup', 'dm32_readonly_backup', args, paths.backup_log_path(out))

    def choose_existing_backup(self):
        d = QFileDialog.getExistingDirectory(self, 'Doğrulanmış yedek klasörü', str(self.layout_.backups))
        if d:
            self.use_existing_backup(Path(d))

    def use_existing_backup(self, folder):
        if not verified_backup(folder):
            self.backup_status.setText(colored('Bu klasör doğrulanmış bir yedek değil.', RED))
            return False
        self.wizard.use_existing_backup(folder)
        self.backup_folder.setText(str(folder))
        self.backup_status.setText(colored('Mevcut yedek seçildi ✓', GREEN) +
                                   '<br>Telsiz o yedekten beri değişmediyse kullanın; yükleme aracı yine kontrol eder.')
        self._auto_detect()
        return True

    # --- detect and plan (no port) ------------------------------------------------------------------------------
    def _auto_detect(self):
        """After a verified backup the checks run by themselves: they only read files and open no port"""
        if self.wizard.can_detect():
            self.show_page(CHECK)
            self.start_detect()
        else:
            self.refresh()

    def start_detect(self):
        if not self.wizard.can_detect():
            return
        self.wizard.start_detect()
        self.detect_status.setText('Telsizdeki yazılım yedekten okunuyor...')
        self.check_status.setText('')
        args = ['--backup', self.layout_.rel(self.wizard.backup_dir)]
        self._run('detect', 'radio_detect', args, None)

    def start_plan(self):
        if not self.wizard.can_plan():
            return
        self.wizard.start_plan()
        self.install_dir = paths.unique_folder(self.layout_.backups, f'install-{paths.today()}')
        self.check_status.setText('Kontrol ediliyor...')
        lay = self.layout_
        args = ['--port', self.wizard.port or 'COM1', '--backup', lay.rel(self.wizard.backup_dir),
                '--plus', lay.rel(self.wizard.package.folder), '--out', lay.rel(self.install_dir), '--plan-only']
        if self.wizard.mode == 'install_plus_from_stock':
            args.insert(-1, '--from-stock')
        self._run('plan', 'install_plus', args, self.install_dir.parent / f'{self.install_dir.name}.plan.log')

    # --- install ------------------------------------------------------------------------------------------------
    def current_warnings(self):
        w = self.wizard
        target = self.rel.firmware(w.package.version) if w.package else None
        if target is None:
            return []
        current = None
        if w.backup_dir is not None:
            try:
                current = releases.match_installed(releases.read_backup_program(w.backup_dir),
                                                   [fv for _, fv, _ in self.rel.versions() if fv])
            except (OSError, ValueError):
                current = None
        if current is None and w.detection and w.detection.get('layout') == 'classic':
            current = self.rel.installed() if w.detection.get('firmware') == 'ddm25' else None
        return releases.change_warnings(current, target, w.detection)

    def _show_warnings(self):
        self.warnings = self.current_warnings() if self.wizard.package else []
        self.warn_label.setText('<br>'.join(colored('• ' + _esc(t), LEVEL_COLORS[lv], lv != releases.INFO)
                                            for lv, t in self.warnings))

    def install_question(self):
        w = self.wizard
        p = w.plan_data
        mode = ('İlk kurulum (stoktan): uygulama ve codec bölümü yazılır; codec telsizin kendi yedeğinden '
                'hazırlanır.' if w.mode == 'install_plus_from_stock' else
                'Güncelleme: yalnızca uygulama bölümü yazılır; codec bölümü korunur.')
        lines = [f'Telsize {w.package.name} yüklensin mi?', '',
                 f'Sürüm: {w.package.version} ({w.package.channel})',
                 f'Kimlik (SHA-256): {w.package.sha256}',
                 mode,
                 f'Değişecek: {p["changed_sectors"]} sektör ({p["first"]} - {p["last"]})',
                 f'Yedek: {Path(w.backup_dir).name}', f'Port: {w.port}', '',
                 'Bootloader, kalibrasyon ve kanallar (veri çipi) yazılmaz.',
                 'Onaydan sonra telsizi kapatıp açmanız istenir. Hata olursa kendiliğinden tekrar denenmez.']
        if self.warnings:
            lines += [''] + ['• ' + t for _, t in self.warnings]
        lines += ['', '⚠ ' + disclaimer.SHORT]
        return '\n'.join(lines)

    def start_install(self):
        if not self.wizard.can_install() or not self.lock.allowed():
            return
        self._show_warnings()
        if not self.confirm('Yüklemeyi onaylayın', self.install_question()):
            self.install_status.setText('Onaylanmadı; hiçbir şey yapılmadı.')
            return
        if releases.needs_second_confirm(self.warnings):
            risky = '\n'.join('• ' + t for lv, t in self.warnings if lv == releases.DANGER)
            if not self.confirm('Riskli sürüm değişikliği',
                                f'{risky}\n\nYine de yüklensin mi?\n\n⚠ {disclaimer.SHORT}', 'Evet, riski biliyorum'):
                self.install_status.setText('Onaylanmadı; hiçbir şey yapılmadı.')
                return
        if not self.lock.take('flash'):
            return
        self.wizard.start_install()
        for b in (self.to_check_btn, self.to_restore_btn):
            b.hide()
        self.install_progress.setValue(0)
        self.install_status.setText('Telsizi <b>kapatın</b> (açıksa). Sarı yazıyı görünce açın.')
        lay = self.layout_
        args = ['--port', self.wizard.port, '--backup', lay.rel(self.wizard.backup_dir),
                '--plus', lay.rel(self.wizard.package.folder), '--out', lay.rel(self.install_dir),
                '--wait', str(INSTALL_WAIT)]
        if self.wizard.mode == 'install_plus_from_stock':
            args.append('--from-stock')
        self._run('install', 'install_plus', args, paths.install_log_path(self.install_dir))

    # --- running a tool -----------------------------------------------------------------------------------------
    def _banner(self):
        return self.install_banner if self.step == 'install' else self.backup_banner

    def _cancel_btn(self):
        return self.install_cancel if self.step == 'install' else self.backup_cancel

    def _run(self, step, tool, args, log_path):
        self.lines = []
        self.step = step
        self.log.clear()
        program, argv, shown = self.layout_.tool_command(tool, args)
        if log_path is not None:
            self.log.open_file(log_path, ' '.join([program] + argv))
        self.log.add('$ ' + shown)
        if step in ('backup', 'install'):
            self.port_state.set_locked(True)
        self.runner = self.runner_factory(self)
        self.runner.line.connect(self._line)
        self.runner.finished.connect(self._finished)
        self.refresh()
        self.runner.start(program, argv, cwd=self.root)

    def _line(self, line):
        self.lines.append(line)
        self.log.add(line)
        if self.step in ('plan', 'detect'):
            return
        banner, cancel = self._banner(), self._cancel_btn()
        wait = parsers.ready_wait(line)
        if wait:
            banner.start(wait[1])
            cancel.show()
            return
        s = line.strip()
        if parsers.RE_BOOT_ROM.match(s) or s.startswith('INSTALLING'):
            banner.stop()
            cancel.hide()
        if self.step == 'backup':
            pct = parsers.backup_progress(line)
            if pct is not None:
                banner.stop()
                cancel.hide()
                self.backup_progress.setValue(pct)
                self.backup_status.setText(f'Yedek alınıyor... %{pct}')
        else:
            pct = parsers.install_progress(line)
            if pct is not None:
                banner.stop()
                cancel.hide()
                self.install_progress.setValue(pct)
                self.install_status.setText(f'Yükleniyor... %{pct}. Kabloyu çekmeyin.')

    def cancel_wait(self):
        """Stop a tool still waiting for the radio (before the boot ROM answered: nothing sent yet)"""
        if self.runner is not None and not self._banner().isHidden():
            self.log.add('# İptal: telsiz beklenirken durduruldu.')
            self.runner.kill()

    def _finished(self, code):
        self.backup_banner.stop()
        self.install_banner.stop()
        self.backup_cancel.hide()
        self.install_cancel.hide()
        step = self.step
        self.log.close_file()
        self.runner = None
        if step in ('backup', 'install'):
            self.lock.release('flash')
            self.port_state.set_locked(False)
        if step == 'backup':
            self._backup_done(parsers.BackupResult(self.lines, code))
        elif step == 'detect':
            self._detect_done(code)
        elif step == 'plan':
            self._plan_done(code)
        elif step == 'install':
            self._install_done(code)
        self.refresh()

    def _backup_done(self, r):
        self.wizard.backup_finished(r)
        if r.verified:
            self.backup_progress.setValue(100)
            self.backup_status.setText(colored('Yedek tamam ✓', GREEN))
            self._auto_detect()
        elif r.timeout:
            self.backup_status.setText(colored('Telsiz yakalanamadı.', RED) +
                                       '<br>Telsizi kapatın, "Tekrar dene"ye basın, sarı yazıyı görünce açın.')
        else:
            self.backup_status.setText(colored('Yedek alınamadı.', RED) + '<br>' + _esc(r.error) +
                                       '<br>Kabloyu kontrol edip "Tekrar dene"ye basın.')

    def _detect_done(self, code):
        w = self.wizard
        try:
            d = parsers.parse_detect('\n'.join(self.lines)) if code == 0 else None
            err = '' if d else (parsers.error_text(self.lines) or f'exit code {code}')
        except ValueError as exc:
            d, err = None, str(exc)
        w.detect_finished(d, err)
        if d is None:
            self.detect_status.setText(colored('Telsizdeki yazılım anlaşılamadı.', RED) + '<br>' + _esc(err))
            return
        remember_backup(w.backup_dir, d)
        text = detection_text(d)
        if not d.get('supported'):
            self.detect_status.setText(colored(text, RED) + '<br>Bu telsize bu programla DDM25 Plus kurulamaz.')
            return
        color = GREEN if d.get('layout') == 'classic' or d.get('stock_tested') else AMBER
        self.detect_status.setText(colored(text, color))
        if w.can_plan():
            self.start_plan()

    def _plan_done(self, code):
        w = self.wizard
        try:
            plan = parsers.parse_plan('\n'.join(self.lines)) if code == 0 else None
            err = '' if plan else (parsers.error_text(self.lines) or f'exit code {code}')
        except ValueError as exc:
            plan, err = None, str(exc)
        safe = plan is not None and parsers.plan_safe(plan, w.mode)
        w.plan_finished(plan, err, safe)
        if plan and safe:
            if plan['mode'] == 'install_plus_from_stock':
                what = (f'Değişecek: <b>uygulama ve codec bölümü, {plan["changed_sectors"]} sektör</b> '
                        '(codec telsizin kendi yedeğinden hazırlanır).<br>')
                slot = colored('✓ Codec bölümü telsizin kendi codec\'iyle hazırlanıp geri okunarak denetlenir',
                               GREEN, False)
            else:
                what = f'Değişecek: <b>uygulama bölümü, {plan["changed_sectors"]} sektör</b>.<br>'
                slot = colored('✓ Codec bölümü değişmez', GREEN, False)
            self.check_status.setText(what + colored('✓ Bootloader değişmez', GREEN, False) + '<br>' + slot + '<br>' +
                                      colored('✓ Kalibrasyon ve kanallar (veri çipi) yazılmaz', GREEN, False))
        elif plan:
            self.check_status.setText(colored('Bu plan güvenli görünmüyor; yükleme kapalı.', RED))
        else:
            self.check_status.setText(colored('Kontrol yapılamadı.', RED) + '<br>' + _esc(err))
        if plan:
            self.plan_details.setText(
                f'Sürüm: {w.package.name}\nSHA-256: {w.package.sha256}\nMod: {plan.get("mode")}\n'
                f'Sektörler: {plan["changed_sectors"]} ({plan["first"]} - {plan["last"]})\n'
                f'boot_area_unchanged: {plan["boot_area_unchanged"]}, '
                f'codec_slot_unchanged: {plan["codec_slot_unchanged"]}\n'
                f'Stok: {plan.get("stock_profile") or "-"}\n'
                f'Yedek: {w.backup_dir}\nYükleme klasörü: {self.install_dir}')

    def _install_done(self, code):
        w = self.wizard
        receipt = None
        try:
            receipt = json.loads((self.install_dir / 'install-receipt.json').read_text())
        except (OSError, ValueError, TypeError):
            pass
        r = parsers.InstallResult(self.lines, code, receipt)
        w.install_finished(r)
        flags = '\n'.join(f'{k}: {v}' for k, v in r.flags().items())
        detail = f'Yükleme klasörü: {self.install_dir}\nSonuç: {"PASS" if r.passed else r.error}\n{flags}'
        self.install_details.setText(detail)
        self.done_details.setText(detail)
        if r.passed:
            self.install_progress.setValue(100)
            paths.save_setting('installed_sha256', w.package.sha256)
            paths.save_setting('installed_version', w.package.version)
            paths.save_setting('restore_previous_backup', str(w.backup_dir))
            self.fw_text.setText(w.package.name)
            paths.save_setting('installed_firmware', w.package.name)
            self.installed.emit(w.package.version)
            self.show_page(DONE)
        elif not r.writes_started:
            self.install_status.setText(
                colored('Hiçbir şey yazılmadı.', AMBER) +
                (' Telsiz zamanında açılmadı.' if r.timeout else ' ' + _esc(r.error)) +
                '<br>Kendiliğinden tekrar denenmez. İsterseniz kontrol adımına dönüp elle yeniden yapın.')
            self.to_check_btn.show()
        else:
            paths.save_setting('restore_previous_backup', str(w.backup_dir))
            self.install_status.setText(
                colored('Yükleme yazma başladıktan sonra durdu.', RED) +
                '<br>Kendiliğinden tekrar denenmez. Telsizi kapatmayın, kabloyu çekmeyin. '
                '"Geri dön" sayfasından yüklemeden önce alınan yedekle geri yükleyebilirsiniz.')
            self.to_restore_btn.show()


def verified_backup(folder):
    try:
        m = json.loads((Path(folder) / 'backup-manifest.json').read_text())
        return m.get('backup_verified') is True and all((Path(folder) / n).is_file()
                                                         for n in ('progmem-pass1.bin', 'q128-pass1.bin'))
    except (OSError, ValueError):
        return False


def remember_backup(folder, detection):
    """Keeps a stock backup's folder for "Stok yazılıma dön" (the first one stays the default)"""
    if not folder or not detection or detection.get('layout') != 'stock':
        return
    s = paths.load_settings()
    known = list(s.get('stock_backups', []))
    if str(folder) not in known:
        known.append(str(folder))
        paths.save_setting('stock_backups', known)


def _esc(text):
    return (text or '').replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')
