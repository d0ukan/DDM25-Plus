"""Ana sayfa: the radio's status in plain words, "Yeni sürüm var", the disclaimer state and the big buttons."""
from PyQt5.QtCore import Qt, pyqtSignal
from PyQt5.QtWidgets import QFrame, QGridLayout, QLabel, QPushButton, QVBoxLayout, QWidget

from . import __version__, AUTHOR, paths
from .widgets import big_button, bigger, plain_label

GREEN, AMBER, GREY, RED = '#2e7d32', '#b26a00', '#666666', '#c62828'


def update_state(installed, newest):
    """('current' | 'update' | 'unknown' | 'none', text) for the installed and the newest FirmwareVersion"""
    if newest is None:
        return 'none', 'Henüz imzalı sürüm listesi yok: "Sürümler" sayfasından denetleyin.'
    if installed is None:
        return 'unknown', f'Telsizdeki sürüm bilinmiyor. En yeni sürüm: DDM25 Plus {newest.version}.'
    if newest.key() <= installed.key():
        return 'current', 'Telsiz güncel ✓'
    return 'update', f'Yeni sürüm var: DDM25 Plus {newest.version}'


class HomeTab(QWidget):
    open_page = pyqtSignal(str)
    help_requested = pyqtSignal()
    disclaimer_requested = pyqtSignal()

    def __init__(self, rel, port_state, gate, show_data=False, parent=None):
        super().__init__(parent)
        self.rel = rel
        self.port_state = port_state
        self.gate = gate
        lay = QVBoxLayout(self)
        title = QLabel('DDM25 Plus')
        bigger(title, 14, True)
        lay.addWidget(title)
        lay.addWidget(plain_label(f'Baofeng DM-32UV için DDM25 Plus programlayıcı {__version__} · {AUTHOR}', 1))
        self.gate_label = plain_label('', 3)
        self.gate_btn = QPushButton('Sorumluluk reddini oku ve kabul et')
        self.gate_btn.clicked.connect(self.disclaimer_requested.emit)
        lay.addWidget(self.gate_label)
        lay.addWidget(self.gate_btn, 0, Qt.AlignLeft)
        card = QFrame()
        card.setFrameShape(QFrame.StyledPanel)
        cl = QVBoxLayout(card)
        self.state_label = QLabel()
        bigger(self.state_label, 9, True)
        self.state_label.setWordWrap(True)
        self.notes_label = plain_label('', 1)
        self.version_label = plain_label('', 3)
        self.cable_label = plain_label('', 2)
        for w in (self.state_label, self.notes_label, self.version_label, self.cable_label):
            cl.addWidget(w)
        lay.addWidget(card)
        grid = QGridLayout()
        specs = [('update', 'Telsizi güncelle', 'SP_ArrowUp', 'Yedek alıp seçilen DDM25 Plus sürümünü yükler '
                  '(stoktan ilk kurulum dahil).'),
                 ('versions', 'Sürümler', 'SP_FileDialogListView', 'İmzalı sürümler, güncelleme, eski sürüme geçiş.'),
                 ('restore', 'Geri dön', 'SP_ArrowBack', 'Önceki sürüme ya da kendi yedeğinizle stok yazılıma dön.'),
                 ('health', 'Telsizin durumu', 'SP_MessageBoxInformation', 'Bellek, hata kaydı ve açılış bilgisi.'),
                 ('live', 'Canlı panel', 'SP_MediaPlay', 'Sinyal, durum ve P25 tarama sayaçları (sadece okuma).'),
                 ('channels', 'Kanallarım', 'SP_FileDialogDetailedView', 'Son yedekteki kanallar (sadece okuma).')]
        if show_data:
            specs.append(('data', 'Verileri güncelle', 'SP_DriveNetIcon', 'ID, röle ve uydu verisi; WSL derlemesi.'))
        self.buttons = {}
        for i, (key, text, ic, tip) in enumerate(specs):
            b = big_button(text, ic, tip)
            b.setMinimumHeight(80)
            bigger(b, 2, True)
            b.clicked.connect(lambda _, k=key: self.open_page.emit(k))
            grid.addWidget(b, i // 2, i % 2)
            self.buttons[key] = b
        lay.addLayout(grid)
        help_btn = big_button('Yardım', 'SP_DialogHelpButton', 'Sık karşılaşılan durumlar ve ne yapılacağı.')
        help_btn.clicked.connect(self.help_requested.emit)
        lay.addWidget(help_btn, 0, Qt.AlignLeft)
        lay.addStretch(1)
        port_state.changed.connect(lambda _: self.refresh())
        rel.changed.connect(self.refresh)
        self.refresh()

    def refresh(self):
        ok = self.gate()
        self.gate_label.setText('' if ok else
                                f'<span style="color:{RED};font-weight:bold;">Sorumluluk reddi kabul edilmedi: '
                                'telsiz işlemleri kapalı.</span>')
        self.gate_label.setVisible(not ok)
        self.gate_btn.setVisible(not ok)
        installed = self.rel.installed()
        newest = self.rel.newest()
        state, text = update_state(installed, newest)
        self.state = state
        color = {'current': GREEN, 'update': AMBER}.get(state, GREY)
        self.state_label.setText(text)
        self.state_label.setStyleSheet(f'color: {color};')
        self.notes_label.setText(f'<i>{newest.notes("tr")}</i>' if state == 'update' and newest else '')
        s = paths.load_settings()
        if installed:
            self.version_label.setText(f'Telsizdeki sürüm: <b>DDM25 Plus {installed.version}</b>')
        else:
            self.version_label.setText('Telsizdeki sürüm: ' + (s.get('installed_firmware') or 'bilinmiyor'))
        self.cable_label.setText(self.port_state.status_text())
        app = self.rel.app_update()
        if app is not None:
            self.notes_label.setText(self.notes_label.text() + f'<br>Yeni program sürümü var: {app[0]["version"]} '
                                     '("Sürümler" sayfası).')
