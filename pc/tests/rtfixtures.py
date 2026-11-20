"""Shared fixtures for the radio tool tests: a test signing key (never a release key), signed manifests, synthetic
backups, a synthetic stock codec, and the pinned monitor's FakeRadio behind the real guards. Nothing here is real
radio data; no serial port is ever opened."""
import contextlib
import io
import json
import os
from pathlib import Path
import struct
import sys
import unittest
from unittest.mock import patch
import zlib

PC = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PC))
from radiotools import ed25519, signed_manifest as sm, upstream  # noqa: E402

START, SIZE, SECTOR, SLOT = 0xC000, 0x100000, 4096, 0xE8000
DATA_SIZE = 0x1000000
CODEC_SIG = (0x152514D8, 0x74C776CD)
CHIPS = {'progmem': {'jedec': 'c84014', 'bytes': SIZE}, 'q128': {'jedec': 'ef4018', 'bytes': DATA_SIZE}}

# A TEST key: its secret is in this file, so nothing it signs can be a release
TEST_SECRET = bytes(range(32))
TEST_PUBLIC_HEX = ed25519.secret_to_public(TEST_SECRET).hex()
OTHER_SECRET = bytes(range(1, 33))

need_upstream = unittest.skipUnless(upstream.available(), 'pinned RadioFoundry files not found (set DDM32_UPSTREAM)')


def use_test_key(case, key=TEST_PUBLIC_HEX):
    p = patch.object(sm, 'PUBLIC_KEY_HEX', key)
    p.start()
    case.addCleanup(p.stop)


def pattern(n, k):
    return bytes((i * k + (i >> 8) + 1) & 255 for i in range(n))


# --- manifests -----------------------------------------------------------------------------------------------------
def url(release, name):
    return f'{sm.URL_PREFIX}{release}/{name}'


def firmware_entry(app, version='1.0.0', channel='stable', settings_format=1, codeplug_format=1,
                   min_compatible=None, release=None, zip_bytes=None, notes=None):
    release = release or f'v{version}'
    name = f'ddm32-plus-{version}.zip'
    z = zip_bytes if zip_bytes is not None else b'zip:' + app[:16]
    return {'version': version, 'channel': channel, 'file': name, 'url': url(release, name),
            'sha256': sm.sha256(z), 'size': len(z), 'application_sha256': sm.sha256(app),
            'application_size': len(app), 'settings_format': settings_format, 'codeplug_format': codeplug_format,
            'min_compatible_version': min_compatible or version,
            'notes': notes or {'tr': f'Sürüm {version}', 'en': f'Version {version}'}}


def app_entry(version, zip_bytes, release=None):
    release = release or f'v{version}'
    name = f'DDM32Plus-{version}.zip'
    return {'version': version, 'file': name, 'url': url(release, name), 'sha256': sm.sha256(zip_bytes),
            'size': len(zip_bytes), 'notes': {'tr': 'Program', 'en': 'Program'}}


def manifest(firmware, app=None, release='v1.0.0'):
    m = {'format': sm.FORMAT, 'repo': sm.REPO, 'release': release, 'created_utc': '2026-11-20T00:00:00Z',
         'firmware': list(firmware)}
    if app is not None:
        m['app'] = app
    return m


def signed(m, secret=TEST_SECRET):
    """(manifest bytes, signature text)"""
    data = (json.dumps(m, indent=2, ensure_ascii=False) + '\n').encode('utf-8')
    return data, sm.sign_bytes(data, secret)


def write_signed(folder, m, secret=TEST_SECRET):
    folder = Path(folder)
    folder.mkdir(parents=True, exist_ok=True)
    data, sig = signed(m, secret)
    (folder / sm.MANIFEST_NAME).write_bytes(data)
    (folder / sm.SIG_NAME).write_text(sig)
    return data, sig


