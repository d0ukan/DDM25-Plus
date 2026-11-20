"""Small shared widgets: the raw-output log pane, the 'switch the radio on now' banner with its countdown."""
import time

from PyQt5.QtCore import Qt, QTimer, pyqtSignal
from PyQt5.QtGui import QFont, QFontDatabase
from PyQt5.QtWidgets import (QDialog, QDialogButtonBox, QFrame, QLabel, QPlainTextEdit, QPushButton, QSizePolicy,
                             QStyle, QTextBrowser, QToolButton, QVBoxLayout, QWidget)


class LogPane(QPlainTextEdit):
    """Raw tool output; optionally mirrored into a log file"""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setReadOnly(True)
        self.setMaximumBlockCount(20000)
        self.setFont(QFontDatabase.systemFont(QFontDatabase.FixedFont))
        self.setPlaceholderText('Araç çıktısı burada görünür.')
        self._file = None

    def open_file(self, path, header):
        self.close_file()
        path.parent.mkdir(parents=True, exist_ok=True)
        self._file = open(path, 'a', encoding='utf-8')
        self._file.write(f'# {time.strftime("%Y-%m-%d %H:%M:%S")} {header}\n')
        self._file.flush()

    def close_file(self):
        if self._file:
            self._file.close()
            self._file = None

    def add(self, line):
        self.appendPlainText(line)
        if self._file:
            self._file.write(line + '\n')
            self._file.flush()


class RadioOnBanner(QFrame):
    """'Telsizi şimdi kapatıp açın' with a countdown, shown while a tool waits for the radio's boot ROM"""
    expired = pyqtSignal()

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setFrameShape(QFrame.StyledPanel)
        self.setObjectName('radioOnBanner')
        self.setStyleSheet('#radioOnBanner { background: #f6c343; border-radius: 6px; } '
                           '#radioOnBanner QLabel { color: #1d1d1d; }')
        lay = QVBoxLayout(self)
        self.title = QLabel('Telsizi şimdi kapatıp açın')
        f = QFont(self.font())
        f.setPointSize(f.pointSize() + 12)
        f.setBold(True)
        self.title.setFont(f)
        self.title.setAlignment(Qt.AlignCenter)
        self.count = QLabel('')
        f2 = QFont(self.font())
        f2.setPointSize(f2.pointSize() + 6)
        self.count.setFont(f2)
        self.count.setAlignment(Qt.AlignCenter)
        self.hint = QLabel('Telsiz açıksa önce kapatın, sonra açın; tuşlara basmayın. Ekran normal açılırsa bir kez daha kapatıp açın.')
        self.hint.setAlignment(Qt.AlignCenter)
        lay.addWidget(self.title)
        lay.addWidget(self.count)
        lay.addWidget(self.hint)
        self.remaining = 0
        self.timer = QTimer(self)
        self.timer.setInterval(1000)
        self.timer.timeout.connect(self._tick)
        self.hide()

    def start(self, seconds):
        self.remaining = int(seconds)
        self._show_count()
        self.show()
        self.timer.start()

    def stop(self):
        self.timer.stop()
        self.hide()

    def _tick(self):
        self.remaining = max(0, self.remaining - 1)
        self._show_count()
        if self.remaining == 0:
            self.timer.stop()
            self.expired.emit()

    def _show_count(self):
        self.count.setText(f'Kalan süre: {self.remaining // 60}:{self.remaining % 60:02d}')


def bigger(widget, delta=4, bold=False):
    f = QFont(widget.font())
    f.setPointSize(max(8, f.pointSize() + delta))
    f.setBold(bold)
    widget.setFont(f)
    return widget


def icon(widget, name):
    """A Qt standard icon by its QStyle name ('SP_DialogHelpButton')"""
    return widget.style().standardIcon(getattr(QStyle, name))


