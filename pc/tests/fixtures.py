"""Synthetic test data: codeplug images built from the layout, a fake lab repo, a fake process runner, tool output.
Nothing here comes from a real backup."""
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
# Tests never touch the user's data folder (settings, backups, releases)
DATA_DIR = tempfile.mkdtemp(prefix='ddm32-test-')
os.environ['DDM32_DATA'] = DATA_DIR
os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')

from ddm32_app import codeplug as cp  # noqa: E402


def to_bcd(v, digits):
    w = 0
    for i in range(digits):
        w |= (v % 10) << (4 * i)
        v //= 10
    return w


def put_freq(rec, off, hz):
    rec[off:off + 4] = to_bcd(hz // 10, 8).to_bytes(4, 'little')


def put_name(buf, off, name, n=16):
    buf[off:off + n] = b'\xff' * n
    data = name.encode('latin-1')[:n]
    buf[off:off + len(data)] = data


def tone_word(text):
    if not text:
        return 0xFFFF
    if text.startswith('D'):
        code = int(text[1:4], 8)
        v = ((code >> 6) & 7) << 8 | ((code >> 3) & 7) << 4 | (code & 7)
        return v | 0x8000 | (0x4000 if text.endswith('I') else 0)
    return to_bcd(int(round(float(text) * 10)), 4)


def channel_record(name, rx, tx=None, mode=0, cc=0, rx_group=0, contact=0, ts2=False, flag4=0, power=0,
                   rx_tone='', tx_tone='', unit_id=0, tot=0):
    r = bytearray(cp.CH_SIZE)
    put_name(r, cp.CH_NAME, name)
    put_freq(r, cp.CH_RX, rx)
    put_freq(r, cp.CH_TX, tx if tx is not None else rx)
    r[cp.CH_MODE] = mode
    r[cp.CH_POWER] = power
    r[cp.CH_TOT] = tot
    r[cp.CH_RX_TONE:cp.CH_RX_TONE + 2] = tone_word(rx_tone).to_bytes(2, 'little')
    r[cp.CH_TX_TONE:cp.CH_TX_TONE + 2] = tone_word(tx_tone).to_bytes(2, 'little')
    r[cp.CH_CC] = cc
    r[cp.CH_RX_GROUP] = rx_group
    r[cp.CH_CONTACT:cp.CH_CONTACT + 2] = contact.to_bytes(2, 'little')
    r[cp.CH_FLAG2] = cp.FLAG2_TS2 if ts2 else 0
    r[cp.CH_FLAG4] = flag4
    if unit_id:
        r[cp.CH_LFLAG1] |= cp.LFLAG1_ID
        r[cp.CH_DMR_ID:cp.CH_DMR_ID + 3] = unit_id.to_bytes(3, 'big')
    else:
        r[cp.CH_DMR_ID + 1] = 0x16
    return bytes(r)


class ImageBuilder:
    def __init__(self):
        self.img = bytearray(b'\xff' * cp.IMAGE_SIZE)
        for bank in range(cp.CH_BANKS):
            a = cp.bank_bitmap(bank)
            self.img[a:a + 16] = bytes(16)
        self.img[cp.ZONE_BITMAP:cp.ZONE_BITMAP + 9] = bytes(9)
        self.img[cp.ZONE_LAYOUT] = 0x00
        self.img[cp.LIST_COUNTS:cp.LIST_COUNTS + cp.LIST_COUNT] = bytes(cp.LIST_COUNT)

    def channel(self, number, record):
        a = cp.channel_addr(number)
        self.img[a:a + cp.CH_SIZE] = record
        i = number - 1
        bm = cp.bank_bitmap(i // cp.CH_PER_BANK)
        k = i % cp.CH_PER_BANK
        self.img[bm + k // 8] |= 1 << (k % 8)

    def zone(self, slot, name, channels, cap=80):
        record = 16 + 2 * cap
        a = cp.ZONE_DATA + slot * record
        self.img[a:a + record] = bytes(record)
        put_name(self.img, a, name)
        for i, n in enumerate(channels):
            self.img[a + 16 + 2 * i:a + 18 + 2 * i] = n.to_bytes(2, 'little')
        self.img[cp.ZONE_BITMAP + slot // 8] |= 1 << (slot % 8)

    def contact(self, index, name, cid, kind=0, ts=None):
        a = cp.CONTACT_BASE + (index - 1) * cp.CONTACT_SIZE
        r = bytearray(cp.CONTACT_SIZE)
        put_name(r, 0, name)
        r[0x10:0x14] = to_bcd(cid, 8).to_bytes(4, 'big')
        r[0x14] = kind
        r[0x17] = 0x01 if ts is None else (ts - 1) << 1
        self.img[a:a + cp.CONTACT_SIZE] = r

    def rx_list(self, index, name, contacts):
        a = cp.LIST_BASE + (index - 1) * cp.LIST_SIZE
        r = bytearray(cp.LIST_SIZE)
        put_name(r, 0, name)
        for i, c in enumerate(contacts):
            r[16 + 2 * i:18 + 2 * i] = c.to_bytes(2, 'little')
        self.img[a:a + cp.LIST_SIZE] = r
        self.img[cp.LIST_COUNTS + index - 1] = len(contacts) + 1

    def general(self, name, dmr_id):
        put_name(self.img, cp.GEN_CALLSIGN, name, 8)
        self.img[cp.GEN_DMR_ID:cp.GEN_DMR_ID + 4] = to_bcd(dmr_id, 8).to_bytes(4, 'big')

    def bytes(self):
        return bytes(self.img)


def sample_image():
    """Every channel mode the Plus firmware knows, with the special values"""
    b = ImageBuilder()
    b.channel(1, channel_record('FM Simplex', 145_500_000, flag4=cp.FLAG4_25K, rx_tone='67.0', tx_tone='D023I',
                                power=9))
    b.channel(2, channel_record('DMR TG91', 438_500_000, 430_900_000, mode=1, cc=1, rx_group=1, contact=1, ts2=True,
                                power=1))
    b.channel(3, channel_record('DMR Any', 438_600_000, mode=1, cc=0x80 | 0x40 | 0x20 | 3, rx_group=2))
    b.channel(4, channel_record('P25 659', 434_250_000, mode=0xA5, cc=0xD0 | 0x2, rx_group=0x93, contact=101,
                                unit_id=1234567))
    b.channel(5, channel_record('P25 Any', 434_250_000, mode=0xA5, cc=0xD0 | 0xF, rx_group=0x7E, contact=0,
                                flag4=cp.FLAG4_RX_ONLY))
    b.channel(6, channel_record('YSF', 144_640_000, mode=0xA6, rx_group=12, flag4=cp.FLAG4_RX_ONLY))
    b.channel(7, channel_record('NXDN', 446_100_000, mode=0xA7, rx_group=0, flag4=cp.FLAG4_RX_ONLY))
    b.channel(8, channel_record('dPMR', 446_106_250, mode=0xA8, rx_group=5, flag4=cp.FLAG4_RX_ONLY))
    b.channel(130, channel_record('Bank1 FM', 145_600_000, flag4=cp.FLAG4_ZONE_SKIP, tot=4))
    # a marked slot with junk frequencies: not a channel
    bad = bytearray(channel_record('Bad', 145_000_000))
    bad[cp.CH_RX:cp.CH_RX + 4] = b'\xab\xcd\xef\x12'
    b.channel(9, bytes(bad))
    b.zone(0, 'Zone A', [1, 2, 4, 130])
    b.zone(3, 'Zone B', [5, 6, 999])    # 999 is not a channel: left out
    b.contact(1, 'TG 91', 91)
    b.contact(2, 'Friend', 2860001, kind=1, ts=2)
    b.contact(3, 'All', 0, kind=2)
    b.rx_list(1, 'List 1', [1, 3])
    b.general('N0CALL', 2860000)
    return b.bytes()


def write_backup(folder, image, verified=True):
    folder.mkdir(parents=True, exist_ok=True)
    (folder / 'q128-pass1.bin').write_bytes(image)
    (folder / 'progmem-pass1.bin').write_bytes(b'\xff' * 16)
    (folder / 'backup-manifest.json').write_text(json.dumps({'backup_verified': verified}))
    return folder


# --- Signed releases (test key from rtfixtures) ----------------------------------------------------------------------

def release_app(version):
    """A made-up application for a version (never a real build)"""
    return (b'DDM32 Plus test ' + version.encode()) * 64


def publish(releases_root, versions, download=True, channel=None, formats=None, min_compatible=None,
            tag=None, secret=None):
    """Writes a signed manifest for versions into the verified-manifest cache (as a GitHub check would) and, with
    download, the release folders (as a download would). Returns {version: (app bytes, FirmwareVersion entry)}"""
    import rtfixtures as rt
    root = Path(releases_root)
    out = {}
    entries = []
    for v in versions:
        app = release_app(v)
        sf, cf = (formats or {}).get(v, (1, 1))
        e = rt.firmware_entry(app, version=v, channel=(channel or {}).get(v, 'beta' if '-' in v else 'stable'),
                              settings_format=sf, codeplug_format=cf,
                              min_compatible=(min_compatible or {}).get(v), release=tag or f'v{v}')
        entries.append(e)
        out[v] = (app, e)
    tag = tag or f'v{versions[0]}'
    m = rt.manifest(entries, release=tag)
    rt.write_signed(root / 'manifests' / tag, m, secret or rt.TEST_SECRET)
    if download:
        for v in versions:
            folder = root / v
            rt.write_plus(folder, out[v][0], signed_for=False)
            rt.write_signed(folder, m, secret or rt.TEST_SECRET)
    return out


def write_progmem_backup(folder, program, data=b'\xff' * 16):
    """A 'verified' backup folder holding a given program image (for the app's own file reads)"""
    folder = Path(folder)
    folder.mkdir(parents=True, exist_ok=True)
    (folder / 'progmem-pass1.bin').write_bytes(program)
    (folder / 'q128-pass1.bin').write_bytes(data)
    (folder / 'backup-manifest.json').write_text(json.dumps({'backup_verified': True}))
    return folder


# --- Tool output (shapes copied from the tools' print statements) ---------------------------------------------------

BACKUP_OK = """READY: COM7 open; switch radio OFF then ON normally, cable seated, no keys held. Waiting 120s.
boot ROM answered; loading 7936-byte stub (31 blocks)
progmem: JEDEC c84014, 1048576 bytes
q128: JEDEC ef4018, 16777216 bytes
progmem pass 1: 50% (524288/1048576)
progmem pass 1: 100% (1048576/1048576)
q128 pass 1: 50% (8388608/16777216)
q128 pass 1: 100% (16777216/16777216)
progmem pass 2: 100% (1048576/1048576)
q128 pass 2: 45% (7602176/16777216)
q128 pass 2: 100% (16777216/16777216)
VERIFIED: both complete flash ranges read twice and matching. Power cycle the radio to start its firmware normally."""

BACKUP_TIMEOUT = """READY: COM7 open; switch radio OFF then ON normally, cable seated, no keys held. Waiting 120s.
Traceback (most recent call last):
  File "tools/dm32_readonly_backup.py", line 201, in main
    radio.start(wait=args.wait)
  File "research/upstream/radiofoundry/stub/stubtool.py", line 300, in start
    raise DumpError(f'the radio did not start its serial boot ROM within {wait} s: radio off and cable in')
DumpError: the radio did not start its serial boot ROM within 120 s: radio off and cable in before the script starts, then switch it on"""

PLAN_JSON = {"mode": "install_plus", "changed_sectors": 179, "first": "0xc000", "last": "0xbe000",
             "codec_slot_stock_tag": 47, "target_application_sha256": "ab" * 32, "boot_area_unchanged": True,
             "codec_slot_unchanged": True, "codec_slot_rewritten_from_backup": False,
             "codec_slot_prepared_from_backup": False, "data_chip_written": False}

PLAN_STOCK_JSON = {"mode": "install_plus_from_stock", "changed_sectors": 230, "first": "0xc000", "last": "0xfb000",
                   "codec_slot_stock_tag": 47, "target_application_sha256": "cd" * 32, "boot_area_unchanged": True,
                   "codec_slot_unchanged": False, "codec_slot_rewritten_from_backup": False,
                   "codec_slot_prepared_from_backup": True, "data_chip_written": False,
                   "stock_profile": "DM32.01.01.047.bin", "stock_version": "047", "stock_version_tested": True,
                   "slot_sectors": 22}

RESTORE_PLAN = {"mode": "restore_stock", "changed_sectors": 244, "first": "0xc000", "last": "0xff000",
                "target_application_sha256": "ef" * 32, "boot_area_unchanged": True, "codec_slot_unchanged": False,
                "codec_slot_rewritten_from_backup": True, "data_chip_written": False}

DETECT_CLASSIC = {"layout": "classic", "firmware": "ddm32", "codec_tag": 47, "stock_profile": None,
                  "stock_version": None, "stock_tested": False, "install_mode": "over_classic", "supported": True,
                  "reason": "", "application_end": "0xd0000"}
DETECT_STOCK = {"layout": "stock", "firmware": "stock", "codec_tag": 47, "stock_profile": "DM32.01.01.047.bin",
                "stock_version": "047", "stock_tested": True, "install_mode": "from_stock", "supported": True,
                "reason": "", "application_end": "0xe0574"}
DETECT_STOCK_049 = dict(DETECT_STOCK, stock_profile="DM32.01.01.049.bin", stock_version="049", stock_tested=False,
                        codec_tag=49)
DETECT_UNKNOWN = {"layout": "unknown", "firmware": "unknown", "codec_tag": None, "stock_profile": None,
                  "stock_version": None, "stock_tested": False, "install_mode": None, "supported": False,
                  "reason": "Unknown firmware: no codec slot and no known stock codec layout", "application_end": "0xc000"}

PLAN_UNSIGNED = """Traceback (most recent call last):
  File "radiotools/install_plus.py", line 96, in plus_application
    signed_manifest.release_gate(app, folder)
radiotools.signed_manifest.ManifestError: This application is not listed in the signed DDM32 Plus release manifest"""

INSTALL_OK = """{plan}
READY on COM7: turn the radio OFF, then ON normally, no buttons held. Waiting 600s.
boot ROM answered; loading 7936-byte stub (31 blocks)
Read current firmware: 50%
Read current firmware: 100%
Check settings chip: 100%
INSTALLING: 179 application sectors below the codec slot. Boot, slot and data writes blocked.
Written and sector-readback checked: 10% (18/179)
Written and sector-readback checked: 100% (179/179)
Read complete installed program chip: 100%
Recheck untouched settings chip: 100%
VERIFIED: application, untouched boot area, codec slot and settings chip all read back. Restarting.
PASS: DDM32 Plus installed and verified. Check the boot screen on the radio. No RF command was sent."""

INSTALL_TIMEOUT = """{plan}
READY on COM7: turn the radio OFF, then ON normally, no buttons held. Waiting 600s.
Traceback (most recent call last):
DumpError: the radio did not start its serial boot ROM within 600 s: radio off and cable in before the script starts, then switch it on"""

HEALTH = """--- view tasks (20) status 0
uptime_ms=82000
tasks.count=4
tasks.untracked=0
tasks.min_free=452
tasks.min_task=IDLE
tasks.free.IDLE=452
tasks.free.host=1200
tasks.free.ui=884
tasks.free.p25_lab=150
tasks.heap_free=6104
tasks.heap_min_free=4048
--- view crash (21) status 0
uptime_ms=82001
crash.built=1
crash.valid=0
--- view reset (23) status 0
uptime_ms=82002
reset.built=1
reset.cause=power
reset.flags=0x00000001
reset.uptime_ms=0
reset.warm=0"""

BUILD_OUT = """== rsync repository -> ~/ddm32-plus
== build
Codec cache: verified reuse
/home/u/ddm32-plus/build/radiofoundry-p25/build/dm32-p25-plus-public/rfoundry_dm32.elf
Memory region         Used Size  Region Size  %age Used
           FLASH:      730864 B       896 KB     79.65%
             RAM:      207424 B     211456 B     98.09%
== result
42072ce9912be26bf8c816812350d5148bdc5fb43f8e0a0571433724abb966bf  build/radiofoundry-p25/build/dm32-p25-plus-public/rfoundry_dm32.bin
  "plus_enabled": true,
  "forced_rebuild_identical": true,
  "compiler_warnings": 0,"""


def plan_text(plan=None):
    return json.dumps(plan or PLAN_JSON, indent=2)


def detect_text(d=None):
    return json.dumps(d or DETECT_CLASSIC, indent=2)


RESTORE_OK = """{plan}
READY on COM7: turn ON normally, no buttons held. Waiting 600s.
boot ROM answered; loading 7936-byte stub (31 blocks)
Using the verified saved backup. No pre-write firmware or settings-chip reads.
INSTALLING: 244 changed application sectors. Boot/data writes blocked.
Firmware written and sector-readback checked: 50% (122/244)
Firmware written and sector-readback checked: 100% (244/244)
Verify newly installed application: 100%
VERIFIED: installed application readback matches. Restarting.
PASS: stock application restored and verified byte for byte. Confirm the normal stock screen on the radio."""


def tle_set(name, catalog, l1_tail_epoch='26321.33455779'):
    """A valid three-line set (checksums computed)"""
    from ddm32_app.dataupdate import tle_checksum
    l1 = '1 %05dU 98067A   %s  .00009779  00000+0  18259-3 0  999' % (catalog, l1_tail_epoch)
    l2 = '2 %05d  51.6318 262.2236 0007381  35.3953 324.7525 15.4943095059549' % catalog
    l1 = l1[:68].ljust(68)
    l2 = l2[:68].ljust(68)
    return '%s\n%s%d\n%s%d\n' % (name, l1, tle_checksum(l1), l2, tle_checksum(l2))


try:
    from PyQt5.QtCore import QObject, pyqtSignal

    class FakeRunner(QObject):
        """Records start() calls; the test feeds lines and the exit code. Never runs anything"""
        line = pyqtSignal(str)
        finished = pyqtSignal(int)
        started = pyqtSignal()
        instances = []

        def __init__(self, parent=None):
            super().__init__(parent)
            self.calls = []
            self.killed = False
            FakeRunner.instances.append(self)

        def start(self, program, args, cwd=None):
            self.calls.append((program, list(args), cwd))

        def is_running(self):
            return bool(self.calls) and not self.killed

        def kill(self):
            self.killed = True

        def feed(self, text, code):
            for l in text.splitlines():
                self.line.emit(l)
            self.finished.emit(code)
except ImportError:  # pragma: no cover
    FakeRunner = None


def fake_ports():
    from ddm32_app.ports import PortInfo
    return [PortInfo('COM3', 'com0com - serial port emulator', hwid='COM0COM\\PORT'),
            PortInfo('COM4', 'Standard Serial over Bluetooth link', hwid='BTHENUM\\{0000}'),
            PortInfo('COM7', 'USB-SERIAL CH340', 0x1A86, 0x7523, 'USB VID:PID=1A86:7523')]
