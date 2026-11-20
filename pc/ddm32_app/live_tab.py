"""Canlı panel (read only): signal, status and the P25 zone scan counters, polled by radiotools/plus_live.py.

The "Bağlan" click is the user's approval to open the port for this panel; the reader runs as its own subprocess
(same model as every other radio step) and sends only the read-only queries in its allowlist. "Durdur" ends it.
"""
import time

from PyQt5.QtCore import QPointF, Qt
from PyQt5.QtGui import QColor, QPainter, QPen, QPolygonF
from PyQt5.QtWidgets import (QComboBox, QFormLayout, QGridLayout, QGroupBox, QHBoxLayout, QLabel, QProgressBar,
                             QSizePolicy, QVBoxLayout, QWidget)

from . import parsers
from .health_tab import RESET_NAMES
from .portstate import PortPicker
from .runner import ProcessRunner
from .widgets import Details, LogPane, big_button, bigger, plain_label

INTERVALS = (1, 2, 3, 4, 5)
DEFAULT_INTERVAL = 2
DBM_LOW, DBM_HIGH = -140, -40
HISTORY = 90
STATES = {'idle': 'Boşta', 'rx': 'Alıyor', 'tx': 'Gönderiyor', 'tx_denied': 'Gönderme engellendi'}
NATIVE_STATES = {'idle': 'boşta', 'armed': 'hazır', 'receive': 'alıyor', 'transmit': 'gönderiyor',
                 'draining': 'bitiriyor', 'stopped': 'durdu', 'fault': 'hata'}
SCAN_LABELS = (('stops', 'Duraklama'), ('carriers', 'Taşıyıcı vardı'), ('cooled', 'Beklemede atlandı'),
               ('takes', 'P25 alıcısı aldı'), ('synced', 'Çerçeve duyuldu'), ('voiced', 'Ses duyuldu'),
               ('ended', 'Çağrı bitti'), ('tails', 'Kuyruk'), ('quiet', 'Çerçeve yok'),
               ('foreign', 'Bize ait değil'), ('sync_ms', 'İlk çerçeve (ms)'), ('voice_ms', 'İlk ses (ms)'))
GREEN, RED, GREY = '#2e7d32', '#c62828', '#666666'


class RssiHistory(QWidget):
    """The last readings as a line, -140 .. -40 dBm"""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.values = []
        self.setMinimumHeight(80)
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)

    def add(self, dbm):
        self.values.append(dbm)
        del self.values[:-HISTORY]
        self.update()

    def clear(self):
        self.values = []
        self.update()

    def paintEvent(self, event):
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        r = self.rect().adjusted(1, 1, -1, -1)
        p.fillRect(r, self.palette().base())
        p.setPen(QPen(self.palette().mid().color()))
        p.drawRect(r)
        for dbm in (-120, -93, -60):          # guide lines; -93 dBm is S9
            y = self._y(dbm, r)
            p.drawLine(r.left(), int(y), r.right(), int(y))
            p.drawText(r.left() + 3, int(y) - 2, f'{dbm}' + (' (S9)' if dbm == -93 else ''))
        pts = [(i, v) for i, v in enumerate(self.values) if v is not None]
        if len(pts) >= 2:
            step = r.width() / max(1, HISTORY - 1)
            x0 = r.right() - (len(self.values) - 1) * step
            poly = QPolygonF([QPointF(x0 + i * step, self._y(v, r)) for i, v in pts])
            p.setPen(QPen(QColor('#1565c0'), 2))
            p.drawPolyline(poly)
        p.end()

    @staticmethod
    def _y(dbm, r):
        f = (min(DBM_HIGH, max(DBM_LOW, dbm)) - DBM_LOW) / (DBM_HIGH - DBM_LOW)
        return r.bottom() - f * r.height()


def dbm_percent(dbm):
    if dbm is None:
        return 0
    return int(round((min(DBM_HIGH, max(DBM_LOW, dbm)) - DBM_LOW) * 100 / (DBM_HIGH - DBM_LOW)))


