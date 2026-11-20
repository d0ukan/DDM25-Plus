"""The selected cable port, shared by the pages, and a small picker that shows itself only when needed."""
from PyQt5.QtCore import QObject, pyqtSignal
from PyQt5.QtWidgets import QComboBox, QHBoxLayout, QLabel, QPushButton, QStyle, QWidget

from . import paths, ports


class PortState(QObject):
    changed = pyqtSignal(object)        # port name or None

    def __init__(self, lister=ports.list_ports, parent=None):
        super().__init__(parent)
        self.lister = lister
        self.found = []
        self.cands = []
        self.port = None
        self.locked = False

    def refresh(self):
        if self.locked:
            return
        self.found = self.lister()
        self.cands = ports.candidates(self.found)
        pick = ports.preferred(self.found, paths.load_settings().get('port'))
        self.set(pick.device if pick else None)

    def set(self, port):
        if self.locked:
            return
        self.port = port
        if port:
            paths.save_setting('port', port)
        self.changed.emit(port)

    def set_locked(self, on):
        """No port change while a radio step holds the cable"""
        if on != self.locked:
            self.locked = on
            self.changed.emit(self.port)

    def has_cable(self):
        return any(p.rank <= 1 for p in self.found)

    def status_text(self):
        return ports.status_text(self.found, self.port)


class PortPicker(QWidget):
    """'Telsiz kablosu bulundu (COMx)'; a choice only when there is more than one candidate"""

    def __init__(self, state, parent=None):
        super().__init__(parent)
        self.state = state
        lay = QHBoxLayout(self)
        lay.setContentsMargins(0, 0, 0, 0)
        self.label = QLabel()
        self.combo = QComboBox()
        self.combo.setToolTip('Birden fazla olası port var: telsiz kablosunu seçin.')
        self.combo.currentIndexChanged.connect(self._chosen)
        self.refresh_btn = QPushButton('Yenile')
        self.refresh_btn.setIcon(self.style().standardIcon(QStyle.SP_BrowserReload))
        self.refresh_btn.setToolTip('Portları yeniden ara (port açılmaz).')
        self.refresh_btn.clicked.connect(state.refresh)
        lay.addWidget(self.label, 1)
        lay.addWidget(self.combo)
        lay.addWidget(self.refresh_btn)
        state.changed.connect(self._show)
        self._show(state.port)

    def _show(self, port):
        self.label.setText(self.state.status_text())
        many = len(self.state.cands) > 1 or (bool(self.state.cands) and not self.state.port)
        self.combo.blockSignals(True)
        self.combo.clear()
        for p in self.state.cands:
            self.combo.addItem(p.label(), p.device)
        i = self.combo.findData(port)
        self.combo.setCurrentIndex(i if i >= 0 else -1)
        self.combo.blockSignals(False)
        self.combo.setVisible(bool(many))
        self.combo.setEnabled(not self.state.locked)
        self.refresh_btn.setEnabled(not self.state.locked)

    def _chosen(self, i):
        self.state.set(self.combo.itemData(i))
