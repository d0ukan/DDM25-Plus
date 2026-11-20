"""Telsizin durumu: one read of the radio's health per click, shown as a traffic light and a few plain lines."""
import time

from PyQt5.QtCore import Qt, QTimer
from PyQt5.QtWidgets import (QCheckBox, QFormLayout, QFrame, QHBoxLayout, QHeaderView, QLabel, QProgressBar,
                             QTableWidget, QTableWidgetItem, QVBoxLayout, QWidget)

from . import parsers
from .portstate import PortPicker
from .runner import ProcessRunner
from .widgets import Details, LogPane, big_button, bigger, plain_label

AUTO_SECONDS = 30
LOW_STACK = 200        # bytes: the radio test plan's "each > 200 B"
LOW_HEAP = 1024        # heap_min_free > 1 KB
LIGHTS = {'green': ('#2e7d32', 'Her şey yolunda'), 'amber': ('#e0a800', 'Dikkat: bellek az'),
          'red': ('#c62828', 'Hata kaydı var'), 'grey': ('#8a8a8a', 'Henüz okunmadı')}
RESET_NAMES = {'power': 'normal açılış (güç)', 'pin': 'reset tuşu / kablo', 'watchdog': 'watchdog (takılma)',
               'software': 'yazılım yeniden başlatması', 'brownout': 'düşük pil / voltaj'}


def verdict(s):
    """'red' crash record, 'amber' low heap or a stack under 200 B, else 'green'"""
    if s['crash_valid']:
        return 'red'
    low_task = any(v is not None and v < LOW_STACK for v in s['tasks'].values())
    if (s['heap_min_free'] is not None and s['heap_min_free'] < LOW_HEAP) or low_task:
        return 'amber'
    return 'green'


def plain_lines(s):
    up = s['uptime_ms']
    lines = []
    if up is not None:
        m = up // 60000
        lines.append(f'Telsiz {m // 60} sa {m % 60} dk\'dır açık.' if m >= 60 else f'Telsiz {m} dk\'dır açık.')
    if s['heap_min_free'] is not None:
        kb = s['heap_min_free'] / 1024
        lines.append(f'Boş bellek (en düşük): {kb:.1f} KB'.replace('.', ',') +
                     (' - az' if s['heap_min_free'] < LOW_HEAP else ' - yeterli'))
    if s['crash_valid']:
        lines.append(f'Hata kaydı var: görev {s["crash_task"]}. Bize "Ayrıntılar"daki çıktıyı gönderin.')
    else:
        lines.append('Hata kaydı yok.')
    if s['reset_cause']:
        lines.append('Son açılış: ' + RESET_NAMES.get(s['reset_cause'], s['reset_cause']) + '.')
    return lines


