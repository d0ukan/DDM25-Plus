"""The signed releases as the pages see them: the catalog (GitHub or the verified cache), the downloaded packages,
and the update check / download jobs (a thread each, so the window stays responsive). Network access happens only
from check() and download(), which run on a click or on the start-up check the user can switch off."""
from PyQt5.QtCore import QObject, QThread, pyqtSignal

from . import __version__, paths, releases


class Job(QThread):
    done = pyqtSignal(object, str)    # result, error

    def __init__(self, fn, parent=None):
        super().__init__(parent)
        self.fn = fn

    def run(self):
        try:
            self.done.emit(self.fn(), '')
        except Exception as exc:          # shown to the user; nothing is replaced on failure
            self.done.emit(None, f'{exc.__class__.__name__}: {exc}')


class Releases(QObject):
    changed = pyqtSignal()
    checked = pyqtSignal(str)                 # error text ('' when the check worked)
    downloaded = pyqtSignal(str, str)         # version, error text
    progress = pyqtSignal(int, int)           # bytes done, total

    def __init__(self, layout, fetch=releases.http_fetch, download=releases.http_download, run_async=True,
                 key=None, parent=None):
        super().__init__(parent)
        self.layout_ = layout
        self.fetch = fetch
        self.download_fn = download
        self.run_async = run_async
        self.key = key                      # None: the key embedded in radiotools/signed_manifest.py
        self.jobs = []
        self.busy = None                    # 'check' or 'download:<version>'
        self.error = ''
        self.catalog = releases.cached_catalog(layout.releases, key)
        self.packages = []
        self.reload_packages()

    # --- state ---------------------------------------------------------------------------------------------------
    def reload_packages(self):
        try:
            self.packages = releases.list_packages(self.layout_.releases, self.key)
        except Exception as exc:            # a damaged folder must not stop the program
            self.packages, self.error = [], str(exc)
        self.changed.emit()

    def package(self, version):
        return next((p for p in self.packages if p.version == version and p.installable), None)

    def versions(self):
        """[(version, FirmwareVersion or None, Package or None)], newest first: the catalog and what is downloaded"""
        seen, out = set(), []
        for fv in self.catalog.firmware:
            out.append((fv.version, fv, self.package(fv.version)))
            seen.add(fv.version)
        for p in self.packages:
            if p.installable and p.version not in seen:
                fv = releases.FirmwareVersion(p.entry, '', p.folder)
                out.append((p.version, fv, p))
                seen.add(p.version)
        sm = releases.rt.signed_manifest()
        out.sort(key=lambda t: sm.parse_version(t[0]), reverse=True)
        return out

    def firmware(self, version):
        return next((fv for v, fv, _ in self.versions() if v == version), None)

    def installed(self):
        """The FirmwareVersion the radio runs as far as this program knows (last install or last backup), or None"""
        s = paths.load_settings()
        sha = s.get('installed_sha256')
        if not sha:
            return None
        return next((fv for _, fv, _ in self.versions() if fv and fv.application_sha256 == sha), None)

    def include_beta(self):
        return paths.load_settings().get('include_beta', True)

    def newest(self):
        return next((fv for _, fv, _ in self.versions() if fv and (self.include_beta() or not fv.is_beta)), None)

    def update_available(self):
        """The newest firmware when it is newer than the installed one (or nothing is known installed)"""
        new = self.newest()
        cur = self.installed()
        if new is None:
            return None
        if cur is None:
            return new if paths.load_settings().get('installed_sha256') else None
        return new if new.key() > cur.key() else None

    def app_update(self):
        return self.catalog.newer_app(__version__, self.include_beta())

    # --- jobs ----------------------------------------------------------------------------------------------------
    def _run(self, name, fn, finish):
        self.busy = name
        self.changed.emit()
        if not self.run_async:
            try:
                result, err = fn(), ''
            except Exception as exc:
                result, err = None, f'{exc.__class__.__name__}: {exc}'
            self._done(finish, result, err)
            return
        job = Job(fn, self)
        job.done.connect(lambda r, e: self._done(finish, r, e))
        job.finished.connect(lambda: self.jobs.remove(job) if job in self.jobs else None)
        self.jobs.append(job)
        job.start()

    def _done(self, finish, result, err):
        self.busy = None
        finish(result, err)
        self.changed.emit()

    def check(self):
        """GitHub's signed releases (the user's click, or the start-up check when it is on)"""
        if self.busy:
            return

        def finish(cat, err):
            if cat is not None:
                self.catalog = cat
                paths.save_setting('last_check_utc', cat.checked_utc)
            self.error = err
            self.checked.emit(err)
        self._run('check', lambda: releases.load_catalog(self.layout_.releases, self.fetch, self.key), finish)

    def download(self, version):
        fv = self.catalog.find(version)
        if self.busy or fv is None:
            return

        def finish(folder, err):
            self.error = err
            self.reload_packages()
            self.downloaded.emit(version, err)
        self._run('download:' + version, lambda: releases.download_firmware(
            fv, self.layout_.releases, self.download_fn, lambda d, t: self.progress.emit(d, t), self.key), finish)
