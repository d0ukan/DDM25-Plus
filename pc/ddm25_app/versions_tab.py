"""Sürümler: every signed DDM25 Plus release (beta and stable marked), the update check, "Bu sürüme geç" for any
listed version (it goes through the normal backup -> check -> install flow, with the compatibility warnings from the
manifests), and the program's own update.

Network access only on "Güncellemeleri denetle" or the start-up check (switch "Açılışta denetle" off to stop it).
Nothing is installed without the install page's own click and confirmation.
"""
from PyQt5.QtCore import Qt, pyqtSignal
from PyQt5.QtWidgets import (QCheckBox, QGroupBox, QHBoxLayout, QHeaderView, QLabel, QProgressBar, QPushButton,
                             QTableWidget, QTableWidgetItem, QVBoxLayout, QWidget)

from . import __version__, disclaimer, paths, releases, selfupdate
from .update_tab import AMBER, GREEN, RED, _esc, colored, default_confirm
from .widgets import Details, big_button, bigger, plain_label

COLUMNS = ('Sürüm', 'Kanal', 'Durum', 'Notlar')


class VersionsTab(QWidget):
    switch_to = pyqtSignal(str)          # version: open the install flow with it
    quit_for_update = pyqtSignal()       # the helper runs; the window must close

    def __init__(self, layout, rel, confirm=None, start_helper=None, parent=None):
        super().__init__(parent)
        self.layout_ = layout
        self.rel = rel
        self.confirm = confirm or (lambda title, text, yes='Evet': default_confirm(self, title, text, yes))
        self.start_helper = start_helper     # (program, args) -> bool; injected by tests
        self.rows = []
        self.app_entry = None
        self._build()
        rel.changed.connect(self.refresh)
        rel.checked.connect(self._checked)
        self.refresh()

    def _build(self):
        outer = QVBoxLayout(self)
        t = QLabel('Sürümler')
        bigger(t, 8, True)
        outer.addWidget(t)
        self.banner = plain_label('', 3)
        outer.addWidget(self.banner)
        row = QHBoxLayout()
        self.check_btn = big_button('Güncellemeleri denetle', 'SP_BrowserReload',
                                    'GitHub\'daki imzalı DDM25 Plus sürümlerini listeler (internet, telsize bir şey '
                                    'yapılmaz).')
        self.check_btn.clicked.connect(self.check)
        row.addWidget(self.check_btn)
        box = QVBoxLayout()
        s = paths.load_settings()
        self.auto_box = QCheckBox('Açılışta denetle')
        self.auto_box.setChecked(s.get('auto_check', True))
        self.auto_box.toggled.connect(lambda on: paths.save_setting('auto_check', bool(on)))
        self.beta_box = QCheckBox('Beta sürümleri de öner')
        self.beta_box.setChecked(s.get('include_beta', True))
        self.beta_box.toggled.connect(self._beta_toggled)
        box.addWidget(self.auto_box)
        box.addWidget(self.beta_box)
        row.addLayout(box)
        outer.addLayout(row)
        self.status = plain_label()
        outer.addWidget(self.status)
        self.table = QTableWidget(0, len(COLUMNS))
        self.table.setHorizontalHeaderLabels(COLUMNS)
        self.table.horizontalHeader().setSectionResizeMode(3, QHeaderView.Stretch)
        self.table.verticalHeader().setVisible(False)
        self.table.setEditTriggers(QTableWidget.NoEditTriggers)
        self.table.setSelectionBehavior(QTableWidget.SelectRows)
        self.table.setSelectionMode(QTableWidget.SingleSelection)
        self.table.itemSelectionChanged.connect(self._selected)
        outer.addWidget(self.table, 1)
        self.switch_btn = big_button('Bu sürüme geç', 'SP_ArrowRight',
                                     'Seçilen sürümü yedek, kontrol ve onayla yükler (eski sürümler dahil; '
                                     'uyumluluk uyarıları gösterilir).')
        self.switch_btn.clicked.connect(self.switch_selected)
        outer.addWidget(self.switch_btn)
        g = QGroupBox('Program')
        gl = QVBoxLayout(g)
        self.app_label = plain_label(f'Bu program: DDM25 Plus {__version__}')
        gl.addWidget(self.app_label)
        self.app_btn = QPushButton('Programı güncelle')
        self.app_btn.setToolTip('Yeni programı indirir, imzasını ve özetini denetler, programı kapatıp yenisiyle '
                                'değiştirir. Açılmazsa eskisine döner. Yedekleriniz ve ayarlarınız etkilenmez.')
        self.app_btn.clicked.connect(self.update_app)
        gl.addWidget(self.app_btn, 0, Qt.AlignLeft)
        self.app_progress = QProgressBar()
        self.app_progress.setRange(0, 100)
        self.app_progress.hide()
        gl.addWidget(self.app_progress)
        outer.addWidget(g)
        self.details = Details()
        self.rejected_label = QLabel('-')
        self.rejected_label.setWordWrap(True)
        self.rejected_label.setTextInteractionFlags(Qt.TextSelectableByMouse)
        self.details.add(self.rejected_label)
        outer.addWidget(self.details)

    def _beta_toggled(self, on):
        paths.save_setting('include_beta', bool(on))
        self.refresh()

    def check(self):
        self.status.setText('Denetleniyor...')
        self.rel.check()

    def _checked(self, err):
        if err:
            self.status.setText(colored('Denetlenemedi.', RED) + ' ' + _esc(err) +
                                '<br>Liste son başarılı denetimden (bilgisayardaki imzalı kopyalardan) gösteriliyor.')
        else:
            self.status.setText(colored('Denetlendi ✓', GREEN) + f' ({len(self.rel.catalog.firmware)} imzalı sürüm)')
        self.refresh()

    def selected_version(self):
        r = self.table.currentRow()
        return self.rows[r][0] if 0 <= r < len(self.rows) else None

    def _selected(self):
        self.refresh_buttons()

    def switch_selected(self):
        v = self.selected_version()
        if v:
            self.switch_to.emit(v)

    def refresh(self):
        current = self.selected_version()
        self.rows = self.rel.versions()
        installed = self.rel.installed()
        self.table.setRowCount(len(self.rows))
        for i, (version, fv, pkg) in enumerate(self.rows):
            state = []
            if installed and installed.version == version:
                state.append('telsizde')
            state.append('indirildi' if pkg else 'indirilmedi')
            cells = (version, 'Beta' if fv and fv.is_beta else 'Kararlı', ', '.join(state),
                     fv.notes('tr') if fv else '')
            for c, text in enumerate(cells):
                item = QTableWidgetItem(text)
                if c == 3 and fv:
                    item.setToolTip(f'TR: {fv.notes("tr")}\nEN: {fv.notes("en")}')
                self.table.setItem(i, c, item)
            if version == current:
                self.table.selectRow(i)
        new = self.rel.update_available()
        if new is not None:
            self.banner.setText(colored(f'Yeni sürüm var: DDM25 Plus {new.version}', AMBER) +
                                f'<br><i>{_esc(new.notes("tr"))}</i>')
        elif not self.rows:
            key = releases.rt.signed_manifest().PUBLIC_KEY_HEX
            self.banner.setText('İmzalı sürüm listesi boş. ' + ('Bu programa henüz imza anahtarı gömülmedi.'
                                                                  if not key else '"Güncellemeleri denetle"ye basın.'))
        else:
            self.banner.setText('')
        rej = self.rel.catalog.rejected
        self.rejected_label.setText('Listelenmeyen (imzası geçmeyen) sürümler:\n' +
                                    '\n'.join(f'{t}: {r}' for t, r in rej) if rej else 'İmzası geçmeyen sürüm yok.')
        self.app_entry = self.rel.app_update()
        if self.app_entry is not None:
            entry = self.app_entry[0]
            notes = (entry.get('notes') or {}).get('tr', '')
            self.app_label.setText(f'Bu program: DDM25 Plus {__version__}<br>' +
                                   colored(f'Yeni program sürümü var: {entry["version"]}', AMBER) +
                                   (f'<br><i>{_esc(notes)}</i>' if notes else ''))
        else:
            self.app_label.setText(f'Bu program: DDM25 Plus {__version__}' +
                                   ('' if self.layout_.can_self_update else
                                    '<br>(Kaynak koddan çalışıyor: güncellemek için depoyu güncelleyin.)'))
        self.refresh_buttons()

    def refresh_buttons(self):
        busy = bool(self.rel.busy)
        self.check_btn.setEnabled(not busy)
        self.check_btn.setText('Denetleniyor...' if self.rel.busy == 'check' else 'Güncellemeleri denetle')
        self.switch_btn.setEnabled(self.selected_version() is not None and not busy)
        self.app_btn.setEnabled(self.app_entry is not None and self.layout_.can_self_update and not busy)

    # --- the program's own update -------------------------------------------------------------------------------
    def _app_progress(self, done, total):
        from PyQt5.QtCore import QCoreApplication
        self.app_progress.setValue(min(100, done * 100 // max(1, total)))
        QCoreApplication.processEvents()

    def update_app(self):
        if self.app_entry is None or not self.layout_.can_self_update:
            return False
        entry = self.app_entry[0]
        if not self.confirm('Programı güncelle',
                            f'DDM25 Plus programı {__version__} → {entry["version"]} güncellensin mi?\n\n'
                            'Yeni program indirilir, imzası ve SHA-256 özeti denetlenir. Bu pencere kapanır; yeni '
                            'program açılmazsa eskisi geri gelir. Yedekleriniz ve ayarlarınız program klasöründe '
                            f'değildir, etkilenmez.\n\n⚠ {disclaimer.SHORT}', 'Evet, güncelle'):
            return False
        self.app_progress.show()
        try:
            z = releases.download_app(entry, self.layout_.updates, self.rel.download_fn,
                                      self._app_progress)
            staged = selfupdate.stage(z, self.layout_.updates, entry['version'])
        except Exception as exc:
            self.app_progress.hide()
            self.status.setText(colored('Program güncellenemedi; hiçbir şey değişmedi.', RED) + ' ' + _esc(str(exc)))
            return False
        import os
        program, args = selfupdate.helper_command(
            staged, os.getpid(), paths.app_dir(), self.layout_.updates / 'update-started.json',
            self.layout_.updates / selfupdate.RESULT_FILE)
        start = self.start_helper or _start_detached
        if not start(program, args):
            self.status.setText(colored('Güncelleme yardımcısı başlatılamadı; hiçbir şey değişmedi.', RED))
            return False
        self.quit_for_update.emit()
        return True


def _start_detached(program, args):
    from PyQt5.QtCore import QProcess
    ok = QProcess.startDetached(program, args)
    return ok[0] if isinstance(ok, tuple) else bool(ok)
