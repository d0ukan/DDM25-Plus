"""Data the firmware build takes in: RadioID.net user.csv / rptrs.json and CelesTrak TLEs, plus the WSL build command.

Every network call goes through a fetch(url) -> bytes function the caller passes in (tests pass a fake). A download
is checked before it replaces anything: written to a temporary file beside the target, then os.replace.

Developer page only (a source checkout of the DDM32 Plus repository with WSL); the frozen program hides it.

TLE (docs/tle-update.md, option A): CelesTrak's amateur element sets are fetched (at most every two hours, the last
good answer is kept), compared with the repository's tle.txt and, on the developer's click, written into the WSL build
copy only (~/ddm32-plus/integration/radiofoundry/sat/tle.txt, checked there by tools/sat_tle.py). The Windows checkout
is never written. The build's rsync keeps the WSL copy while it is the newer one. The resulting build is a new image:
the program installs it only after it is published as a signed release (pc/release/make_release.py).
"""
import base64
import datetime
import hashlib
import json
import os
from pathlib import Path
import time
import urllib.request

from . import tls

USER_CSV_URL = 'https://radioid.net/static/user.csv'
RPTRS_URL = 'https://radioid.net/static/rptrs.json'
AMATEUR_URL = 'https://celestrak.org/NORAD/elements/gp.php?GROUP=amateur&FORMAT=tle'
TLE_CACHE_SECONDS = 2 * 3600         # CelesTrak updates the GP data every two hours; never ask more often
WSL_COPY = '~/ddm32-plus'
TLE_REL = 'integration/radiofoundry/sat/tle.txt'
USER_AGENT = 'DDM32Plus-data-update/1'


class DataError(ValueError):
    pass


def http_fetch(url, timeout=60):
    req = urllib.request.Request(url, headers={'User-Agent': USER_AGENT})
    with urllib.request.urlopen(req, timeout=timeout, context=tls.context()) as r:
        return r.read()


def firmware_paths(fw):
    """The firmware tree's data files; fw is the repository root (firmware sources at its root)"""
    fw = Path(fw)
    return {
        'user_csv': fw / 'research-data' / 'user.csv',
        'rptrs_json': fw / 'research-data' / 'rptrs.json',
        'sat_freq': fw / 'integration' / 'radiofoundry' / 'sat' / 'sat_freq.txt',
        'tle': fw / 'integration' / 'radiofoundry' / 'sat' / 'tle.txt',
    }


def atomic_write(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + '.download')
    tmp.write_bytes(data)
    os.replace(tmp, path)


def file_date(path):
    try:
        return time.strftime('%d.%m.%Y %H:%M', time.localtime(Path(path).stat().st_mtime))
    except OSError:
        return 'yok'


# --- RadioID ------------------------------------------------------------------------------------------------------

def check_user_csv(data):
    """Row count of a RadioID user.csv (header RADIO_ID,CALLSIGN,...)"""
    head = data[:200].decode('utf-8', 'replace').lstrip('﻿')
    if not head.startswith('RADIO_ID,CALLSIGN'):
        raise DataError('user.csv başlığı beklenen gibi değil (RADIO_ID,CALLSIGN,...)')
    rows = data.count(b'\n') - 1 + (0 if data.endswith(b'\n') else 1)
    if rows < 1:
        raise DataError('user.csv boş')
    return rows


TURKEY = ('turkey', 'turkiye', 'türkiye')


def turkish_users(data):
    """Rows of user.csv whose last column (COUNTRY) is Turkey / Turkiye"""
    n = 0
    for line in data.split(b'\n'):
        country = line.rstrip(b'\r').rpartition(b',')[2].strip(b'" ').decode('utf-8', 'replace').lower()
        if country in TURKEY:
            n += 1
    return n


def check_rptrs(data, country=TURKEY):
    """(all repeaters, those in Turkey: RadioID writes "Turkiye") of a RadioID rptrs.json"""
    try:
        doc = json.loads(data.decode('utf-8'))
    except (UnicodeDecodeError, ValueError) as exc:
        raise DataError('rptrs.json JSON değil: %s' % exc)
    rows = doc.get('rptrs') if isinstance(doc, dict) else None
    if not isinstance(rows, list) or not rows:
        raise DataError('rptrs.json içinde "rptrs" listesi yok')
    return len(rows), sum(1 for r in rows if isinstance(r, dict) and str(r.get('country') or '').lower() in country)


