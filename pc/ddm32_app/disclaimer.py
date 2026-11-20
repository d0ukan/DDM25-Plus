"""The disclaimer: shown in full at the first start (Turkish, English on request), accepted by ticking
"Okudum, anladım ve kabul ediyorum". No radio button works before that (RadioLock's gate). The acceptance is stored with
the text's version (a hash of both texts), so a changed text asks again. Every flash confirmation repeats SHORT."""
import hashlib

from . import paths

TITLE_TR = 'SORUMLULUK REDDİ — OKUMADAN KULLANMAYIN'
TITLE_EN = 'DISCLAIMER — READ BEFORE USING'

TEXT_TR = """
<p><b>DDM32 Plus</b> <b>resmî olmayan, deneysel</b> bir firmware ve programlama aracıdır. Baofeng, RadioFoundry veya
Open25-DM32 ile bağlantılı değildir ve onlar tarafından onaylanmamıştır.</p>
<ul>
<li>Telsizinize firmware yüklemek (flashlamak) <b>telsizinizi kalıcı olarak kullanılamaz hâle getirebilir</b>,
kalibrasyon verilerini, kanal kayıtlarını ve garantinizi kaybettirebilir.</li>
<li>Bu yazılım <b>"OLDUĞU GİBİ"</b>, hiçbir açık veya örtülü garanti olmaksızın sunulur. Çalışacağı, hatasız olduğu
veya belirli bir amaca uygun olduğu garanti <b>edilmez</b>.</li>
<li>Yükleme, kurulum ve kullanımın <b>tüm riski ve sorumluluğu size aittir.</b> Geliştiriciler (d0ukan / TA2DOU) ve
katkıda bulunanlar; telsizde, bilgisayarda veya başka ekipmanda oluşabilecek <b>hiçbir arıza, hasar, veri kaybı,
doğrudan veya dolaylı zarardan sorumlu tutulamaz.</b></li>
<li>Yayın yapmak (TX) yalnızca <b>geçerli bir lisansla ve izinli frekanslarda</b> yasaldır. Telsiz mevzuatına uymak
tamamen kullanıcının sorumluluğundadır. Acil durum ve can güvenliği haberleşmesi için bu firmware'e
<b>güvenmeyin.</b></li>
<li>Yüklemeden önce mutlaka <b>tam yedek</b> alın ve yedeği güvenli bir yerde saklayın.</li>
</ul>
<p><b>Bu yazılımı indirerek, yükleyerek veya kullanarak bu koşulları okuduğunuzu, anladığınızı ve kabul ettiğinizi
beyan etmiş olursunuz. Kabul etmiyorsanız kullanmayın.</b></p>
"""

TEXT_EN = """
<p><b>DDM32 Plus</b> is <b>unofficial, experimental</b> firmware and programming software. It is not affiliated with
or endorsed by Baofeng, RadioFoundry or Open25-DM32.</p>
<ul>
<li>Flashing firmware <b>can permanently brick your radio</b>, erase calibration and channel data, and void your
warranty.</li>
<li>This software is provided <b>"AS IS", without warranty of any kind</b>, express or implied, including
merchantability or fitness for a particular purpose.</li>
<li><b>You flash, install and use it entirely at your own risk and responsibility.</b> The authors (d0ukan / TA2DOU)
and contributors accept <b>no liability for any malfunction, damage, data loss, or direct or indirect loss</b> to
radios, computers or any other equipment.</li>
<li>Transmitting is legal <b>only with a valid licence on authorised frequencies</b>; complying with radio
regulations is solely your responsibility. <b>Do not rely</b> on this firmware for emergency or safety-of-life
communication.</li>
<li>Always make and keep a <b>full backup</b> before flashing.</li>
</ul>
<p><b>By downloading, installing or using this software you confirm that you have read, understood and accepted
these terms. If you do not accept them, do not use it.</b></p>
"""

ACCEPT_TEXT = 'Okudum, anladım ve kabul ediyorum'
SHORT = 'Sorumluluk size aittir. Yedeğiniz hazır mı?'
VERSION = hashlib.sha256((TITLE_TR + TEXT_TR + TITLE_EN + TEXT_EN).encode('utf-8')).hexdigest()[:16]
SETTING = 'disclaimer'


def accepted(settings=None):
    s = paths.load_settings() if settings is None else settings
    rec = s.get(SETTING)
    return isinstance(rec, dict) and rec.get('version') == VERSION and rec.get('accepted') is True


def accept():
    paths.save_setting(SETTING, {'version': VERSION, 'accepted': True, 'accepted_utc': paths.now_utc()})


def withdraw():
    paths.save_setting(SETTING, {'version': VERSION, 'accepted': False, 'withdrawn_utc': paths.now_utc()})


def html(lang='tr'):
    if lang == 'en':
        return f'<h2>⚠ {TITLE_EN}</h2>{TEXT_EN}'
    return f'<h2>⚠ {TITLE_TR}</h2>{TEXT_TR}'


def make_dialog(parent=None):
    """The must-accept dialog (Qt imported here only)"""
    from PyQt5.QtWidgets import (QCheckBox, QDialog, QDialogButtonBox, QHBoxLayout, QPushButton, QTextBrowser,
                                 QVBoxLayout)

    class DisclaimerDialog(QDialog):
        def __init__(self, parent=None):
            super().__init__(parent)
            self.setWindowTitle('DDM32 Plus - Sorumluluk reddi / Disclaimer')
            self.lang = 'tr'
            lay = QVBoxLayout(self)
            top = QHBoxLayout()
            self.lang_btn = QPushButton('English')
            self.lang_btn.setToolTip('Show the disclaimer in English / Türkçe göster')
            self.lang_btn.clicked.connect(self.toggle_lang)
            top.addStretch(1)
            top.addWidget(self.lang_btn)
            lay.addLayout(top)
            self.text = QTextBrowser()
            self.text.setHtml(html('tr'))
            lay.addWidget(self.text, 1)
            self.check = QCheckBox(ACCEPT_TEXT)
            f = self.check.font()
            f.setBold(True)
            f.setPointSize(f.pointSize() + 2)
            self.check.setFont(f)
            lay.addWidget(self.check)
            self.buttons = QDialogButtonBox()
            self.accept_btn = self.buttons.addButton('Kabul ediyorum', QDialogButtonBox.AcceptRole)
            self.reject_btn = self.buttons.addButton('Kabul etmiyorum', QDialogButtonBox.RejectRole)
            self.accept_btn.setEnabled(False)
            self.check.toggled.connect(self.accept_btn.setEnabled)
            self.buttons.accepted.connect(self._accept)
            self.buttons.rejected.connect(self.reject)
            lay.addWidget(self.buttons)
            self.resize(720, 640)

        def toggle_lang(self):
            self.lang = 'en' if self.lang == 'tr' else 'tr'
            self.text.setHtml(html(self.lang))
            self.lang_btn.setText('Türkçe' if self.lang == 'en' else 'English')

        def _accept(self):
            if self.check.isChecked():
                self.accept()

    return DisclaimerDialog(parent)


def ask(parent=None):
    """Shows the dialog; True (and stored) only when the box was ticked and accepted"""
    d = make_dialog(parent)
    ok = d.exec_() == d.Accepted and d.check.isChecked()
    if ok:
        accept()
    return ok