class LiveTab(QWidget):
    def __init__(self, layout, lock, port_state, runner_factory=ProcessRunner, parent=None):
        super().__init__(parent)
        self.layout_ = layout
        self.root = layout.root
        self.lock = lock
        self.port_state = port_state
        self.runner_factory = runner_factory
        self.port = None
        self.runner = None
        self.stopping = False
        self.lines = []
        self.once = []          # crash / reset, read before the first poll
        self.block = None       # the poll being read
        self.polls = 0
        self.last = None
        self._build()
        port_state.changed.connect(self.set_port)
        self.set_port(port_state.port)

    # --- layout -------------------------------------------------------------------------------------------------
    def _build(self):
        outer = QVBoxLayout(self)
        t = QLabel('Canlı panel')
        bigger(t, 8, True)
        outer.addWidget(t)
        outer.addWidget(plain_label('Telsiz açık ve kablo takılıyken <b>Bağlan</b>\'a basın: port açılır ve telsiz '
                                    'birkaç saniyede bir okunur. Sadece okur; ayar değiştirmez, yayın yapmaz.'))
        outer.addWidget(PortPicker(self.port_state))
        row = QHBoxLayout()
        self.connect_btn = big_button('Bağlan', 'SP_MediaPlay', 'Portu açar ve telsizi okumaya başlar (sadece okuma).')
        self.connect_btn.clicked.connect(self.start)
        row.addWidget(self.connect_btn, 2)
        row.addWidget(QLabel('Yenileme:'))
        self.interval = QComboBox()
        for s in INTERVALS:
            self.interval.addItem(f'{s} sn', s)
        self.interval.setCurrentIndex(INTERVALS.index(DEFAULT_INTERVAL))
        self.interval.setToolTip('Kaç saniyede bir okunacağı. Değiştirmek için önce Durdur.')
        row.addWidget(self.interval)
        self.stop_btn = big_button('Durdur', 'SP_MediaStop', 'Okumayı bitirir ve portu kapatır.')
        self.stop_btn.clicked.connect(self.stop)
        row.addWidget(self.stop_btn, 1)
        outer.addLayout(row)
        self.status = plain_label('Bağlı değil.')
        outer.addWidget(self.status)

        cards = QHBoxLayout()
        sig = QGroupBox('Sinyal')
        sl = QVBoxLayout(sig)
        self.rssi_bar = QProgressBar()
        self.rssi_bar.setRange(0, 100)
        self.rssi_bar.setFormat('-')
        self.rssi_bar.setTextVisible(True)
        bigger(self.rssi_bar, 3, True)
        self.rssi_bar.setMinimumHeight(34)
        sl.addWidget(self.rssi_bar)
        self.history = RssiHistory()
        sl.addWidget(self.history)
        f = QFormLayout()
        self.v_state, self.v_mode, self.v_channel, self.v_freq, self.v_noise = (QLabel('-') for _ in range(5))
        for w in (self.v_state, self.v_mode, self.v_channel, self.v_freq):
            bigger(w, 2)
        self.v_channel.setWordWrap(True)
        f.addRow('Durum:', self.v_state)
        f.addRow('Mod:', self.v_mode)
        f.addRow('Kanal:', self.v_channel)
        f.addRow('Frekans:', self.v_freq)
        f.addRow('Gürültü:', self.v_noise)
        sl.addLayout(f)
        cards.addWidget(sig, 3)

        st = QGroupBox('Durum')
        fl = QFormLayout(st)
        self.v_native, self.v_sync, self.v_uptime, self.v_heap, self.v_stack, self.v_reset, self.v_crash = (
            QLabel('-') for _ in range(7))
        for w in (self.v_native, self.v_crash):
            w.setWordWrap(True)
        fl.addRow('P25 alıcısı:', self.v_native)
        fl.addRow('P25 senkron:', self.v_sync)
        fl.addRow('Açık kalma:', self.v_uptime)
        fl.addRow('Boş bellek (en düşük):', self.v_heap)
        fl.addRow('En az boş yığın:', self.v_stack)
        fl.addRow('Son açılış:', self.v_reset)
        fl.addRow('Hata kaydı:', self.v_crash)
        cards.addWidget(st, 2)
        outer.addLayout(cards)

        sc = QGroupBox('P25 bölge tarama sayaçları')
        grid = QGridLayout(sc)
        self.scan_labels = {}
        for i, (key, text) in enumerate(SCAN_LABELS):
            value = QLabel('-')
            bigger(value, 2, True)
            grid.addWidget(QLabel(text + ':'), i // 4, (i % 4) * 2)
            grid.addWidget(value, i // 4, (i % 4) * 2 + 1)
            self.scan_labels[key] = value
        self.scan_note = QLabel('')
        self.scan_note.setWordWrap(True)
        grid.addWidget(self.scan_note, 3, 0, 1, 8)
        outer.addWidget(sc)

        self.details = Details()
        self.log = LogPane()
        self.details.add(self.log, 1)
        outer.addWidget(self.details, 1)
        outer.addStretch(0)
        self.refresh()

    # --- state --------------------------------------------------------------------------------------------------
    def set_port(self, port):
        if self.runner is None:
            self.port = port
        self.refresh()

    def running(self):
        return self.runner is not None

    def refresh(self):
        busy = self.runner is not None
        self.connect_btn.setEnabled(bool(self.port) and not busy and self.lock.free_for('live'))
        self.stop_btn.setEnabled(busy and not self.stopping)
        self.interval.setEnabled(not busy)

    def seconds(self):
        return self.interval.currentData() or DEFAULT_INTERVAL

    # --- reader process -----------------------------------------------------------------------------------------
    def start(self):
        """The user's click: open the port with the read-only reader"""
        if not self.port or self.runner is not None or not self.lock.take('live'):
            return
        self.lines, self.once, self.block, self.polls = [], [], None, 0
        self.stopping = False
        self.history.clear()
        self.log.clear()
        self.port_state.set_locked(True)
        self.runner = self.runner_factory(self)
        self.runner.line.connect(self._line)
        self.runner.finished.connect(self._finished)
        program, argv, shown = self.layout_.tool_command('plus_live', ['--port', self.port,
                                                                       '--interval', str(self.seconds())])
        self.log.add('$ ' + shown)
        self.status.setText(f'Bağlanılıyor ({self.port})...')
        self.refresh()
        self.runner.start(program, argv, cwd=self.root)

    def stop(self):
        if self.runner is not None:
            self.stopping = True
            self.log.add('# Durduruldu.')
            self.runner.kill()
            self.refresh()

    def _line(self, line):
        self.lines.append(line)
        self.log.add(line)
        s = line.strip()
        m = parsers.RE_POLL.match(s)
        if m and m.group(1) == 'poll':
            self.block = []
            return
        if m and m.group(1) == 'end':
            if self.block is not None:
                self.polls += 1
                self.show_poll(parsers.parse_blocks(self.block))
            self.block = None
            return
        if self.block is not None:
            self.block.append(s)
        else:
            self.once.append(s)

    def _finished(self, code):
        self.lock.release('live')
        self.port_state.set_locked(False)
        self.runner = None
        stamp = time.strftime('%H:%M:%S')
        if self.stopping or code == 0:
            self.status.setText(f'Durduruldu ({stamp}). Port kapalı.')
        else:
            err = parsers.error_text(self.lines) or next(
                (l for l in reversed(self.lines) if l.startswith('No answer')), '') or f'exit code {code}'
            self.status.setText(_color('Bağlantı kesildi.', RED) + ' Telsiz açık mı, kablo takılı mı, DDM32 Plus '
                                'çalışıyor mu?<br><small>' + _esc(err) + '</small>')
        self.stopping = False
        self.refresh()

    # --- showing a poll -----------------------------------------------------------------------------------------
    def show_poll(self, blocks):
        s = parsers.live_summary(blocks, parsers.parse_blocks(self.once))
        self.last = s
        self.status.setText(_color('Bağlı', GREEN) + f' ({self.port}), her {self.seconds()} sn. '
                            f'Son okuma: {time.strftime("%H:%M:%S")}')
        sig = s['signal']
        dbm = sig['rssi_dbm']
        self.history.add(dbm)
        self.rssi_bar.setValue(dbm_percent(dbm))
        meter = parsers.s_meter(dbm)
        self.rssi_bar.setFormat(f'{dbm} dBm   {meter}' if dbm is not None else 'Sinyal yok')
        if not sig['ok']:
            for w in (self.v_state, self.v_mode, self.v_channel, self.v_freq, self.v_noise):
                w.setText('-')
        else:
            self.v_state.setText(STATES.get(sig['state'], sig['state'] or '-') +
                                 (' (monitör açık)' if sig['monitor'] else ''))
            self.v_mode.setText((sig['mode'] or '-').upper())
            if sig['vfo']:
                ch = 'VFO'
            else:
                ch = ' · '.join(x for x in (str(sig['channel_number'] or ''), sig['channel'] or '') if x) or '-'
                if sig['zone']:
                    ch += f'  (bölge: {sig["zone"]})'
            self.v_channel.setText(ch)
            self.v_freq.setText(parsers.format_mhz(sig['rx_hz']))
            self.v_noise.setText('-' if sig['noise'] is None else str(sig['noise']))
        n = s['native']
        if n['ok']:
            text = NATIVE_STATES.get(n['state'], n['state'] or '-')
            if n['reason'] and n['reason'] != 'ok':
                text += f' (son neden: {n["reason"]})'
            self.v_native.setText(text)
            self.v_sync.setText('-' if n['p25_sync'] is None else str(n['p25_sync']))
        else:
            self.v_native.setText('okunamadı' if n['status'] not in (None, 0) else '-')
            self.v_sync.setText('-')
        h = s['health']
        if h:
            self.v_uptime.setText(parsers.format_uptime(h['uptime_ms']))
            self.v_heap.setText(_bytes(h['heap_min_free']))
            self.v_stack.setText(f'{h["min_task"]}: {_bytes(h["min_free"])}' if h['min_task'] else '-')
            self.v_reset.setText(RESET_NAMES.get(h['reset_cause'], h['reset_cause'] or '-'))
            self.v_crash.setText(_color(f'VAR (görev {h["crash_task"]})', RED) if h['crash_valid'] else 'yok')
        scan = s['scan']
        if scan:
            for key, label in self.scan_labels.items():
                v = scan.get(key)
                label.setText('-' if v is None else str(v))
            self.scan_note.setText('')
        else:
            for label in self.scan_labels.values():
                label.setText('-')
            self.scan_note.setText('Bu sürüm tarama sayaçlarını vermiyor.' if s['scan_status'] not in (None, 0, -1)
                                   else '')


def _color(text, color):
    return f'<span style="color:{color};font-weight:bold;">{text}</span>'


def _esc(text):
    return (text or '').replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')


def _bytes(v):
    return '-' if v is None else f'{v:,} B'.replace(',', '.')