def file_counts(paths):
    """What is on disk now: dates and counts (cheap enough for the 20 MB files)"""
    out = {}
    for key in ('user_csv', 'rptrs_json', 'tle'):
        p = paths[key]
        info = {'date': file_date(p), 'count': None}
        try:
            data = p.read_bytes()
            if key == 'user_csv':
                info['count'] = check_user_csv(data)
                info['country'] = turkish_users(data)
            elif key == 'rptrs_json':
                info['count'], info['country'] = check_rptrs(data)
            else:
                sets = parse_tle_text(data.decode('ascii', 'replace'))
                info['count'] = len(sets)
                info['epoch'] = min((s['epoch'] for s in sets), default=None)
        except (OSError, DataError):
            pass
        out[key] = info
    return out


def update_user_csv(paths, fetch=http_fetch):
    data = fetch(USER_CSV_URL)
    rows = check_user_csv(data)
    atomic_write(paths['user_csv'], data)
    return rows


def update_rptrs(paths, fetch=http_fetch):
    data = fetch(RPTRS_URL)
    total, country = check_rptrs(data)
    atomic_write(paths['rptrs_json'], data)
    return total, country


def age_text(epoch, today=None):
    """'bugün', '1 gün önce', '3 gün önce'"""
    if epoch is None:
        return 'yok'
    days = ((today or datetime.date.today()) - epoch).days
    return 'bugün' if days <= 0 else '%d gün önce' % days


def summary_text(info, today=None):
    """The one line the data page shows: 'ID: 8.694 Türk kullanıcı, Röle: 109, Uydu verisi: 3 gün önce'"""
    u, r, t = info['user_csv'], info['rptrs_json'], info['tle']
    n = lambda v: f'{v:,}'.replace(',', '.')   # noqa: E731
    parts = ['ID: ' + (f'{n(u.get("country", 0))} Türk kullanıcı' if u['count'] else 'yok'),
             'Röle: ' + (n(r.get('country', 0)) if r['count'] else 'yok'),
             'Uydu verisi: ' + (age_text(t.get('epoch'), today) if t['count'] else 'yok')]
    return ', '.join(parts)


def update_all(paths, fetch=http_fetch):
    """The two RadioID downloads in turn; each file is replaced only when its own check passes. Returns
    {key: error text or ''}. The TLE has its own check (tle_check) and is written only into the WSL copy"""
    out = {}
    for key, fn in (('user_csv', update_user_csv), ('rptrs_json', update_rptrs)):
        try:
            fn(paths, fetch)
            out[key] = ''
        except Exception as exc:     # reported per file; the others still run
            out[key] = f'{exc.__class__.__name__}: {exc}'
    return out


# --- TLE ----------------------------------------------------------------------------------------------------------

def tle_checksum(line):
    return sum(int(c) if c.isdigit() else 1 if c == '-' else 0 for c in line[:68]) % 10


def tle_epoch(line1):
    """The element set's epoch (UTC date) from line 1, columns 19-32: YYDDD.dddddddd"""
    field = line1[18:32].strip()
    yy, day = int(field[:2]), float(field[2:])
    year = 2000 + yy if yy < 57 else 1900 + yy
    return datetime.date(year, 1, 1) + datetime.timedelta(days=int(day) - 1)


def check_set(name, l1, l2, catalog=None):
    if not (l1.startswith('1 ') and l2.startswith('2 ') and len(l1) >= 69 and len(l2) >= 69):
        raise DataError('TLE satırları biçimsiz (%s)' % name)
    for line in (l1, l2):
        if tle_checksum(line) != int(line[68]):
            raise DataError('TLE sağlama toplamı tutmuyor (%s)' % name)
    n1, n2 = int(l1[2:7]), int(l2[2:7])
    if n1 != n2 or (catalog is not None and n1 != int(catalog)):
        raise DataError('TLE katalog numarası beklenen değil (%s)' % name)
    return {'name': name.strip(), 'line1': l1[:69], 'line2': l2[:69], 'catalog': n1, 'epoch': tle_epoch(l1)}


def parse_tle_text(text, catalog=None):
    """Three-line sets (name, 1, 2) in text; '#' lines skipped"""
    lines = [l.rstrip() for l in text.replace('\r', '').split('\n') if l.strip() and not l.startswith('#')]
    sets = []
    i = 0
    while i < len(lines):
        if lines[i].startswith('1 ') and i + 1 < len(lines) and lines[i + 1].startswith('2 '):
            name, l1, l2, i = 'SAT %s' % lines[i][2:7].strip(), lines[i], lines[i + 1], i + 2
        elif i + 2 < len(lines):
            name, l1, l2, i = lines[i], lines[i + 1], lines[i + 2], i + 3
        else:
            raise DataError('Eksik TLE seti')
        sets.append(check_set(name, l1, l2, catalog))
    return sets


