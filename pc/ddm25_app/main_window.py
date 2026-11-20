"""The DDM25 Plus main window: home page, the task pages, help / disclaimer / about, status bar.

At the first start (and after a changed disclaimer text) the disclaimer dialog comes first; until it is accepted the
RadioLock refuses every radio step, so no radio button works.
"""
from PyQt5.QtCore import QByteArray, Qt, QTimer
from PyQt5.QtWidgets import QLabel, QMainWindow, QMessageBox, QTabWidget, QToolBar

from . import __version__, disclaimer, paths, ports, releases, selfupdate
from .codeplug_tab import CodeplugTab
from .data_tab import DataTab
from .health_tab import HealthTab
from .home_tab import HomeTab
from .live_tab import LiveTab
from .portstate import PortState
from .release_state import Releases
from .restore_tab import RestoreTab
from .runner import ProcessRunner, RadioLock
from .update_tab import PREP, UpdateTab
from .versions_tab import VersionsTab
from .widgets import bigger, icon, show_about, show_help

TITLE = 'DDM25 Plus'


class MainWindow(QMainWindow):
    def __init__(self, layout=None, runner_factory=ProcessRunner, port_lister=ports.list_ports, confirm=None,
                 fetch=None, download=None, run_async=True, show_help_dialog=show_help, ask_disclaimer=None,
                 start_helper=None, key=None, auto_check=None):
        super().__init__()
        self.layout_ = layout or paths.detect_layout()
        self.root = self.layout_.root
        self.ask_disclaimer = ask_disclaimer or (lambda: disclaimer.ask(self))
        self.gate_ok = disclaimer.accepted()
        self.lock = RadioLock(gate=lambda: self.gate_ok)
        self.show_help_dialog = show_help_dialog
        self.setWindowTitle(f'{TITLE} {__version__}')
        self.rel = Releases(self.layout_, fetch=fetch or releases.http_fetch,
                            download=download or releases.http_download, run_async=run_async, key=key, parent=self)
        self.port_state = PortState(port_lister, self)
        self.port_state.refresh()
        self.tabs = QTabWidget()
        bigger(self.tabs.tabBar(), 2)
        self.updater = UpdateTab(self.layout_, self.lock, self.port_state, self.rel, runner_factory, confirm)
        self.versions = VersionsTab(self.layout_, self.rel, confirm, start_helper)
        self.restore = RestoreTab(self.layout_, self.lock, self.port_state, runner_factory, confirm)
        self.health = HealthTab(self.layout_, self.lock, self.port_state, runner_factory)
        self.live = LiveTab(self.layout_, self.lock, self.port_state, runner_factory)
        self.codeplug = CodeplugTab(self.layout_.backups)
        self.data = None
        if self.layout_.can_build:
            kw = {'fetch': fetch} if fetch else {}
            self.data = DataTab(self.layout_, runner_factory, run_async=run_async, **kw)
        self.home = HomeTab(self.rel, self.port_state, self.lock.allowed, show_data=self.data is not None)
        self.pages = {}
        for key_, widget, ic, text in (('home', self.home, 'SP_DirHomeIcon', 'Ana sayfa'),
                                       ('update', self.updater, 'SP_ArrowUp', 'Telsizi güncelle'),
                                       ('versions', self.versions, 'SP_FileDialogListView', 'Sürümler'),
                                       ('restore', self.restore, 'SP_ArrowBack', 'Geri dön'),
                                       ('health', self.health, 'SP_MessageBoxInformation', 'Telsizin durumu'),
                                       ('live', self.live, 'SP_MediaPlay', 'Canlı panel'),
                                       ('channels', self.codeplug, 'SP_FileDialogDetailedView', 'Kanallarım'),
                                       ('data', self.data, 'SP_DriveNetIcon', 'Verileri güncelle')):
            if widget is None:
                continue
            self.pages[key_] = self.tabs.addTab(widget, icon(self, ic), text)
        self.setCentralWidget(self.tabs)
        bar = QToolBar('Yardım')
        bar.setMovable(False)
        bar.setToolButtonStyle(Qt.ToolButtonTextBesideIcon)
        for text, ic, tip, fn in (('Yardım', 'SP_DialogHelpButton', 'Sık karşılaşılan durumlar', self.help),
                                  ('Sorumluluk reddi', 'SP_MessageBoxWarning', 'Sorumluluk reddini göster',
                                   self.show_disclaimer),
                                  ('Hakkında', 'SP_MessageBoxInformation', 'DDM25 Plus · d0ukan · TA2DOU',
                                   lambda: show_about(self))):
            act = bar.addAction(icon(self, ic), text)
            act.setToolTip(tip)
            act.triggered.connect(fn)
        self.addToolBar(bar)
        self.home.open_page.connect(self.open_page)
        self.home.help_requested.connect(self.help)
        self.home.disclaimer_requested.connect(self.show_disclaimer)
        self.updater.go_home.connect(lambda: self.open_page('home'))
        self.updater.go_health.connect(lambda: self.open_page('health'))
        self.updater.go_restore.connect(lambda: self.open_page('restore'))
        self.updater.installed.connect(lambda _: self.update_status())
        self.restore.restored.connect(lambda _: self.update_status())
        self.versions.switch_to.connect(self.switch_to_version)
        self.versions.quit_for_update.connect(self.quit_for_update)
        self.status_port = QLabel()
        self.status_build = QLabel()
        self.statusBar().addWidget(self.status_port)
        self.statusBar().addPermanentWidget(self.status_build)
        where = QLabel(f'Veriler: {self.layout_.data}')
        where.setToolTip('Yedekler, indirilen sürümler ve ayarlar burada; program klasöründe değil.')
        self.statusBar().addPermanentWidget(where)
        self.port_state.changed.connect(lambda _: self.update_status())
        self.rel.changed.connect(self.update_status)
        self.update_status()
        self._restore_geometry()
        self.guard = QTimer(self)
        self.guard.setInterval(300)
        self.guard.timeout.connect(self._guard)
        self.guard.start()
        self.auto_check = paths.load_settings().get('auto_check', True) if auto_check is None else auto_check
        # The self-update helper writes its result just after this program came up (or after a rollback started
        # the old one again): look for it for a short while after the start
        self.update_polls = 0
        self.update_timer = QTimer(self)
        self.update_timer.setInterval(2000)
        self.update_timer.timeout.connect(self.poll_update_result)
        if self.layout_.can_self_update:
            selfupdate.cleanup(self.layout_.updates, keep=paths.app_dir())
            self.update_timer.start()

    # --- start-up: disclaimer first, then the optional update check ---------------------------------------------
    def first_start(self):
        """Called once the window is shown"""
        if not self.gate_ok:
            self.show_disclaimer()
        if self.auto_check:
            self.rel.check()

    def poll_update_result(self, show=None):
        self.update_polls += 1
        result = selfupdate.read_result(self.layout_.updates)
        if result is not None or self.update_polls >= 15:
            self.update_timer.stop()
        if result is None:
            return None
        r = result.get('result')
        text = {'ok': 'Program güncellendi ✓', 'rolled_back': 'Yeni program açılamadı; eski sürüme dönüldü.',
                'aborted': 'Program güncellemesi yapılamadı; hiçbir şey değişmedi.'}.get(r, str(r))
        (show or (lambda t: QMessageBox.information(self, TITLE, t)))(text)
        return text

    def refresh_gate(self):
        self.gate_ok = disclaimer.accepted()
        self._guard()
        self.update_status()

    def show_disclaimer(self):
        ok = self.ask_disclaimer()
        self.gate_ok = disclaimer.accepted()
        if not ok and not self.gate_ok:
            self.statusBar().showMessage('Sorumluluk reddi kabul edilmedi: telsiz işlemleri kapalı.', 10000)
        self.update_status()
        self._guard()

    # --- navigation ---------------------------------------------------------------------------------------------
    def open_page(self, key):
        if key == 'update' and self.updater.wizard.running is None and self.updater.page() in (PREP,):
            self.updater.reload_packages()
        if key == 'home':
            self.home.refresh()
        if key in self.pages:
            self.tabs.setCurrentIndex(self.pages[key])

    def switch_to_version(self, version):
        if self.updater.select_version(version):
            self.open_page('update')

    def help(self):
        self.show_help_dialog(self)

    def _guard(self):
        self.updater.refresh()
        self.restore.refresh()
        self.health.refresh()
        self.live.refresh()

    def update_status(self):
        self.status_port.setText(f'Kablo: {self.port_state.port or "bulunamadı"}')
        installed = self.rel.installed()
        self.status_build.setText('Telsizdeki sürüm: ' + (installed.version if installed else
                                                          paths.load_settings().get('installed_firmware') or
                                                          'bilinmiyor'))
        self.home.refresh()

    def _restore_geometry(self):
        g = paths.load_settings().get('window_geometry')
        if g:
            try:
                if self.restoreGeometry(QByteArray.fromHex(g.encode())):
                    return
            except (ValueError, TypeError):
                pass
        self.resize(1100, 800)

    def busy_text(self):
        """Why the window must stay open: closing it would end a running tool (QProcess kills its child)"""
        if self.updater.runner is not None:
            return {'install': 'Yükleme sürüyor', 'backup': 'Yedek alınıyor', 'plan': 'Kontrol sürüyor',
                    'detect': 'Kontrol sürüyor'}.get(self.updater.step, 'Bir adım sürüyor')
        if self.restore.runner is not None:
            return 'Geri yükleme sürüyor' if self.restore.step == 'restore' else 'Kontrol sürüyor'
        if self.health.runner is not None:
            return 'Durum okunuyor'
        if self.data is not None and (self.data.runner is not None or self.data.jobs):
            return 'Derleme / indirme sürüyor'
        return ''

    def close_logs(self):
        for tab in (self.updater, self.restore, self.health, self.live, self.data):
            if tab is not None:
                tab.log.close_file()

    def quit_for_update(self):
        """The self-update helper runs: close without asking (nothing radio-related may be running)"""
        if self.busy_text():
            return
        self.live.stop()
        self.close_logs()
        self._closing_for_update = True
        self.close()

    def closeEvent(self, event):
        busy = self.busy_text()
        if busy:
            QMessageBox.warning(self, TITLE, f'{busy}. Bitmeden pencere kapatılamaz '
                                             '(kapatmak işlemi yarıda keser).')
            event.ignore()
            return
        self.live.stop()            # read only: ending the reader just closes the port
        paths.save_setting('window_geometry', bytes(self.saveGeometry().toHex()).decode())
        self.close_logs()
        event.accept()