class HealthTab(QWidget):
    def __init__(self, layout, lock, port_state, runner_factory=ProcessRunner, parent=None):
        super().__init__(parent)
        self.layout_ = layout
        self.root = layout.root
        self.lock = lock
        self.port_state = port_state
        self.runner_factory = runner_factory
        self.port = None
        self.runner = None
        self.lines = []
        self.last = None
        self._build()
        self.timer = QTimer(self)
        self.timer.setInterval(AUTO_SECONDS * 1000)
        self.timer.timeout.connect(self._auto)
        port_state.changed.connect(self.set_port)
        self.set_port(port_state.port)

    def _build(self):
        outer = QVBoxLayout(self)
        t = QLabel('Telsizin durumu')
        bigger(t, 8, True)
        outer.addWidget(t)
        outer.addWidget(plain_label('Telsiz açık ve kablo takılıyken okuyun. Sadece okur, telsize bir şey yazmaz.'))
        outer.addWidget(PortPicker(self.port_state))
        row = QHBoxLayout()
        self.read_btn = big_button('Durumu oku', 'SP_MessageBoxInformation',
                                   'Telsizin bellek, hata kaydı ve açılış bilgisini bir kez okur.')
        self.read_btn.clicked.connect(self.read_once)
        row.addWidget(self.read_btn, 2)
        self.auto = QCheckBox(f'her {AUTO_SECONDS} sn oku')
        self.auto.setToolTip('Açıkken her 30 saniyede bir okur. Varsayılan: kapalı.')
        self.auto.setChecked(False)
        self.auto.toggled.connect(self._auto_toggled)
        row.addWidget(self.auto, 1)
        outer.addLayout(row)

        card = QFrame()
        card.setFrameShape(QFrame.StyledPanel)
        cl = QHBoxLayout(card)
        self.light = QLabel()
        self.light.setFixedSize(56, 56)
        cl.addWidget(self.light, 0, Qt.AlignTop)
        right = QVBoxLayout()
        self.verdict_label = QLabel()
        bigger(self.verdict_label, 10, True)
        self.summary = plain_label('', 3)
        self.when = QLabel('')
        right.addWidget(self.verdict_label)
        right.addWidget(self.summary)
        right.addWidget(self.when)
        cl.addLayout(right, 1)
        outer.addWidget(card)
        self._set_light('grey')

        self.details = Details()
        box = QWidget()
        f = QFormLayout(box)
        self.v_uptime, self.v_heap, self.v_heap_min = QLabel('-'), QLabel('-'), QLabel('-')
        self.v_min_task, self.v_crash, self.v_reset = QLabel('-'), QLabel('-'), QLabel('-')
        self.v_crash.setWordWrap(True)
        f.addRow('Çalışma süresi:', self.v_uptime)
        f.addRow('Heap boş:', self.v_heap)
        f.addRow('Heap en az boş:', self.v_heap_min)
        f.addRow('En az boş yığın:', self.v_min_task)
        f.addRow('Çökme kaydı:', self.v_crash)
        f.addRow('Reset nedeni:', self.v_reset)
        self.details.add(box)
        self.table = QTableWidget(0, 3)
        self.table.setHorizontalHeaderLabels(['Görev', 'Boş yığın', ''])
        self.table.horizontalHeader().setSectionResizeMode(2, QHeaderView.Stretch)
        self.table.verticalHeader().setVisible(False)
        self.table.setEditTriggers(QTableWidget.NoEditTriggers)
        self.table.setMinimumHeight(160)
        self.details.add(self.table, 1)
        self.log = LogPane()
        self.details.add(self.log, 1)
        outer.addWidget(self.details, 1)
        outer.addStretch(0)

    def _set_light(self, key):
        color, text = LIGHTS[key]
        self.light.setStyleSheet(f'background: {color}; border-radius: 28px;')
        self.verdict_label.setText(text)
        self.verdict_label.setStyleSheet(f'color: {color};')
        self.verdict = key

    def set_port(self, port):
        self.port = port
        self.refresh()

    def refresh(self):
        busy = self.runner is not None
        self.read_btn.setEnabled(bool(self.port) and not busy and self.lock.free_for('health'))
        self.read_btn.setText('Okunuyor...' if busy else 'Durumu oku')

    def _auto_toggled(self, on):
        if on:
            self.timer.start()
        else:
            self.timer.stop()

    def _auto(self):
        if self.runner is None and self.lock.owner is None and self.lock.allowed() and self.port:
            self.read_once()

    def read_once(self):
        if not self.port or self.runner is not None or not self.lock.take('health'):
            return
        self.lines = []
        self.log.clear()
        self.port_state.set_locked(True)
        self.runner = self.runner_factory(self)
        self.runner.line.connect(self._line)
        self.runner.finished.connect(self._finished)
        self.refresh()
        program, argv, shown = self.layout_.tool_command('plus_txdiag', ['--port', self.port, '--health'])
        self.log.add('$ ' + shown)
        self.runner.start(program, argv, cwd=self.root)

    def _line(self, line):
        self.lines.append(line)
        self.log.add(line)

    def _finished(self, code):
        self.lock.release('health')
        self.port_state.set_locked(False)
        self.runner = None
        h = parsers.parse_health('\n'.join(self.lines))
        s = parsers.health_summary(h)
        stamp = time.strftime('%H:%M:%S')
        if code != 0 or not s['ok']:
            self._set_light('grey')
            self.verdict_label.setText('Okunamadı')
            self.summary.setText('Telsiz açık mı, kablo takılı mı? DDM32 Plus çalışırken tekrar deneyin.')
            err = parsers.error_text(self.lines) or f'exit code {code}'
            self.when.setText(f'{stamp}: {err}')
        else:
            self.when.setText(f'Son okuma: {stamp}')
            self.show_summary(s)
        self.refresh()

    def show_summary(self, s):
        self.last = s
        self._set_light(verdict(s))
        self.summary.setText('<br>'.join(plain_lines(s)))
        self.v_uptime.setText(parsers.format_uptime(s['uptime_ms']))
        self.v_heap.setText(_bytes(s['heap_free']))
        self.v_heap_min.setText(_bytes(s['heap_min_free']))
        self.v_min_task.setText(f'{s["min_task"]}: {_bytes(s["min_free"])}' if s['min_task'] else '-')
        if s['crash_valid']:
            self.v_crash.setText(f'VAR - görev {s["crash_task"]}, pc {s["crash_pc"]}, '
                                 f'çalışma {parsers.format_uptime(s["crash_uptime_ms"])}')
        elif s['crash_built'] == '0':
            self.v_crash.setText('Bu build kaydetmiyor')
        else:
            self.v_crash.setText('Kayıt yok')
        self.v_reset.setText(s['reset_cause'] or '-')
        tasks = s['tasks']
        top = max([v for v in tasks.values() if v] + [1024])
        self.table.setRowCount(len(tasks))
        for i, (name, free) in enumerate(tasks.items()):
            self.table.setItem(i, 0, QTableWidgetItem(name))
            self.table.setItem(i, 1, QTableWidgetItem(_bytes(free)))
            bar = QProgressBar()
            bar.setRange(0, top)
            bar.setValue(free or 0)
            bar.setTextVisible(False)
            if free is not None and free < LOW_STACK:
                bar.setStyleSheet('QProgressBar::chunk { background: #d9534f; }')
            self.table.setCellWidget(i, 2, bar)


def _bytes(v):
    return '-' if v is None else f'{v:,} B'.replace(',', '.')