def format_tle_file(old_text, sets, fetched=None):
    """tle.txt as the committed one: first comment line with epoch / fetch date (shown on the radio), the other
    comment lines kept, then the sets"""
    fetched = fetched or datetime.date.today()
    epoch = min(s['epoch'] for s in sets)
    first = '# CelesTrak elements, epoch %s (fetched %s): refresh before a release' % (
        epoch.strftime('%d.%m.%Y'), fetched.strftime('%d.%m.%Y'))
    comments = [l.rstrip() for l in old_text.replace('\r', '').split('\n') if l.startswith('#')][1:]
    body = []
    for s in sets:
        body += [s['name'], s['line1'], s['line2']]
    return '\n'.join([first] + comments + body) + '\n'


def satellites(sat_freq_text):
    """[(catalog, name)] of sat_freq.txt, in its order"""
    out = []
    for line in sat_freq_text.splitlines():
        s = line.strip()
        if not s or s.startswith('#'):
            continue
        parts = s.split()
        out.append((int(parts[0]), parts[1] if len(parts) > 1 else str(parts[0])))
    return out


def find_sets(text, wanted):
    """The element sets of the wanted catalog numbers in a big TLE file (CelesTrak's amateur group); the others are
    skipped unchecked. Returns ({catalog: set}, {catalog: error}): a wanted set with a bad checksum is an error, not
    an element set"""
    lines = [l.rstrip() for l in text.replace('\r', '').split('\n') if l.strip() and not l.startswith('#')]
    found, errors = {}, {}
    for i in range(len(lines) - 1):
        l1, l2 = lines[i], lines[i + 1]
        if not (l1.startswith('1 ') and l2.startswith('2 ')):
            continue
        try:
            n = int(l1[2:7])
        except ValueError:
            continue
        if n not in wanted or n in found:
            continue
        name = lines[i - 1] if i > 0 and not lines[i - 1].startswith(('1 ', '2 ')) else 'SAT %d' % n
        try:
            found[n] = check_set(name, l1, l2, n)
            errors.pop(n, None)
        except (DataError, ValueError, IndexError) as exc:
            errors[n] = str(exc)
    return found, errors


def fetch_amateur(fetch=http_fetch, cache=None, now=None):
    """(text, fetched at (epoch seconds), from cache) of CelesTrak's amateur group. cache: a file kept beside the
    settings; an answer younger than TLE_CACHE_SECONDS is reused instead of asking again"""
    now = time.time() if now is None else now
    if cache is not None:
        try:
            mtime = Path(cache).stat().st_mtime
            if 0 <= now - mtime < TLE_CACHE_SECONDS:
                text = Path(cache).read_text(encoding='ascii', errors='replace')
                if '\n1 ' in '\n' + text:
                    return text, mtime, True
        except OSError:
            pass
    data = fetch(AMATEUR_URL)
    text = data.decode('ascii', 'replace')
    if '\n1 ' not in '\n' + text or '\n2 ' not in text:
        raise DataError('CelesTrak cevabı TLE değil: ' + text[:60].strip())
    if cache is not None:
        try:
            atomic_write(cache, data)
            os.utime(cache, (now, now))
        except OSError:
            pass
    return text, now, False


def _date(value):
    if isinstance(value, datetime.date):
        return value
    try:
        return datetime.date.fromisoformat(str(value))
    except ValueError:
        return None


def age_days(epoch, today=None):
    if epoch is None:
        return None
    return max(0, ((today or datetime.date.today()) - epoch).days)


class TleCheck:
    """One comparison: rows for the page, the tle.txt that would go to the WSL copy, whether anything is newer"""

    def __init__(self, rows, text, fetched_at, from_cache, errors):
        self.rows = rows
        self.text = text
        self.fetched_at = fetched_at
        self.from_cache = from_cache
        self.errors = errors
        self.newer = [r for r in rows if r['status'] == 'new']
        self.epochs = {str(r['catalog']): r['use_epoch'].isoformat() for r in rows}
        self.sha256 = hashlib.sha256(text.encode('ascii')).hexdigest()

    @property
    def changed(self):
        return bool(self.newer)