def big_button(text, icon_name=None, tooltip='', parent=None):
    b = QPushButton(text, parent)
    bigger(b, 4, True)
    b.setMinimumHeight(52)
    b.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)
    if icon_name:
        b.setIcon(icon(b, icon_name))
        b.setIconSize(b.iconSize() * 1.6)
    if tooltip:
        b.setToolTip(tooltip)
    return b


def plain_label(text='', delta=2, wrap=True):
    l = QLabel(text)
    bigger(l, delta)
    l.setWordWrap(wrap)
    l.setTextFormat(Qt.RichText)
    return l


class Details(QWidget):
    """An 'Ayrıntılar' expander: closed by default, holds the technical part"""

    def __init__(self, title='Ayrıntılar', parent=None):
        super().__init__(parent)
        lay = QVBoxLayout(self)
        lay.setContentsMargins(0, 0, 0, 0)
        self.toggle = QToolButton()
        self.toggle.setText(title)
        self.toggle.setCheckable(True)
        self.toggle.setToolButtonStyle(Qt.ToolButtonTextBesideIcon)
        self.toggle.setArrowType(Qt.RightArrow)
        self.toggle.setStyleSheet('QToolButton { border: none; }')
        self.toggle.toggled.connect(self.set_open)
        self.body = QWidget()
        self.inner = QVBoxLayout(self.body)
        self.inner.setContentsMargins(16, 0, 0, 0)
        self.body.hide()
        lay.addWidget(self.toggle, 0, Qt.AlignLeft)
        lay.addWidget(self.body, 1)

    def add(self, widget, stretch=0):
        self.inner.addWidget(widget, stretch)
        return widget

    def set_open(self, on):
        self.toggle.blockSignals(True)
        self.toggle.setChecked(on)
        self.toggle.blockSignals(False)
        self.toggle.setArrowType(Qt.DownArrow if on else Qt.RightArrow)
        self.body.setVisible(on)

    def is_open(self):
        return not self.body.isHidden()


HELP_HTML = """
<h2>Yardım</h2>
<h3>1. Telsiz yakalanamadı (zaman aşımı)</h3>
<p>Araç telsizin açılmasını bekler. Telsizi kapatın, kabloyu takılı bırakın, <b>Tekrar dene</b>'ye basın ve sarı
<b>"Telsizi şimdi kapatıp açın"</b> yazısını görünce telsiz açıksa kapatın, sonra açın. Telsiz normal ekranla açılırsa kapatıp bir daha açın.
Tuşlara basmayın.</p>
<h3>2. Kablo bulunamadı</h3>
<p>Programlama kablosunu bilgisayara ve telsize sıkıca takın, <b>Yenile</b>'ye basın. DM-32UV kablosu genelde
"USB-SERIAL CH340" olarak görünür (program bunu kendisi seçer). Görünmüyorsa CH340 sürücüsünü kurun. Sanal portlar
(com0com, Bluetooth) telsiz değildir ve seçilmez.</p>
<h3>3. Yedek eski</h3>
<p>Yükleme, telsizin <b>şu anki</b> yedeğiyle yapılmalı. Telsiz o yedekten sonra değiştiyse (başka bir yükleme,
CPS ile yazma) yükleme aracı bunu fark eder ve hiçbir şey yazmadan durur. Yeni bir yedek alın.</p>
<h3>4. Sürüm kurulamıyor</h3>
<p>Program yalnızca <b>imzalı</b> DDM32 Plus sürümlerini kurar: sürüm listesinin imzası programdaki anahtarla
doğrulanmalı ve indirilen dosyanın SHA-256 özeti listedekiyle aynı olmalı. İmzası geçmeyen sürümler listede görünmez
("Sürümler" sayfasında Ayrıntılar).</p>
<h3>5. Stok yazılımdan ilk kurulum</h3>
<p>Telsizde stok yazılım varsa program bunu yedekten anlar ve telsizin kendi DMR codec'ini yedeğinizden alıp ayrılmış
codec bölümüne hazırlar. DDM32 Plus yalnızca stok .047 üzerinde denendi; diğer desteklenen stok sürümlerde (.032 -
.049) uyarı gösterilir. Tanınmayan sürümlerde (örneğin .050) önce Open25-DM32 Classic'i kendi Flasher'ıyla kurun,
sonra DDM32 Plus'ı onun üzerine kurun.</p>
<h3>6. Kurulum yarıda kaldı</h3>
<p>Program asla kendiliğinden tekrar denemez. "Hiçbir şey yazılmadı" yazıyorsa telsiz zamanında açılmamıştır:
Kontrol ve Yükle adımlarını yeniden yapabilirsiniz. "Yazma başladıktan sonra durdu" yazıyorsa telsizi kapatmayın,
kabloyu çekmeyin ve <b>Geri dön</b> sayfasından yüklemeden önce alınan yedekle geri yükleyin. Bootloader hiç
yazılmadığı için telsiz yeniden yüklenebilir durumda kalır.</p>
<h3>7. Yedekler nerede?</h3>
<p>%LOCALAPPDATA%\\DDM32Plus\\yedekler klasöründe. Bu klasörü <b>silmeyin ve kimseyle paylaşmayın</b>: telsizinizin
kalibrasyonu ve kanalları içindedir. Program güncellemeleri bu klasöre dokunmaz.</p>
"""


