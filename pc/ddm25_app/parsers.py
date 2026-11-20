"""Parsers for the output of the guarded command-line tools the app runs as subprocesses.

The tools themselves (radiotools/dm32_readonly_backup.py, install_plus.py, install_firstboot.py, radio_detect.py,
plus_txdiag.py, plus_live.py; tools/build_radiofoundry.py for the WSL build) stay the only code that talks to the radio
or builds firmware; the app only reads what they print. Every parser takes plain text, so the tests feed recorded/synthetic output.
"""
import json
import re

# --- Backup (radiotools/dm32_readonly_backup.py) -----------------------------------------------------------------------

PROGMEM_BYTES = 1 << 20
Q128_BYTES = 1 << 24
_BACKUP_TOTAL = 2 * (PROGMEM_BYTES + Q128_BYTES)

RE_READY_BACKUP = re.compile(r'^READY: (\S+) open;.*Waiting (\d+)s\.')
RE_READY_INSTALL = re.compile(r'^READY on (\S+): .*Waiting (\d+)s\.')
RE_BACKUP_PROGRESS = re.compile(r'^(progmem|q128) pass ([12]): (\d+)% \((\d+)/(\d+)\)')
RE_BOOT_ROM = re.compile(r'^boot ROM answered')
RE_TIMEOUT = re.compile(r'did not start its serial boot ROM within (\d+) s')
RE_VERIFIED = re.compile(r'^VERIFIED: ')
RE_PASS = re.compile(r'^PASS: ')
RE_ERROR_LINE = re.compile(r'^((?:\w+\.)*\w+(?:Error|Exception)|SystemExit|KeyboardInterrupt|DumpError)\b:?\s*(.*)$')