def tle_check(paths, fetch=http_fetch, cache=None, wsl=None, now=None, today=None):
    """Compare CelesTrak's amateur sets with the repository's tle.txt and the epochs last written to the WSL copy
    (wsl: {catalog: 'YYYY-MM-DD'}). A satellite takes the CelesTrak set only when it is newer; otherwise its current
    set stays. Only reads (the download is cached beside the settings); nothing in firmware/ or WSL is written"""
    today = today or datetime.date.today()
    sats = satellites(paths['sat_freq'].read_text(encoding='utf-8'))
    if not sats:
        raise DataError('sat_freq.txt içinde uydu yok')
    text, fetched_at, from_cache = fetch_amateur(fetch, cache, now)
    new_sets, errors = find_sets(text, {n for n, _ in sats})
    try:
        old_text = paths['tle'].read_text(encoding='utf-8')
    except OSError:
        old_text = ''
    try:
        current = {s['catalog']: s for s in parse_tle_text(old_text)}
    except DataError:           # a broken repository copy: every satellite takes CelesTrak's set
        current = {}
    wsl = wsl or {}
    rows, chosen = [], []
    for n, name in sats:
        cur, new = current.get(n), new_sets.get(n)
        base = max([d for d in (cur['epoch'] if cur else None, _date(wsl.get(str(n)))) if d], default=None)
        if new and (base is None or new['epoch'] > base):
            status, use = 'new', new
        elif new and new['epoch'] == base:
            status, use = 'same', new
        elif cur or new:
            status = 'older' if new else 'missing'
            use = new if (new and (cur is None or new['epoch'] > cur['epoch'])) else cur
        else:
            raise DataError("%s (%d) için ne CelesTrak'ta ne tle.txt'de element seti var" % (name, n))
        rows.append({'catalog': n, 'name': name, 'current': base, 'current_age': age_days(base, today),
                     'celestrak': new['epoch'] if new else None,
                     'celestrak_age': age_days(new['epoch'], today) if new else None,
                     'status': status, 'use_epoch': use['epoch'], 'error': errors.get(n, '')})
        chosen.append(use)
    result = format_tle_file(old_text, chosen, today)
    if len(parse_tle_text(result)) != len(sats):      # the result must read back, one set a satellite
        raise DataError('Yeni tle.txt geri okunamadı')
    return TleCheck(rows, result, fetched_at, from_cache, errors)


def tle_write_script(text, copy=WSL_COPY):
    """bash for WSL: write tle.txt into the build copy only, checked by the firmware's own generator first"""
    data = base64.b64encode(text.encode('ascii')).decode('ascii')
    tmp = TLE_REL + '.download'
    return ' && '.join([
        'set -e',
        'cd %s' % copy,
        'test -f integration/radiofoundry/sat/sat_freq.txt',
        "printf %%s '%s' | base64 -d > %s" % (data, tmp),
        'python3 tools/sat_tle.py --tle %s > /dev/null' % tmp,
        'mv %s %s' % (tmp, TLE_REL),
        'echo "== TLE written to %s/%s"' % (copy, TLE_REL),
        'sha256sum %s' % TLE_REL,
        'head -1 %s' % TLE_REL,
    ])


def tle_write_command(text, distro='Ubuntu'):
    """(program, args) for QProcess: wsl -d <distro> -e bash -lc <script>"""
    return 'wsl.exe', ['-d', distro, '-e', 'bash', '-lc', tle_write_script(text)]


def tle_written(lines, sha256):
    """True when the WSL write printed the expected file hash"""
    return any(l.strip().startswith(sha256 + ' ') for l in lines)


# --- WSL build ----------------------------------------------------------------------------------------------------

BUILD_DIR = 'build/radiofoundry-p25/build/dm32-p25-plus-public'


def wsl_path(windows_path):
    p = str(Path(windows_path).resolve()).replace('\\', '/')
    if len(p) > 1 and p[1] == ':':
        return '/mnt/' + p[0].lower() + p[2:]
    return p


def build_script(root):
    """The bash script the Derle button runs in WSL: sync the repository to ~/ddm32-plus, build the public profile,
    print hash/flags"""
    src = wsl_path(Path(root)).rstrip('/') + '/'
    return ' && '.join([
        'set -e',
        'echo "== rsync repository -> %s"' % WSL_COPY,
        ('rsync -a --exclude /research --exclude /toolchain --exclude /build/ --exclude /logs/ --exclude /pc/ '
         "--exclude /artifacts/ --exclude __pycache__/ --exclude /%s '%s' %s/" % (TLE_REL, src, WSL_COPY)),
        # tle.txt: the Windows copy only when it is newer than the one the app wrote into WSL ("Uydu verileri")
        "rsync -a --update '%s%s' %s/%s" % (src, TLE_REL, WSL_COPY, TLE_REL),
        'cd %s' % WSL_COPY,
        'echo "== TLE: $(head -1 %s)"' % TLE_REL,
        'echo "== build"',
        'python3 tools/build_radiofoundry.py --plus --public --stack-check --verify-rebuild',
        'echo "== result"',
        'sha256sum %s/rfoundry_dm32.bin' % BUILD_DIR,
        "grep -E '\"(forced_rebuild_identical|compiler_warnings|plus_enabled)\"' %s/rfoundry_dm32.json" % BUILD_DIR,
    ])


def build_command(root, distro='Ubuntu'):
    """(program, args) for QProcess: wsl -d <distro> -e bash -lc <script> (no Windows shell in between)"""
    return 'wsl.exe', ['-d', distro, '-e', 'bash', '-lc', build_script(root)]
