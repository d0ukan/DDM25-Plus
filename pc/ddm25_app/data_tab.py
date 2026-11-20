"""Verileri güncelle (developer page): the RadioID downloads, the satellite elements (TLE, into the WSL build copy) and
a new firmware build (WSL). Shown only when the program runs from a source checkout of the repository with WSL; the
frozen program hides it."""
import time
from pathlib import Path

from PyQt5.QtCore import Qt, QThread, pyqtSignal
from PyQt5.QtWidgets import (QFormLayout, QGridLayout, QGroupBox, QHBoxLayout, QHeaderView, QLabel, QPushButton,
                             QTableWidget, QTableWidgetItem, QVBoxLayout, QWidget)

from . import dataupdate, parsers, paths
from .runner import ProcessRunner
from .widgets import Details, LogPane, big_button, bigger, plain_label

GREEN, RED, AMBER = '#2e7d32', '#c62828', '#8a6d00'
NAMES = {'user_csv': 'ID listesi', 'rptrs_json': 'Röle listesi'}
TLE_STATUS = {'new': ('Yeni veri var', GREEN), 'same': ('Güncel', None), 'older': ('Güncel', None),
              'missing': ("CelesTrak'ta yok, eski kalır", AMBER)}
STANDALONE_TEXT = ('Bu sayfa sadece geliştirme bilgisayarında (depo klasörü ve WSL ile) çalışır: ID listesi, röle '
                   'listesi ve uydu verisi yeni bir firmware sürümüne derlenerek telsize gider.<br><br>'
                   'Yeni sürümler size hazır bir <b>paket</b> olarak gelir: paketi programın yanındaki '
                   '<b>paketler</b> klasörüne koyun ve "Telsizi güncelle" sayfasını kullanın.')


class Job(QThread):
    done = pyqtSignal(object, str)    # result, error

    def __init__(self, fn, parent=None):
        super().__init__(parent)
        self.fn = fn

    def run(self):
        try:
            self.done.emit(self.fn(), '')
        except Exception as exc:        # shown to the user; nothing is replaced on failure
            self.done.emit(None, f'{exc.__class__.__name__}: {exc}')