def backup_progress(line):
    """Overall backup progress 0..100 from one progress line, or None. Two passes over progmem (1 MiB) and q128
    (16 MiB), in that order"""
    m = RE_BACKUP_PROGRESS.match(line.strip())
    if not m:
        return None
    memory, capture_pass, done = m.group(1), int(m.group(2)), int(m.group(4))
    base = (capture_pass - 1) * (PROGMEM_BYTES + Q128_BYTES)
    if memory == 'q128':
        base += PROGMEM_BYTES
    return min(100, (base + done) * 100 // _BACKUP_TOTAL)


def ready_wait(line):
    """(port, seconds) when the tool has the port open and waits for the radio to be switched on"""
    s = line.strip()
    m = RE_READY_BACKUP.match(s) or RE_READY_INSTALL.match(s)
    return (m.group(1), int(m.group(2))) if m else None


def error_text(lines):
    """The last exception line of a traceback ('ValueError: ...'), or ''"""
    for line in reversed([l.strip() for l in lines if l.strip()]):
        m = RE_ERROR_LINE.match(line)
        if m:
            return line
        if line.startswith('Refusing'):
            return line
    return ''


def is_timeout(lines):
    """True when the tool gave up waiting for the radio's boot ROM (the radio was not switched on in time)"""
    return any(RE_TIMEOUT.search(l) for l in lines)


class BackupResult:
    """What one backup run printed"""

    def __init__(self, lines, exit_code):
        self.lines = list(lines)
        self.exit_code = exit_code
        self.verified = exit_code == 0 and any(RE_VERIFIED.match(l.strip()) for l in self.lines)
        self.timeout = is_timeout(self.lines)
        self.error = '' if self.verified else (error_text(self.lines) or f'exit code {exit_code}')


# --- Install and restore (radiotools/install_plus.py, install_firstboot.py) ------------------------------------------------------------------------------

# Phases in the order install_plus prints them, with the share of the bar each one gets
INSTALL_PHASES = (
    ('Read current firmware', 10),
    ('Check settings chip', 8),
    ('Written and sector-readback checked', 54),
    ('Read complete installed program chip', 20),
    ('Recheck untouched settings chip', 8),
)
# A restore writes from the saved backup without pre-reads (install_firstboot.py --restore-stock, install_plus.py
# --restore), then reads the application back
RESTORE_PHASES = (
    ('Firmware written and sector-readback checked', 75),
    ('Verify newly installed application', 25),
    ('Verify restored application', 25),
)
RE_PHASE = re.compile(r'^(.+?): (\d+)%')


def install_progress(line, phases=INSTALL_PHASES):
    """Overall progress 0..100 from one line, or None"""
    m = RE_PHASE.match(line.strip())
    if not m:
        return None
    label, percent = m.group(1), min(100, int(m.group(2)))
    done = 0
    for name, weight in phases:
        if name == label:
            if phases is RESTORE_PHASES and name.startswith('Verify'):
                done = 75
            return min(100, done + weight * percent // 100)
        done += weight
    return None


def first_json(text):
    """The first JSON object a tool printed. Raises ValueError with the tool's own error when there is none"""
    start = text.find('{')
    if start < 0:
        raise ValueError(error_text(text.splitlines()) or 'No JSON in the tool output')
    depth = 0
    for i in range(start, len(text)):
        if text[i] == '{':
            depth += 1
        elif text[i] == '}':
            depth -= 1
            if depth == 0:
                return json.loads(text[start:i + 1])
    raise ValueError('Incomplete JSON in the tool output')


def parse_plan(text):
    """The JSON write plan install_plus / install_firstboot print first (with --plan-only it is all they print)"""
    plan = first_json(text)
    for key in ('mode', 'changed_sectors', 'first', 'last', 'boot_area_unchanged', 'codec_slot_unchanged'):
        if key not in plan:
            raise ValueError(f'Plan lacks {key}')
    return plan


def parse_detect(text):
    """radio_detect.py's JSON"""
    d = first_json(text)
    for key in ('layout', 'firmware', 'install_mode', 'supported'):
        if key not in d:
            raise ValueError(f'Detection lacks {key}')
    return d


APP_SECTORS = (0x100000 - 0xC000) // 4096       # every application + slot sector: 244


def plan_safe(plan, mode=None):
    """A plan the wizard lets the user run: never the boot area, never the data chip, and for each mode exactly the
    region that mode may write"""
    if mode is not None and plan.get('mode') != mode:
        return False
    if plan.get('boot_area_unchanged') is not True or plan.get('data_chip_written', False) is not False:
        return False
    try:
        first = int(plan['first'], 16) if plan.get('first') else None
    except (TypeError, ValueError):
        return False
    if not plan.get('changed_sectors') or first is None or first < 0xC000:
        return False
    m = plan.get('mode')
    if m == 'install_plus':
        return plan['codec_slot_unchanged'] is True and not plan.get('codec_slot_rewritten_from_backup')
    if m == 'install_plus_from_stock':
        return (plan.get('codec_slot_prepared_from_backup') is True and
                not plan.get('codec_slot_rewritten_from_backup'))
    if m in ('restore', 'restore_stock'):
        return plan['changed_sectors'] == APP_SECTORS and first == 0xC000
    return False


class InstallResult:
    """One install run: printed lines plus the receipt it left in --out"""

    RECEIPT_FLAGS = ('writes_started', 'installation_verified', 'bootloader_unchanged', 'codec_slot_verified',
                     'data_sector_crcs_unchanged', 'restart_sent')

    def __init__(self, lines, exit_code, receipt=None):
        self.lines = list(lines)
        self.exit_code = exit_code
        self.receipt = receipt or {}
        self.verified = any(RE_VERIFIED.match(l.strip()) for l in self.lines)
        self.passed = exit_code == 0 and any(RE_PASS.match(l.strip()) for l in self.lines)
        self.timeout = is_timeout(self.lines)
        # Unknown counts as started: only a receipt that says no write was sent lets the user plan again
        self.writes_started = bool(self.receipt.get('writes_started', True)) or any(
            l.startswith('INSTALLING:') for l in self.lines)
        if self.passed:
            self.error = ''
        else:
            self.error = self.receipt.get('error') or error_text(self.lines) or f'exit code {exit_code}'

    def flags(self):
        return {k: self.receipt.get(k) for k in self.RECEIPT_FLAGS if k in self.receipt}


# --- Health (radiotools/plus_txdiag.py --health) -----------------------------------------------------------------------

RE_VIEW = re.compile(r'^--- view (\w+) \((\d+)\) status (-?\d+)')


def parse_health(text):
    """Key=value lines of the three debug-link views (tasks, crash, reset). Returns a dict:
    status {view: code}, uptime_ms {view: ms}, tasks {name: free bytes}, values {key: text} (every key as printed)"""
    out = {'status': {}, 'uptime_ms': {}, 'tasks': {}, 'values': {}}
    view = None
    for raw in text.splitlines():
        line = raw.strip()
        m = RE_VIEW.match(line)
        if m:
            view = m.group(1)
            out['status'][view] = int(m.group(3))
            continue
        key, sep, value = line.partition('=')
        if not sep or not key or ' ' in key:
            continue
        if key == 'uptime_ms' and view:
            out['uptime_ms'][view] = _int(value)
            continue
        out['values'][key] = value
        if key.startswith('tasks.free.'):
            out['tasks'][key[len('tasks.free.'):]] = _int(value)
    return out


def _int(text):
    try:
        return int(text, 0)
    except ValueError:
        return None


def health_summary(h):
    """The figures the live panel shows"""
    v = h['values']
    up = h['uptime_ms'].get('tasks', next(iter(h['uptime_ms'].values()), None))
    return {
        'uptime_ms': up,
        'heap_free': _int(v['tasks.heap_free']) if 'tasks.heap_free' in v else None,
        'heap_min_free': _int(v['tasks.heap_min_free']) if 'tasks.heap_min_free' in v else None,
        'min_task': v.get('tasks.min_task'),
        'min_free': _int(v['tasks.min_free']) if 'tasks.min_free' in v else None,
        'crash_valid': v.get('crash.valid') == '1',
        'crash_built': v.get('crash.built'),
        'crash_task': v.get('crash.task'),
        'crash_pc': v.get('crash.pc'),
        'crash_uptime_ms': _int(v['crash.uptime_ms']) if 'crash.uptime_ms' in v else None,
        'reset_cause': v.get('reset.cause'),
        'tasks': dict(sorted(h['tasks'].items(), key=lambda kv: (kv[1] is None, kv[1]))),
        'ok': bool(h['status']) and all(code == 0 for code in h['status'].values()),
    }


def format_uptime(ms):
    if ms is None:
        return '-'
    s = ms // 1000
    return f'{s // 3600:d}:{s // 60 % 60:02d}:{s % 60:02d}'


# --- Build (tools/build_radiofoundry.py through WSL) -----------------------------------------------------

RE_MEMORY = re.compile(r'^\s*(FLASH|RAM):\s+(\d+)\s*(\w+)\s+(\d+)\s*(\w+)\s+([\d.]+)%')
RE_SHA = re.compile(r'^([0-9a-f]{64})\s+\*?(\S*rfoundry_dm32\.bin)$')
RE_JSON_FLAG = re.compile(r'^\s*"(forced_rebuild_identical|compiler_warnings|plus_enabled)":\s*(true|false|\d+)')


def parse_build(text):
    """RAM/FLASH lines, the image hash and the manifest flags the build command prints"""
    out = {'memory': {}, 'memory_lines': [], 'sha256': None, 'forced_rebuild_identical': None,
           'compiler_warnings': None, 'plus_enabled': None, 'tle_line': None}
    for raw in text.splitlines():
        line = raw.rstrip()
        if line.startswith('== TLE: '):
            out['tle_line'] = line[len('== TLE: '):].lstrip('# ').strip()
            continue
        m = RE_MEMORY.match(line)
        if m:
            out['memory'][m.group(1)] = {'used': int(m.group(2)) * _unit(m.group(3)),
                                         'size': int(m.group(4)) * _unit(m.group(5)), 'percent': float(m.group(6))}
            out['memory_lines'].append(line.strip())
            continue
        m = RE_SHA.match(line.strip())
        if m:
            out['sha256'] = m.group(1)
            continue
        m = RE_JSON_FLAG.match(line)
        if m:
            value = m.group(2)
            out[m.group(1)] = value == 'true' if value in ('true', 'false') else int(value)
    return out


def _unit(u):
    return {'B': 1, 'KB': 1024, 'MB': 1024 * 1024}.get(u.upper(), 1)


# --- Live panel (radiotools/plus_live.py) ------------------------------------------------------------------------------

RE_BLOCK = re.compile(r'^--- (.+?) \((\S+)\) status (-?\d+)')
RE_POLL = re.compile(r'^=== (poll|end) (\d+)')
SCAN_FIELDS = 'version stops carriers cooled takes synced voiced ended tails quiet foreign sync_ms voice_ms'.split()


def parse_blocks(lines):
    """'--- NAME (ID) status CODE' blocks and their key=value lines: {name: {status, values, lines}}"""
    out = {}
    cur = None
    for raw in lines:
        line = raw.strip()
        m = RE_BLOCK.match(line)
        if m:
            cur = out[m.group(1)] = {'status': int(m.group(3)), 'values': {}, 'lines': [line]}
            continue
        if cur is None or RE_POLL.match(line):
            continue
        cur['lines'].append(line)
        key, sep, value = line.partition('=')
        if sep and key and ' ' not in key:
            cur['values'][key] = value
    return out


def _num(values, key):
    v = values.get(key)
    return _int(v) if v is not None else None


def s_meter(dbm):
    """VHF/UHF S-meter (IARU R.1: S9 = -93 dBm, 6 dB a unit): 'S7', 'S9+20', or '' without a reading"""
    if dbm is None or dbm == 0:
        return ''
    if dbm > -93:
        return 'S9+%d' % (dbm + 93)
    return 'S%d' % max(0, min(9, 9 - round((-93 - dbm) / 6)))


def live_summary(blocks, once=None):
    """What the live panel shows from one poll's blocks (and the crash/reset blocks read once at the start)"""
    blocks = dict(once or {}, **(blocks or {}))
    core = blocks.get('view core', {})
    cv = core.get('values', {}) if core.get('status') == 0 else {}
    rssi = _num(cv, 'rssi_dbm')
    signal = {
        'ok': bool(cv),
        'rssi_dbm': rssi if rssi not in (None, 0) else None,
        'noise': _num(cv, 'noise'),
        'state': cv.get('state'),
        'mode': cv.get('mode'),
        'zone': cv.get('zone_name'),
        'channel': cv.get('channel_name'),
        'channel_number': _num(cv, 'channel_number'),
        'vfo': cv.get('vfo') == '1',
        'rx_hz': _num(cv, 'rx_hz'),
        'tx_hz': _num(cv, 'tx_hz'),
        'squelch': _num(cv, 'squelch'),
        'monitor': cv.get('monitor') == '1',
        'uptime_ms': _num(cv, 'uptime_ms'),
    }
    nb = blocks.get('native', {})
    nv = nb.get('values', {}) if nb.get('status') == 0 else {}
    native = {'ok': bool(nv), 'status': nb.get('status'), 'state': nv.get('state_name'),
              'reason': nv.get('reason_name'), 'frequency_hz': _num(nv, 'frequency_hz'),
              'rx_count': _num(nv, 'rx_count'), 'rx_seen': _num(nv, 'rx_seen'), 'p25_sync': _num(nv, 'p25_sync'),
              'faults': _num(nv, 'faults')}
    sb = blocks.get('scan', {})
    sv = sb.get('values', {}) if sb.get('status') == 0 else {}
    scan = {k: _num(sv, k) for k in SCAN_FIELDS} if sv and 'raw' not in sv else None
    health_text = '\n'.join(l for name in ('view tasks', 'view crash', 'view reset') for l in
                            blocks.get(name, {}).get('lines', []))
    health = health_summary(parse_health(health_text)) if health_text else None
    return {'signal': signal, 'native': native, 'scan': scan, 'scan_status': sb.get('status'), 'health': health}


def format_mhz(hz):
    """434250000 -> '434,25000 MHz'"""
    if not hz:
        return '-'
    return ('%.5f MHz' % (hz / 1e6)).replace('.', ',')
