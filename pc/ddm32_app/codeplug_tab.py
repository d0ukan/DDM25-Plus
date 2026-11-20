"""Kanallarım (read only): the codeplug decoded from a backup image; edits stay in the app and in saved files."""
import json
from pathlib import Path

from PyQt5.QtCore import QSortFilterProxyModel, Qt
from PyQt5.QtGui import QStandardItem, QStandardItemModel
from PyQt5.QtWidgets import (QCheckBox, QFileDialog, QHBoxLayout, QHeaderView, QLabel, QLineEdit, QMenu,
                             QTabWidget, QTableView, QToolButton, QVBoxLayout, QWidget)

from . import codeplug
from .widgets import bigger, icon, plain_label

TABLES = (
    ('channels', 'Kanallar', codeplug.CHANNEL_COLUMNS, 'number'),
    ('zones', 'Bölgeler', codeplug.ZONE_COLUMNS, 'index'),
    ('contacts', 'Kişiler', codeplug.CONTACT_COLUMNS, 'index'),
    ('lists', 'RX grup listeleri', codeplug.LIST_COLUMNS, 'index'),
)
READ_ONLY_KEYS = {'number', 'index', 'mode_byte', 'count', 'freq', 'summary'}


class SortProxy(QSortFilterProxyModel):
    def lessThan(self, a, b):
        x, y = a.data(Qt.UserRole + 1), b.data(Qt.UserRole + 1)
        if isinstance(x, (int, float)) and isinstance(y, (int, float)):
            return x < y
        return str(a.data() or '').lower() < str(b.data() or '').lower()


def _verified(folder):
    try:
        return json.loads((Path(folder) / 'backup-manifest.json').read_text()).get('backup_verified') is True
    except (OSError, ValueError):
        return False