class DataTab(QWidget):
    def __init__(self, layout, runner_factory=ProcessRunner, fetch=dataupdate.http_fetch, run_async=True,
                 parent=None):
        super().__init__(parent)
        self.layout_ = layout
        self.root = Path(layout.repo or layout.root)
        self.paths = dataupdate.firmware_paths(self.root)
        self.runner_factory = runner_factory
        self.fetch = fetch
        self.run_async = run_async
        self.jobs = []
        self.runner = None
        self.step = None            # 'build' or 'tle' while a WSL process runs
        self.lines = []
        self.build_result = None
        self.tle = None             # the last TleCheck
        if layout.can_build:
            self._build()
            self.show_files()
        else:
            self._build_standalone()

    @property
    def available(self):
        return self.layout_.can_build

    def _build_standalone(self):
        outer = QVBoxLayout(self)
        t = QLabel('Verileri güncelle')
        bigger(t, 8, True)
        outer.addWidget(t)
        self.standalone_label = plain_label(STANDALONE_TEXT, 3)
        outer.addWidget(self.standalone_label)
        outer.addStretch(1)
        self.log = LogPane()        # closed by the window like the others; never shown
        self.log.hide()

    def _build(self):
        outer = QVBoxLayout(self)
        t = QLabel('Verileri güncelle')
        bigger(t, 8, True)
        outer.addWidget(t)
        outer.addWidget(plain_label('DMR ID listesi, röle listesi ve uydu verisi internetten indirilir. '
                                    'Telsize gitmeleri için ardından yeni bir sürüm derlenir ve yüklenir.'))
        self.summary = plain_label('', 3)
        outer.addWidget(self.summary)
        self.all_btn = big_button('Hepsini güncelle', 'SP_BrowserReload',
                                  'ID ve röle listesini indirir, uydu verisini kontrol eder. Bozuk bir indirme eski '
                                  'dosyanın yerine geçmez.')
        self.all_btn.clicked.connect(self.update_all)
        outer.addWidget(self.all_btn)
        self.data_msg = plain_label()
        outer.addWidget(self.data_msg)

        box = QGroupBox('Uydu verileri (TLE)')
        tl = QVBoxLayout(box)
        self.tle_msg = plain_label('CelesTrak\'ın amatör uydu listesiyle karşılaştırmak için "Kontrol et".', 1)
        tl.addWidget(self.tle_msg)
        self.tle_table = QTableWidget(0, 5)
        self.tle_table.setHorizontalHeaderLabels(['Uydu', 'Katalog', 'Şu anki veri', 'CelesTrak', 'Durum'])
        self.tle_table.horizontalHeader().setSectionResizeMode(QHeaderView.ResizeToContents)
        self.tle_table.horizontalHeader().setStretchLastSection(True)
        self.tle_table.verticalHeader().setVisible(False)
        self.tle_table.setEditTriggers(QTableWidget.NoEditTriggers)
        self.tle_table.setMaximumHeight(240)
        self.tle_table.hide()
        tl.addWidget(self.tle_table)
        row = QHBoxLayout()
        self.tle_check_btn = QPushButton('Kontrol et')
        self.tle_check_btn.setToolTip('CelesTrak\'tan amatör uydu listesini indirir (iki saatte bir en fazla) ve '
                                      'karşılaştırır. Hiçbir dosya değişmez.')
        self.tle_check_btn.clicked.connect(self.check_tle)
        self.tle_write_btn = QPushButton('Derleme kopyasına yaz (WSL)')
        self.tle_write_btn.setToolTip('Yeni tle.txt\'yi sadece WSL\'deki derleme kopyasına yazar (önce firmware\'in '
                                      'kendi aracıyla denetlenir). Windows\'taki firmware klasörü değişmez.')
        self.tle_write_btn.clicked.connect(self.write_tle)
        self.tle_write_btn.setEnabled(False)
        row.addWidget(self.tle_check_btn)
        row.addWidget(self.tle_write_btn)
        row.addStretch(1)
        tl.addLayout(row)
        outer.addWidget(box)

        self.build_btn = big_button('Yeni sürüm derle', 'SP_ComputerIcon',
                                    'WSL içinde DDM25 Plus firmware\'ini (genel profil) yeni verilerle derler (birkaç dakika).')
        self.build_btn.clicked.connect(self.start_build)
        outer.addWidget(self.build_btn)
        self.build_msg = plain_label()
        outer.addWidget(self.build_msg)

        self.details = Details()
        files = QWidget()
        grid = QGridLayout(files)
        grid.addWidget(QLabel('<b>Dosya</b>'), 0, 0)
        grid.addWidget(QLabel('<b>Tarih</b>'), 0, 1)
        grid.addWidget(QLabel('<b>İçerik</b>'), 0, 2)
        self.rows = {}
        specs = (('user_csv', 'RadioID user.csv', 'user.csv indir', self.update_user_csv),
                 ('rptrs_json', 'RadioID rptrs.json', 'rptrs.json indir', self.update_rptrs),
                 ('tle', 'CelesTrak TLE (depodaki tle.txt)', None, None))
        for i, (key, title, button, fn) in enumerate(specs, start=1):
            date, count = QLabel('-'), QLabel('-')
            grid.addWidget(QLabel(title), i, 0)
            grid.addWidget(date, i, 1)
            grid.addWidget(count, i, 2)
            btn = None
            if button:
                btn = QPushButton(button)
                btn.clicked.connect(fn)
                grid.addWidget(btn, i, 3)
            self.rows[key] = (date, count, btn)
        self.v_tle_wsl = QLabel('-')
        grid.addWidget(QLabel('TLE (WSL derleme kopyası)'), 4, 0)
        grid.addWidget(self.v_tle_wsl, 4, 1, 1, 2)
        grid.setColumnStretch(2, 1)
        self.details.add(files)
        form_w = QWidget()
        form = QFormLayout(form_w)
        self.v_ram, self.v_flash, self.v_sha = QLabel('-'), QLabel('-'), QLabel('-')
        self.v_sha.setTextInteractionFlags(Qt.TextSelectableByMouse)
        self.v_identical, self.v_warn, self.v_review, self.v_tle_build = (QLabel('-') for _ in range(4))
        self.v_review.setWordWrap(True)
        form.addRow('RAM:', self.v_ram)
        form.addRow('FLASH:', self.v_flash)
        form.addRow('SHA-256:', self.v_sha)
        form.addRow('Rebuild identical:', self.v_identical)
        form.addRow('Uyarı sayısı:', self.v_warn)
        form.addRow('İmza:', self.v_review)
        form.addRow('Derlemedeki TLE:', self.v_tle_build)
        self.details.add(form_w)
        self.log = LogPane()
        self.details.add(self.log, 1)
        outer.addWidget(self.details, 1)
        outer.addStretch(0)

    # --- files --------------------------------------------------------------------------------------------------
    def wsl_tle(self):
        """What the app last wrote into the WSL copy: {epochs: {catalog: date}, written, sha256}"""
        return paths.load_settings().get('tle_wsl') or {}

    def show_files(self):
        info = dataupdate.file_counts(self.paths)
        u, r, t = info['user_csv'], info['rptrs_json'], info['tle']
        self._set('user_csv', u['date'], f'{_n(u["count"])} kayıt, Türkiye {_n(u.get("country", 0))}'
                  if u['count'] else '-')
        self._set('rptrs_json', r['date'], f'{_n(r["count"])} röle, Türkiye {_n(r.get("country", 0))}'
                  if r['count'] else '-')
        self._set('tle', t['date'], f'{t["count"]} uydu, epoch {t["epoch"].strftime("%d.%m.%Y")}'
                  if t['count'] else '-')
        wsl = self.wsl_tle()
        epochs = [dataupdate._date(v) for v in (wsl.get('epochs') or {}).values()]
        epochs = [e for e in epochs if e]
        if epochs:
            oldest = min(epochs)
            self.v_tle_wsl.setText(f'{wsl.get("written", "?")} yazıldı, epoch {oldest.strftime("%d.%m.%Y")}')
            if t['count'] and t.get('epoch') and oldest > t['epoch']:
                t = dict(t, epoch=oldest)          # the next build takes the newer WSL copy
            elif not t['count']:
                t = {'count': len(epochs), 'epoch': oldest}
        info['tle'] = t
        self.summary.setText(dataupdate.summary_text(info))

    def _set(self, key, date, text):
        self.rows[key][0].setText(date)
        self.rows[key][1].setText(text)

    def _buttons(self, on):
        busy_wsl = self.runner is not None
        self.all_btn.setEnabled(on)
        self.tle_check_btn.setEnabled(on)
        self.tle_write_btn.setEnabled(on and not busy_wsl and self.tle is not None and self.tle.changed)
        for _, _, b in self.rows.values():
            if b is not None:
                b.setEnabled(on)

    def _job(self, fn, finish):
        self._buttons(False)
        self.data_msg.setText('İndiriliyor...')

        def done(result, err):
            self._buttons(True)
            finish(result, err)
            self.show_files()
        if not self.run_async:
            try:
                done(fn(), '')
            except Exception as exc:
                done(None, f'{exc.__class__.__name__}: {exc}')
            return
        job = Job(fn, self)
        job.done.connect(done)
        job.finished.connect(lambda: self.jobs.remove(job))
        self.jobs.append(job)
        job.start()

    def _simple(self, ok_text):
        def finish(result, err):
            self.data_msg.setText(_color('Hata, dosya değişmedi: ', RED) + _esc(err) if err else ok_text(result))
        return finish

    def update_all(self):
        def run():
            errors = dataupdate.update_all(self.paths, self.fetch)
            try:
                tle, tle_err = self._tle_check(), ''
            except Exception as exc:       # reported with the others
                tle, tle_err = None, f'{exc.__class__.__name__}: {exc}'
            return errors, tle, tle_err

        def finish(result, err):
            if err:
                self.data_msg.setText(_color('Hata: ', RED) + _esc(err))
                return
            errors, tle, tle_err = result
            bad = [NAMES[k] for k, e in errors.items() if e]
            if not bad:
                text = _color('ID ve röle listesi güncellendi ✓', GREEN)
            else:
                text = (_color('Güncellenemeyen: ' + ', '.join(bad), RED) + ' (eski dosyalar duruyor; ayrıntılar: ' +
                        _esc('; '.join(f'{NAMES[k]}: {e}' for k, e in errors.items() if e)) + ')')
            self._tle_done(tle, tle_err)
            if tle is not None and tle.changed:
                text += ' Uydu verisi için yeni element setleri var: "Derleme kopyasına yaz (WSL)".'
            elif not bad:
                text += ' Telsize gitmesi için "Yeni sürüm derle".'
            self.data_msg.setText(text)
        self._job(run, finish)

    def update_user_csv(self):
        self._job(lambda: dataupdate.update_user_csv(self.paths, self.fetch),
                  self._simple(lambda n: f'user.csv güncellendi: {_n(n)} kayıt.'))

    def update_rptrs(self):
        self._job(lambda: dataupdate.update_rptrs(self.paths, self.fetch),
                  self._simple(lambda r: f'rptrs.json güncellendi: {r[0]} röle (Türkiye {r[1]}).'))

    # --- TLE ----------------------------------------------------------------------------------------------------
    def _tle_check(self):
        cache = paths.settings_path().parent / 'celestrak-amateur.txt'
        return dataupdate.tle_check(self.paths, self.fetch, cache=cache, wsl=self.wsl_tle().get('epochs'))

    def check_tle(self):
        self.tle_msg.setText('Kontrol ediliyor...')
        self._job(self._tle_check, lambda result, err: self._tle_done(result, err))
        self.data_msg.setText('')

    def _tle_done(self, check, err):
        self.tle = check
        if err or check is None:
            self.tle_msg.setText(_color('Uydu verisi kontrol edilemedi: ', RED) + _esc(err) +
                                 ' (hiçbir dosya değişmedi)')
            self.tle_write_btn.setEnabled(False)
            return
        t = self.tle_table
        t.setRowCount(len(check.rows))
        for i, r in enumerate(check.rows):
            status, color = TLE_STATUS[r['status']]
            if r['error']:
                status += ' (CelesTrak seti bozuk)'
            cells = (r['name'], str(r['catalog']), _epoch(r['current'], r['current_age']),
                     _epoch(r['celestrak'], r['celestrak_age']), status)
            for j, text in enumerate(cells):
                item = QTableWidgetItem(text)
                if j == 4 and color:
                    item.setForeground(_qcolor(color))
                t.setItem(i, j, item)
        t.show()
        when = time.strftime('%H:%M', time.localtime(check.fetched_at))
        source = f'CelesTrak ({when} indirildi' + (', iki saat içinde tekrar sorulmaz)' if check.from_cache else ')')
        if check.changed:
            names = ', '.join(r['name'] for r in check.newer)
            self.tle_msg.setText(_color(f'{len(check.newer)} uydu için daha yeni veri var: ', GREEN) + _esc(names) +
                                 f'. Kaynak: {source}. "Derleme kopyasına yaz (WSL)" ile yazın, sonra '
                                 '"Yeni sürüm derle".')
        else:
            self.tle_msg.setText(f'Uydu verisi güncel; yazılacak bir şey yok. Kaynak: {source}.')
        self.tle_write_btn.setEnabled(check.changed and self.runner is None)

    def write_tle(self):
        """The developer's click: tle.txt into the WSL build copy (checked there by the firmware's generator)"""
        if self.runner is not None or self.tle is None or not self.tle.changed:
            return
        distro = paths.load_settings().get('wsl_distro', 'Ubuntu')
        program, args = dataupdate.tle_write_command(self.tle.text, distro)
        self._start_wsl('tle', program, args, 'pcapp-tle.log')
        self.tle_msg.setText('WSL derleme kopyasına yazılıyor...')

    def _tle_write_finished(self, code):
        check = self.tle
        if code == 0 and check is not None and dataupdate.tle_written(self.lines, check.sha256):
            paths.save_setting('tle_wsl', {'epochs': check.epochs, 'written': time.strftime('%d.%m.%Y %H:%M'),
                                           'sha256': check.sha256})
            self.tle_msg.setText(_color('Uydu verisi WSL derleme kopyasına yazıldı ✓', GREEN) +
                                 ' Telsize gitmesi için "Yeni sürüm derle"; yeni sürüm kurulmadan önce incelenir.')
            self.tle = None
            self.show_files()
        else:
            err = parsers.error_text(self.lines) or (self.lines[-1] if self.lines else f'exit code {code}')
            self.tle_msg.setText(_color('Yazılamadı: ', RED) + _esc(err) + ' (WSL kopyası değişmedi)')

    # --- WSL processes (build, TLE write) -----------------------------------------------------------------------
    def _start_wsl(self, step, program, args, log_name):
        self.step = step
        self.lines = []
        self.log.clear()
        self.log.open_file(self.root / 'logs' / log_name, program + ' ' + ' '.join(args[:-1]))
        self.log.add('$ ' + program + ' ' + ' '.join(args[:-1]) + ' <script>')
        self.runner = self.runner_factory(self)
        self.runner.line.connect(self._line)
        self.runner.finished.connect(self._finished)
        self.build_btn.setEnabled(False)
        self.tle_write_btn.setEnabled(False)
        self.runner.start(program, args, cwd=self.root)

    def start_build(self):
        if self.runner is not None:
            return
        for v in (self.v_ram, self.v_flash, self.v_sha, self.v_identical, self.v_warn, self.v_review,
                  self.v_tle_build):
            v.setText('-')
        self.build_msg.setText('Derleniyor... (birkaç dakika sürebilir)')
        distro = paths.load_settings().get('wsl_distro', 'Ubuntu')
        program, args = dataupdate.build_command(self.root, distro)
        self._start_wsl('build', program, args, 'pcapp-build.log')

    def _line(self, line):
        self.lines.append(line)
        self.log.add(line)

    def _finished(self, code):
        self.log.close_file()
        self.runner = None
        step, self.step = self.step, None
        self.build_btn.setEnabled(True)
        if step == 'tle':
            self._tle_write_finished(code)
        else:
            self._build_finished(code)
        self._buttons(True)

    def _build_finished(self, code):
        r = parsers.parse_build('\n'.join(self.lines))
        self.build_result = r
        self.v_ram.setText(_mem(r['memory'].get('RAM')))
        self.v_flash.setText(_mem(r['memory'].get('FLASH')))
        self.v_sha.setText(r['sha256'] or '-')
        self.v_identical.setText({True: 'evet', False: 'HAYIR'}.get(r['forced_rebuild_identical'], '-'))
        self.v_warn.setText('-' if r['compiler_warnings'] is None else str(r['compiler_warnings']))
        self.v_tle_build.setText(r.get('tle_line') or '-')
        if code != 0 or not r['sha256']:
            self.build_msg.setText(_color('Derleme başarısız.', RED) + ' Ayrıntılardaki çıktıya bakın.')
            self.v_review.setText(parsers.error_text(self.lines) or f'exit code {code}')
            return
        self.v_review.setText('imzasız (henüz yayımlanmadı)')
        self.build_msg.setText(_color('Derlendi.', GREEN) + ' Bu program yalnızca imzalı sürümleri kurar: önce yayın '
                               'aracıyla (pc/release/make_release.py) imzalanıp yayımlanmalı.')

def _color(text, color):
    return f'<span style="color:{color};font-weight:bold;">{text}</span>'


def _qcolor(name):
    from PyQt5.QtGui import QColor
    return QColor(name)


def _esc(text):
    return (text or '').replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')


def _epoch(date, age):
    if date is None:
        return '-'
    return date.strftime('%d.%m.%Y') + (' (bugün)' if age == 0 else f' ({age} gün önce)')


def _n(v):
    return f'{v:,}'.replace(',', '.')


def _mem(m):
    if not m:
        return '-'
    return f'{m["used"]:,} / {m["size"]:,} B ({m["percent"]:.2f}%)'.replace(',', '.')