# --- codec slot, backups and release folders -----------------------------------------------------------------------
def make_slot(tag=47, slot_magic=0x43524444):
    """A codec slot v2 exactly as hrc7000_codec builds it (codec_slot_t, 128-byte header)"""
    code = struct.pack('<2I', *CODEC_SIG) + pattern(0x800 - 8, 5)
    tables = pattern(0x302, 11)
    rels = struct.pack('<3I', 8, 0x40, 0x100)
    code_off = 128
    tab_off = code_off + len(code)
    rel_off = tab_off + ((len(tables) + 3) & ~3)
    crc = zlib.crc32(rels, zlib.crc32(tables, zlib.crc32(code))) & 0xffffffff
    header = struct.pack('<8I7I4I', slot_magic, 2, code_off, len(code), 0x16cdc, tab_off, len(tables),
                         0x18000e48, *range(0x10, 0x80, 0x10)[:7], tag, crc, rel_off, len(rels) // 4)
    header += bytes(128 - len(header))
    return header + code + tables + b'\xff' * (rel_off - tab_off - len(tables)) + rels


def make_program(slot=None, app=None):
    boot = pattern(START, 7)
    app = pattern(0x9000, 3) if app is None else app
    slot = make_slot() if slot is None else slot
    program = boot + app + b'\xff' * (SLOT - START - len(app)) + slot
    return program + b'\xff' * (SIZE - len(program))


def make_data():
    data = bytearray(b'\xff' * DATA_SIZE)
    data[:SECTOR] = pattern(SECTOR, 13)
    data[0x800000:0x800000 + 3 * SECTOR] = pattern(3 * SECTOR, 17)
    return bytes(data)


def write_backup(folder, program, data, chips=CHIPS):
    folder = Path(folder)
    folder.mkdir(parents=True)
    captures = []
    for memory, image in (('progmem', program), ('q128', data)):
        for n in (1, 2):
            name = f'{memory}-pass{n}.bin'
            (folder / name).write_bytes(image)
            captures.append({'file': name, 'bytes': len(image), 'sha256': sm.sha256(image),
                             'sector_sha256': [sm.sha256(image[i:i + SECTOR]) for i in range(0, len(image), SECTOR)]})
    m = {'backup_verified': True, 'captures': captures}
    if chips is not None:
        m['chips'] = chips
    (folder / 'backup-manifest.json').write_text(json.dumps(m))


CODEC_SYMBOLS = {'__codec_slot': 0x030e8000, '__codec_slot_end': 0x03100000, '__codec_run_lo': 0x43a00,
                 '__codec_run_hi': 0x58000, '__codec_tab_lo': 0x18000e48, '__codec_tab_hi': 0x18003e38}
MAP_LINE = '                0x{value:08x}                {name} = 0x{value:x}\n'


def map_text(symbols=None):
    symbols = dict(CODEC_SYMBOLS, **(symbols or {}))
    text = '\nMemory Configuration\n\n .text          0x0300c000     0x7000\n'
    for name, value in symbols.items():
        if value is not None:
            text += MAP_LINE.format(name=name, value=value)
    return text.encode()


def write_plus(folder, app, symbols=None, plus=True, identical=True, app_digest=None, map_digest=None,
               signed_for=None, secret=TEST_SECRET):
    """A release folder: rfoundry_dm32.bin/.json/.map and (unless signed_for is False) the signed manifest listing
    signed_for (default: this app)"""
    folder = Path(folder)
    folder.mkdir(parents=True, exist_ok=True)
    (folder / 'rfoundry_dm32.bin').write_bytes(app)
    mapping = map_text(symbols)
    (folder / 'rfoundry_dm32.map').write_bytes(mapping)
    (folder / 'rfoundry_dm32.json').write_text(json.dumps({
        'plus_enabled': plus, 'classic_enabled': True, 'forced_rebuild_identical': identical, 'public_enabled': True,
        'outputs': {'rfoundry_dm32.bin': app_digest or sm.sha256(app),
                    'rfoundry_dm32.map': map_digest or sm.sha256(mapping)}}))
    if signed_for is not False:
        listed = app if signed_for is None else signed_for
        write_signed(folder, manifest([firmware_entry(listed)]), secret)
    return folder


# --- a synthetic stock codec for the pinned codec tool -------------------------------------------------------------
STOCK_CODE_AT, STOCK_TABLES_AT = 0x030C68B8, 0x03072658       # where stock .047 keeps them
STOCK_CODE_LEN, STOCK_TABLES_LEN = 0x2000, 0x2F78
ENTRIES = {'decode': 0x0, 'encode': 0x260, 'dec_init': 0xB4, 'dec_set': 0x84, 'enc_init': 0x314, 'enc_set': 0x2E4,
           'fec_init': 0x1BF8}


def synthetic_codec(name='DM32.01.01.047.bin', tag=47):
    """(code, tables, refs): a made-up codec with the signature words and three planted self-references"""
    run = 0x16CDC
    code = bytearray(pattern(STOCK_CODE_LEN, 29))
    struct.pack_into('<2I', code, 0, *CODEC_SIG)
    refs = [(0x40, run + 0x100), (0x84, run), (0x1000, run + STOCK_CODE_LEN - 4)]
    for off, v in refs:
        struct.pack_into('<I', code, off, v)
    tables = pattern(STOCK_TABLES_LEN, 31)
    return bytes(code), tables, refs


def stock_program(code, tables, app_seed=19):
    """A 1 MiB stock-layout program flash: boot, a made-up stock application, the codec and tables where .047 keeps
    them, no slot"""
    p = bytearray(pattern(START, 7) + pattern(0xD4574, app_seed) + b'\xff' * (SIZE - START - 0xD4574))
    c, t = STOCK_CODE_AT - 0x03000000, STOCK_TABLES_AT - 0x03000000
    p[c:c + len(code)] = code
    p[t:t + len(tables)] = tables
    return bytes(p)


@contextlib.contextmanager
def synthetic_stock_codec(unit, name='DM32.01.01.047.bin', tag=47):
    """The real pinned codec tool, its profiles and codec maps replaced by one synthetic stock profile; unit.load_codec
    returns it. Yields (codec module, code, tables, program)"""
    codec = unit.load_codec()
    code, tables, refs = synthetic_codec(name, tag)
    prof = {'name': name, 'board': 'dm32', 'tag': tag, 'size': 869748,
            'code': (STOCK_CODE_AT, len(code), 0x16CDC), 'tables': (STOCK_TABLES_AT, len(tables), 0x18000EA8),
            'code_sha256': codec.sha256(code), 'tables_sha256': codec.sha256(tables),
            'tables_head_crc': zlib.crc32(tables[:64]), 'entries': dict(ENTRIES)}
    maps = codec.synthetic_map(prof, code, refs)
    with patch.dict(codec.PROFILES, {'f' * 64: prof}, clear=True), \
            patch.object(codec.codecmap, 'load_maps', lambda *a, **k: maps), \
            patch.object(unit, 'load_codec', lambda: codec):
        yield codec, code, tables, stock_program(code, tables)


# --- the pinned monitor's FakeRadio behind the real guards ---------------------------------------------------------
def fake_radio(m, program, data, corrupt_after_writes=None, data_jedec='ef4018'):
    univ = m.stub_univ_image()
    lay = m.univ_layout(univ)

    class Radio(m.FakeRadio):
        rts = dtr = port = None
        opened = closed = False
        corrupt = corrupt_after_writes
        ops = []

        def open(self):
            self.opened = True

        def close(self):
            self.closed = True

        def step(self):
            ib = self.inb
            if self.running and len(ib) >= 6 and ib[0] == 2:
                n = (ib[2] << 8) | ib[3]
                if len(ib) >= 6 + n:
                    op, payload = ib[1], bytes(ib[6:6 + n])
                    self.ops.append(op)
                    if op in (7, 9) and self.writes and self.corrupt is not None:
                        image = bytearray(self.images['progmem'])
                        image[self.corrupt] ^= 0x01
                        self.images['progmem'] = bytes(image)
                        self.corrupt = None
                    if op == 5 and payload[:6] == bytes.fromhex('140a00000312') and payload[10:] == b'\x9f':
                        del ib[:6 + n]
                        self.requests += 1
                        self.out += self.frame(m.OK, bytes.fromhex(self.data_jedec))
                        return True
            return super().step()

    radio = Radio({'progmem': program, 'q128': data}, stub_len=(len(univ) + m.BLOCK - 1) // m.BLOCK * m.BLOCK)
    radio.ops = []
    radio.data_jedec = data_jedec
    radio.univ = {'run': 0x30000, 'end': lay[1], 'board': 0, 'flags': 0, 'honor': True}
    return radio


def run_tool_main(module, argv0, args, radio=None):
    """module.main() with argv; the serial port is the simulated radio (or must not be opened at all).
    Returns the printed output"""
    def no_port(*a, **k):
        raise AssertionError('serial port opened')
    out = io.StringIO()
    factory = (lambda *a, **k: radio) if radio is not None else no_port
    with patch.object(sys, 'argv', [argv0, '--port', 'SIMULATED', *map(str, args)]), \
            patch('serial.Serial', factory), contextlib.redirect_stdout(out), \
            contextlib.redirect_stderr(io.StringIO()):
        try:
            module.main()
        finally:
            run_tool_main.output = out.getvalue()
    return out.getvalue()


def env_without(*names):
    return {k: v for k, v in os.environ.items() if k not in names}
