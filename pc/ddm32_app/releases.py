"""Signed releases from GitHub: the version list, downloads with hash checks, the installed version, and the
compatibility warnings for a change of version. No Qt here; network and file access are injected for the tests.

Every release of d0ukan/DDM32-Plus carries ddm32-manifest.json and its Ed25519 signature (radiotools/signed_manifest.py).
A release whose manifest does not verify with the key embedded in this program is not listed (it shows up only as a
rejected release in the details). A firmware zip is used only when its SHA-256 and size equal the signed manifest's;
the application inside must equal the manifest's application_sha256. The installer checks the signed manifest again
itself before it writes anything.
Network access happens only on a click (or the start-up check, which the user can switch off); installing always
needs its own click and confirmation.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import urllib.parse
import urllib.request
import zipfile

from . import __version__, paths, rt, tls

START, SLOT = 0xC000, 0xE8000
API_URL = 'https://api.github.com/repos/d0ukan/DDM32-Plus/releases?per_page=100'
ALLOWED_HOSTS = ('api.github.com', 'github.com', 'objects.githubusercontent.com',
                 'release-assets.githubusercontent.com')
USER_AGENT = f'DDM32Plus/{__version__}'
PACKAGE_FILES = ('rfoundry_dm32.bin', 'rfoundry_dm32.json', 'rfoundry_dm32.map')
MAX_API = 8 << 20
MAX_MANIFEST = 1 << 20
MAX_SIG = 1024
MANIFEST_CACHE = 'manifests'


class NetError(Exception):
    pass


class VerifyError(Exception):
    pass


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


# --- network -------------------------------------------------------------------------------------------------------
def _check_url(url):
    u = urllib.parse.urlsplit(url)
    if u.scheme != 'https' or u.hostname not in ALLOWED_HOSTS:
        raise NetError(f'Refusing a download outside GitHub over HTTPS: {u.scheme}://{u.hostname}')


class _Redirects(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        _check_url(newurl)
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def _open(url, timeout):
    _check_url(url)
    opener = urllib.request.build_opener(_Redirects, urllib.request.HTTPSHandler(context=tls.context()))
    req = urllib.request.Request(url, headers={'User-Agent': USER_AGENT, 'Accept': 'application/octet-stream, */*'})
    if 'api.github.com' in url:
        req.add_header('Accept', 'application/vnd.github+json')
    try:
        return opener.open(req, timeout=timeout)
    except NetError:
        raise
    except Exception as exc:
        raise NetError(f'{type(exc).__name__}: {exc}') from None


def http_fetch(url, limit=MAX_MANIFEST, timeout=30):
    """The body of url (GitHub over HTTPS only), at most limit bytes"""
    with _open(url, timeout) as r:
        data = r.read(limit + 1)
    if len(data) > limit:
        raise NetError(f'Answer larger than {limit} bytes: {url}')
    return data


def http_download(url, dest, size, progress=None, timeout=60):
    """url into dest (via dest.partial), refusing more than size bytes; progress(done, size)"""
    dest = Path(dest)
    part = dest.with_name(dest.name + '.partial')
    done = 0
    with _open(url, timeout) as r, open(part, 'wb') as out:
        while True:
            block = r.read(1 << 16)
            if not block:
                break
            done += len(block)
            if done > size:
                out.close()
                part.unlink()
                raise NetError('Download larger than the signed size')
            out.write(block)
            if progress:
                progress(done, size)
    os.replace(part, dest)
    return dest


# --- catalog of signed releases ------------------------------------------------------------------------------------
class FirmwareVersion:
    def __init__(self, entry, release, manifest_dir=None):
        self.entry = entry
        self.release = release
        self.manifest_dir = manifest_dir
        self.version = entry['version']
        self.channel = entry['channel']
        self.application_sha256 = entry['application_sha256']
        self.application_size = entry['application_size']

    @property
    def is_beta(self):
        return self.channel == 'beta'

    def key(self):
        return rt.signed_manifest().parse_version(self.version)

    def notes(self, lang='tr'):
        return self.entry['notes'].get(lang) or self.entry['notes'].get('en', '')

    def label(self):
        return f'DDM32 Plus {self.version}' + (' (beta)' if self.is_beta else '')


class Catalog:
    def __init__(self):
        self.firmware = []          # FirmwareVersion, newest first
        self.apps = []              # (app entry, release tag), newest first
        self.rejected = []          # (release tag, reason)
        self.checked_utc = None
        self.source = ''            # 'github' or 'cache'

    def add(self, tag, manifest, manifest_dir):
        known = {f.version for f in self.firmware}
        for e in manifest['firmware']:
            if e['version'] not in known:
                self.firmware.append(FirmwareVersion(e, tag, manifest_dir))
        if manifest.get('app'):
            self.apps.append((manifest['app'], tag, manifest_dir))

    def sort(self):
        sm = rt.signed_manifest()
        self.firmware.sort(key=lambda f: sm.parse_version(f.version), reverse=True)
        self.apps.sort(key=lambda a: sm.parse_version(a[0]['version']), reverse=True)

    def find(self, version):
        return next((f for f in self.firmware if f.version == version), None)

    def by_application(self, sha):
        return next((f for f in self.firmware if f.application_sha256 == sha), None)

    def newest(self, include_beta=True):
        return next((f for f in self.firmware if include_beta or not f.is_beta), None)

    def newer_app(self, current=__version__, include_beta=True):
        """(app entry, tag, manifest_dir) of a program newer than current, else None"""
        sm = rt.signed_manifest()
        cur = sm.parse_version(current)
        for entry, tag, mdir in self.apps:
            pre = '-' in entry['version']
            if sm.parse_version(entry['version']) > cur and (include_beta or not pre):
                return entry, tag, mdir
        return None


def _cache_dir(releases_root):
    return Path(releases_root) / MANIFEST_CACHE


def _store(cache, tag, data, sig):
    d = cache / tag
    d.mkdir(parents=True, exist_ok=True)
    sm = rt.signed_manifest()
    (d / sm.MANIFEST_NAME).write_bytes(data)
    (d / sm.SIG_NAME).write_bytes(sig if isinstance(sig, bytes) else sig.encode('ascii'))
    return d


def load_catalog(releases_root, fetch=http_fetch, key=None):
    """The signed releases on GitHub (one API call, then each release's manifest and signature). Verified manifests
    are kept under <releases_root>/manifests/<tag>/ for offline use"""
    sm = rt.signed_manifest()
    cat = Catalog()
    cat.source, cat.checked_utc = 'github', paths.now_utc()
    try:
        listing = json.loads(fetch(API_URL, limit=MAX_API).decode('utf-8'))
    except NetError:
        raise
    except ValueError:
        raise NetError('GitHub answered with something that is not a release list') from None
    if not isinstance(listing, list):
        raise NetError('GitHub answered with something that is not a release list')
    cache = _cache_dir(releases_root)
    for r in listing:
        if not isinstance(r, dict) or r.get('draft'):
            continue
        tag = str(r.get('tag_name', ''))
        assets = {a.get('name'): a for a in r.get('assets', []) if isinstance(a, dict)}
        if sm.MANIFEST_NAME not in assets or sm.SIG_NAME not in assets:
            cat.rejected.append((tag, 'no signed manifest'))
            continue
        try:
            urls = [assets[n].get('browser_download_url', '') for n in (sm.MANIFEST_NAME, sm.SIG_NAME)]
            for u in urls:
                if not u.startswith(sm.URL_PREFIX + tag + '/'):
                    raise sm.ManifestError('manifest asset outside this release')
            data = fetch(urls[0], limit=MAX_MANIFEST)
            sig = fetch(urls[1], limit=MAX_SIG)
            m = sm.verify_bytes(data, sig, key)
            if m['release'] != tag:
                raise sm.ManifestError(f'manifest is for release {m["release"]}, not {tag}')
        except (sm.ManifestError, NetError) as exc:
            cat.rejected.append((tag, str(exc)))
            continue
        cat.add(tag, m, _store(cache, tag, data, sig))
    cat.sort()
    return cat


def cached_catalog(releases_root, key=None):
    """The catalog from the manifests verified earlier (each verified again now); for offline use"""
    sm = rt.signed_manifest()
    cat = Catalog()
    cat.source = 'cache'
    cache = _cache_dir(releases_root)
    if cache.is_dir():
        for d in sorted(p for p in cache.iterdir() if p.is_dir()):
            try:
                m = sm.verify_files(d, key)
                if m['release'] != d.name:
                    raise sm.ManifestError('release mismatch')
            except sm.ManifestError as exc:
                cat.rejected.append((d.name, str(exc)))
                continue
            cat.add(d.name, m, d)
    cat.sort()
    return cat


# --- firmware packages ---------------------------------------------------------------------------------------------
class Package:
    """A downloaded release folder: rfoundry_dm32.bin/.json/.map with the signed manifest that lists it"""

    def __init__(self, folder, key=None):
        sm = rt.signed_manifest()
        self.folder = Path(folder)
        self.image = self.folder / 'rfoundry_dm32.bin'
        self.problems = []
        self.entry = None
        try:
            app = self.image.read_bytes()
            self.sha256 = sha256_bytes(app)
            self.size = len(app)
            self.entry = sm.release_gate(app, self.folder, key)
            for name in PACKAGE_FILES:
                if not (self.folder / name).is_file():
                    self.problems.append(f'{name} yok')
        except OSError as exc:
            self.sha256, self.size = '', 0
            self.problems.append(f'okunamadı ({exc.__class__.__name__})')
        except sm.ManifestError as exc:
            self.problems.append(f'imza denetimi geçmedi: {exc}')
        self.version = self.entry['version'] if self.entry else self.folder.name
        self.name = f'DDM32 Plus {self.version}'

    @property
    def installable(self):
        return self.entry is not None and not self.problems

    @property
    def channel(self):
        return self.entry['channel'] if self.entry else ''

    def status_text(self):
        if self.problems:
            return 'Paket kullanılamaz: ' + ', '.join(self.problems)
        return 'İmzalı sürüm listesinde ✓'


def package_folder(releases_root, version):
    return Path(releases_root) / version


def list_packages(releases_root, key=None):
    """Downloaded release folders, newest version first"""
    sm = rt.signed_manifest()
    root = Path(releases_root)
    out = []
    if root.is_dir():
        for d in root.iterdir():
            if d.is_dir() and (d / 'rfoundry_dm32.bin').is_file():
                out.append(Package(d, key))

    def order(p):
        try:
            return (1, sm.parse_version(p.version))
        except sm.ManifestError:
            return (0, ())
    return sorted(out, key=order, reverse=True)


def _safe_member(name):
    return name in PACKAGE_FILES


def download_firmware(fv, releases_root, download=http_download, progress=None, key=None):
    """The verified release folder for fv (downloaded when not there yet). Raises VerifyError / NetError; a failed
    download leaves nothing behind"""
    sm = rt.signed_manifest()
    root = Path(releases_root)
    folder = package_folder(root, fv.version)
    if folder.is_dir():
        p = Package(folder, key)
        if p.installable and p.sha256 == fv.application_sha256:
            return folder
        shutil.rmtree(folder)
    tmp = root / (fv.version + '.partial')
    if tmp.exists():
        shutil.rmtree(tmp)
    tmp.mkdir(parents=True)
    try:
        entry = fv.entry
        zpath = tmp / entry['file']
        download(entry['url'], zpath, entry['size'], progress)
        if zpath.stat().st_size != entry['size'] or sha256_file(zpath) != entry['sha256']:
            raise VerifyError('İndirilen dosyanın SHA-256 özeti imzalı listedekiyle aynı değil')
        with zipfile.ZipFile(zpath) as z:
            names = z.namelist()
            if sorted(names) != sorted(PACKAGE_FILES) or not all(_safe_member(n) for n in names):
                raise VerifyError('Sürüm paketinin içeriği beklenen üç dosya değil')
            for n in PACKAGE_FILES:
                info = z.getinfo(n)
                if info.file_size > 64 << 20:
                    raise VerifyError('Sürüm paketinde fazla büyük dosya')
                (tmp / n).write_bytes(z.read(n))
        zpath.unlink()
        app = (tmp / 'rfoundry_dm32.bin').read_bytes()
        if sha256_bytes(app) != fv.application_sha256 or len(app) != fv.application_size:
            raise VerifyError('Paketteki uygulama imzalı listedeki özetle aynı değil')
        src = Path(fv.manifest_dir)
        for n in (sm.MANIFEST_NAME, sm.SIG_NAME):
            shutil.copyfile(src / n, tmp / n)
        p = Package(tmp, key)
        if not p.installable:
            raise VerifyError(p.status_text())
        os.replace(tmp, folder)
        return folder
    except BaseException:
        shutil.rmtree(tmp, ignore_errors=True)
        raise


# --- what the radio runs and what a change of version means ----------------------------------------------------------
def match_installed(program, firmware):
    """The FirmwareVersion whose application is in this program flash image (from a backup), else None"""
    for fv in firmware:
        n = fv.application_size
        if (len(program) >= SLOT and sha256_bytes(program[START:START + n]) == fv.application_sha256 and
                program[START + n:SLOT] == b'\xff' * (SLOT - START - n)):
            return fv
    return None


def read_backup_program(backup_dir):
    """progmem-pass1.bin of a backup the backup tool marked verified (file read only)"""
    folder = Path(backup_dir)
    m = json.loads((folder / 'backup-manifest.json').read_text())
    if m.get('backup_verified') is not True:
        raise ValueError('not a verified backup')
    return (folder / 'progmem-pass1.bin').read_bytes()


INFO, WARN, DANGER = 'info', 'warn', 'danger'


def change_warnings(current, target, detection=None):
    """[(level, Turkish text)] for installing target on a radio that runs current (a FirmwareVersion or None) and
    whose backup was detected as detection (radio_detect's JSON or None). DANGER asks for a second confirmation"""
    sm = rt.signed_manifest()
    out = []
    if target.is_beta:
        out.append((WARN, 'Bu bir beta sürüm: çoğu özellik henüz gerçek telsizde denenmedi.'))
    if detection:
        if detection.get('layout') == 'stock':
            v = detection.get('stock_version') or '?'
            out.append((INFO, f'İlk kurulum: telsizde stok yazılım .{v} var. Telsizin kendi DMR codec\'i yedeğinizden '
                              'alınıp ayrılmış codec bölümüne hazırlanır.'))
            if not detection.get('stock_tested'):
                out.append((WARN, f'DDM32 Plus yalnızca stok .047 üzerinde denendi. Stok .{v} desteklenen bir '
                                  'sürüm ama bu sürümle denenmedi.'))
        elif detection.get('firmware') == 'classic':
            out.append((INFO, 'Telsizde Open25-DM32 Classic var; codec bölümü korunur, kanallar Classic biçiminden '
                              'okunur.'))
    if current is None:
        if detection and detection.get('firmware') == 'ddm32':
            out.append((WARN, 'Telsizdeki DDM32 Plus sürümü imzalı listede yok; ayar uyumluluğu denetlenemedi.'))
        return out
    cur, new = sm.parse_version(current.version), sm.parse_version(target.version)
    if new == cur:
        out.append((INFO, 'Bu sürüm zaten telsizde yüklü.'))
        return out
    cf, tf = current.entry, target.entry
    if new < cur:
        out.append((WARN, f'Eski bir sürüme geçiyorsunuz ({current.version} → {target.version}).'))
        if tf['settings_format'] < cf['settings_format']:
            out.append((DANGER, f'Ayar biçimi geriye gidiyor ({cf["settings_format"]} → {tf["settings_format"]}): '
                                'eski sürüm, yeni sürümde kaydedilen ayarları farklı okuyabilir.'))
        if tf['codeplug_format'] < cf['codeplug_format']:
            out.append((DANGER, f'Kanal biçimi geriye gidiyor ({cf["codeplug_format"]} → {tf["codeplug_format"]}): '
                                'örneğin yeni sürümde kaydedilmiş bir P25 kanalı eski sürümde farklı görünebilir. '
                                'Kurulumdan sonra kanallarınızı kontrol edin; gerekirse CPS ile yeniden yazın.'))
        if new < sm.parse_version(cf['min_compatible_version']):
            out.append((DANGER, f'{current.version} sürümünün kaydettiği veriler en az {cf["min_compatible_version"]} '
                                f'gerektirir; {target.version} bu verileri doğru okuyamayabilir.'))
    else:
        if tf['settings_format'] > cf['settings_format'] or tf['codeplug_format'] > cf['codeplug_format']:
            out.append((INFO, 'Yeni sürüm ayar/kanal biçimini yeniler: sonra eski bir sürüme dönmek uyarılı olur.'))
    return out


def needs_second_confirm(warnings):
    return any(level == DANGER for level, _ in warnings)


# --- the program itself ----------------------------------------------------------------------------------------------
def download_app(entry, updates_dir, download=http_download, progress=None):
    """The program zip of a signed manifest's app entry, verified (size and SHA-256)"""
    updates_dir = Path(updates_dir)
    updates_dir.mkdir(parents=True, exist_ok=True)
    dest = updates_dir / entry['file']
    if dest.exists():
        dest.unlink()
    download(entry['url'], dest, entry['size'], progress)
    if dest.stat().st_size != entry['size'] or sha256_file(dest) != entry['sha256']:
        dest.unlink()
        raise VerifyError('İndirilen program dosyasının SHA-256 özeti imzalı listedekiyle aynı değil')
    return dest