def show_help(parent):
    d = QDialog(parent)
    d.setWindowTitle('DDM32 Plus - Yardım')
    lay = QVBoxLayout(d)
    t = QTextBrowser()
    t.setHtml(HELP_HTML)
    bigger(t, 1)
    lay.addWidget(t)
    bb = QDialogButtonBox(QDialogButtonBox.Close)
    bb.rejected.connect(d.reject)
    bb.button(QDialogButtonBox.Close).setText('Kapat')
    lay.addWidget(bb)
    d.resize(680, 600)
    d.exec_()


def about_html():
    from . import AUTHOR, REPO_URL, __version__
    return f"""
<h2>DDM32 Plus</h2>
<p><b>DDM32 Plus · {AUTHOR}</b><br>Program sürümü {__version__}<br>
<a href="{REPO_URL}">{REPO_URL}</a></p>
<p>Baofeng DM-32UV için resmî olmayan, deneysel firmware ve programlayıcı. Lisans: GPL-3.0-or-later.</p>
<h3>Teşekkürler / Credits</h3>
<ul>
<li><b>RadioFoundry</b> (MIT): açık firmware tabanı, RAM izleyici, codec bölümü araçları -
<a href="https://github.com/RadioFoundry/RadioFoundry">github.com/RadioFoundry/RadioFoundry</a></li>
<li><b>Open25-DM32 Classic</b> (MIT, tylerwatt12): P25, IMBE, native modem, Classic düzeni -
<a href="https://github.com/tylerwatt12/Open25-DM32">github.com/tylerwatt12/Open25-DM32</a></li>
<li>mbelib (ISC), OP25 ve diğer kaynaklar: depodaki THIRD_PARTY.md</li>
<li>Ed25519 imza denetimi: RFC 8032 başvuru kodu</li>
</ul>
<p>Baofeng, RadioFoundry veya Open25-DM32 ile bağlantılı değildir. Telsizin kendi DMR codec'i ve stok yazılım bu
programla dağıtılmaz.</p>
"""


def show_about(parent):
    d = QDialog(parent)
    d.setWindowTitle('DDM32 Plus - Hakkında')
    lay = QVBoxLayout(d)
    t = QTextBrowser()
    t.setOpenExternalLinks(True)
    t.setHtml(about_html())
    lay.addWidget(t)
    bb = QDialogButtonBox(QDialogButtonBox.Close)
    bb.rejected.connect(d.reject)
    bb.button(QDialogButtonBox.Close).setText('Kapat')
    lay.addWidget(bb)
    d.resize(560, 480)
    d.exec_()