def _sort_key(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return str(v)


class CodeplugTab(QWidget):
    def __init__(self, backups, parent=None):
        super().__init__(parent)
        self.backups_dir = Path(backups)
        self.cp = None
        self.source = None
        self.changes = {}        # (table, key, field) -> {old, new}
        self.models, self.views, self.proxies = {}, {}, {}
        self._loading = False
        self._build()
        self.reload_backups()

    def _build(self):
        outer = QVBoxLayout(self)
        t = QLabel('Kanallarım')
        bigger(t, 8, True)
        outer.addWidget(t)
        warn = QLabel('Sadece görüntüleme: telsize yazmaz. Kanalları değiştirmek için upstream CPS kullanın.')
        warn.setWordWrap(True)
        warn.setToolTip('Telsize yazmaz — yazmak için upstream CPS kullanın. Buradaki düzenlemeler sadece bu '
                        'uygulamada ve kaydettiğiniz dosyalarda kalır.')
        warn.setStyleSheet('QLabel { background: #fde2a7; color: #3b2a00; padding: 6px; border-radius: 4px; '
                           'font-weight: bold; }')
        outer.addWidget(warn)
        self.info = plain_label('Yedek bulunamadı. Önce "Telsizi güncelle" sayfasından bir yedek alın.', 2)
        outer.addWidget(self.info)
        frow = QHBoxLayout()
        frow.addWidget(QLabel('Ara:'))
        self.filter = QLineEdit()
        self.filter.setPlaceholderText('ad, frekans, mod, NAC, TG ...')
        self.filter.setClearButtonEnabled(True)
        self.filter.textChanged.connect(self._filter)
        frow.addWidget(self.filter, 1)
        self.all_columns = QCheckBox('Tüm sütunlar')
        self.all_columns.setToolTip('Kapalı: sadece No, Ad, Frekans, Mod, Ayar. Açık: kaydın bütün alanları.')
        self.all_columns.toggled.connect(self._columns_mode)
        frow.addWidget(self.all_columns)
        self.more_btn = QToolButton()
        self.more_btn.setText('Diğer işlemler')
        self.more_btn.setIcon(icon(self, 'SP_FileDialogDetailedView'))
        self.more_btn.setToolButtonStyle(Qt.ToolButtonTextBesideIcon)
        self.more_btn.setPopupMode(QToolButton.InstantPopup)
        menu = QMenu(self.more_btn)
        menu.addAction('Başka bir yedek aç...', self.pick_folder)
        menu.addAction('Bu tabloyu CSV olarak kaydet...', self.export_csv_dialog)
        menu.addAction('Değişiklik listesini kaydet (JSON)...', self.export_changes_dialog)
        menu.addAction('Değişiklikleri geri al', self.revert)
        self.more_btn.setMenu(menu)
        frow.addWidget(self.more_btn)
        outer.addLayout(frow)
        self.tabs = QTabWidget()
        for key, title, columns, _ in TABLES:
            model = QStandardItemModel(0, len(columns))
            model.setHorizontalHeaderLabels([h for _, h in columns])
            model.itemChanged.connect(lambda item, k=key: self._edited(k, item))
            proxy = SortProxy()
            proxy.setSourceModel(model)
            proxy.setFilterCaseSensitivity(Qt.CaseInsensitive)
            proxy.setFilterKeyColumn(-1)
            view = QTableView()
            view.setModel(proxy)
            view.setSortingEnabled(True)
            view.sortByColumn(0, Qt.AscendingOrder)
            view.horizontalHeader().setSectionResizeMode(QHeaderView.Interactive)
            view.horizontalHeader().setStretchLastSection(True)
            view.verticalHeader().setVisible(False)
            view.setAlternatingRowColors(True)
            bigger(view, 1)
            self.models[key], self.proxies[key], self.views[key] = model, proxy, view
            self.tabs.addTab(view, title)
        outer.addWidget(self.tabs, 1)
        self.changes_label = QLabel('')
        outer.addWidget(self.changes_label)
        self._columns_mode(False)

    def _columns_mode(self, show_all):
        view = self.views['channels']
        for i, (k, _) in enumerate(codeplug.CHANNEL_COLUMNS):
            hidden = (k not in codeplug.SIMPLE_CHANNEL_KEYS) if not show_all else (k in codeplug.DERIVED_KEYS)
            view.setColumnHidden(i, hidden)
        view.resizeColumnsToContents()

    # --- loading ------------------------------------------------------------------------------------------------
    def reload_backups(self):
        """The newest verified backup loads by itself"""
        self.backups = sorted((p.parent for p in self.backups_dir.glob('*/q128-pass1.bin')),
                              key=lambda p: p.stat().st_mtime, reverse=True)
        for f in self.backups:
            if _verified(f):
                return self.load_folder(f)
        return False

    def pick_folder(self):
        d = QFileDialog.getExistingDirectory(self, 'Yedek klasörü (q128-pass1.bin)', str(self.backups_dir))
        if d:
            self.load_folder(Path(d))

    def load_folder(self, folder):
        image = folder / 'q128-pass1.bin'
        try:
            cp = codeplug.load(image)
        except (OSError, codeplug.CodeplugError) as exc:
            self.info.setText(f'Okunamadı: {exc}')
            return False
        self.load_codeplug(cp, folder, _verified(folder))
        return True

    def load_codeplug(self, cp, source, verified=None):
        self.cp, self.source = cp, source
        self.changes = {}
        self._loading = True
        data = {'channels': cp.channels, 'zones': cp.zones, 'contacts': cp.contacts, 'lists': cp.lists}
        for key, _, columns, _ in TABLES:
            m = self.models[key]
            m.removeRows(0, m.rowCount())
            for row in data[key]:
                items = []
                for col, _ in columns:
                    v = row.get(col, '')
                    it = QStandardItem('' if v is None else str(v))
                    it.setData(_sort_key(v), Qt.UserRole + 1)
                    it.setEditable(col not in READ_ONLY_KEYS)
                    items.append(it)
                m.appendRow(items)
            self.views[key].resizeColumnsToContents()
        self._loading = False
        c = cp.counts()
        name = Path(str(source)).name
        ver = '' if verified else ' (doğrulanmamış yedek)'
        zones = f'{c["zones"]} bölge' if c['zones'] else 'bölge yok (telsiz "All channels" gösterir)'
        self.info.setText(f'<b>{c["channels"]} kanal</b>, {zones}, {c["contacts"]} kişi. '
                          f'Kaynak: yedek {name}{ver}.')
        self.info.setToolTip(', '.join(f'{k}: {v}' for k, v in sorted(cp.mode_counts().items())) +
                             (f'. Okunamayan kanal kaydı: {cp.bad_records}' if cp.bad_records else ''))
        self._columns_mode(self.all_columns.isChecked())
        self._update_changes()

    # --- edits --------------------------------------------------------------------------------------------------
    def _columns(self, table):
        return next(t[2] for t in TABLES if t[0] == table)

    def _key_col(self, table):
        return next(t[3] for t in TABLES if t[0] == table)

    def _edited(self, table, item):
        if self._loading:
            return
        columns = self._columns(table)
        field = columns[item.column()][0]
        m = self.models[table]
        key_index = [c for c, _ in columns].index(self._key_col(table))
        key = m.item(item.row(), key_index).text()
        original = self._original(table, key, field)
        ident = (table, key, field)
        new = item.text()
        if new == original:
            self.changes.pop(ident, None)
        else:
            self.changes[ident] = {'old': original, 'new': new}
        item.setData(_sort_key(new), Qt.UserRole + 1)
        self._update_changes()

    def _original(self, table, key, field):
        rows = {'channels': self.cp.channels, 'zones': self.cp.zones, 'contacts': self.cp.contacts,
                'lists': self.cp.lists}[table]
        kc = self._key_col(table)
        for r in rows:
            if str(r[kc]) == key:
                v = r.get(field, '')
                return '' if v is None else str(v)
        return ''

    def _update_changes(self):
        n = len(self.changes)
        self.changes_label.setText(f'{n} değişiklik (sadece bu uygulamada; "Diğer işlemler" ile kaydedin).'
                                   if n else '')

    def revert(self):
        if self.cp is not None:
            self.load_codeplug(self.cp, self.source)

    def change_rows(self):
        return [{'table': t, 'key': k, 'field': f, 'old': v['old'], 'new': v['new']}
                for (t, k, f), v in sorted(self.changes.items())]

    def _filter(self, text):
        for p in self.proxies.values():
            p.setFilterFixedString(text)

    # --- exports ------------------------------------------------------------------------------------------------
    def current_table(self):
        return TABLES[self.tabs.currentIndex()][0]

    def table_rows(self, table):
        """The table as shown now (edits included, filter and sort ignored)"""
        m, columns = self.models[table], self._columns(table)
        return [{c: m.item(r, i).text() for i, (c, _) in enumerate(columns)} for r in range(m.rowCount())]

    def csv_text(self, table):
        return codeplug.to_csv(self._columns(table), self.table_rows(table))

    def export_csv(self, table, path):
        Path(path).write_text(self.csv_text(table), encoding='utf-8-sig')

    def export_changes(self, path):
        Path(path).write_text(codeplug.change_list(Path(str(self.source)).name if self.source else '',
                                                   self.change_rows()), encoding='utf-8')

    def export_csv_dialog(self):
        if self.cp is None:
            return
        t = self.current_table()
        path, _ = QFileDialog.getSaveFileName(self, 'CSV kaydet', f'ddm32-{t}.csv', 'CSV (*.csv)')
        if path:
            self.export_csv(t, path)

    def export_changes_dialog(self):
        if self.cp is None:
            return
        path, _ = QFileDialog.getSaveFileName(self, 'Değişiklik listesi', 'ddm32-degisiklikler.json',
                                              'JSON (*.json)')
        if path:
            self.export_changes(path)
