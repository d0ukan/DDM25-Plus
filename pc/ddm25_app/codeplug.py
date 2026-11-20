"""Read-only decoder of the DDM25 Plus codeplug in a data-flash (q128, 16 MiB) backup image.

Layout: the open-firmware codeplug RadioFoundry/Classic/Plus keep in the data flash (upstream
core/codeplug/codeplug.c, boards/dm32/board.c flash map), plus the DDM25 Plus channel modes
(tools/classic_channels.py, plus_features.py):

  EEPROM image at 0x700000, flash image at 0x720000
  channels: bank 0 (1-128) bitmap EE+0x3780, banks 1-7 (129-1024) from FL+0x7B1B0, stride 16 + 128 x 56
  zones: bitmap EE+0x8010 (68 slots), records EE+0x8030, layout byte EE+0x806F (<= 4: 80 channels, else 16)
  contacts: FL+0x87620, 1024 x 24;  RX group lists: counts FL+0x8D620 (76), records FL+0x8D6A0, 76 x 80
  general: radio name EE+0xE0 (8 chars), DMR ID EE+0xE8 (BCD)

Channel modes (CH_MODE, record offset 0x18): 0 FM, 1 DMR (CH_CC high bits 0x80 any CC, 0x40 any TS, 0x20 every TG),
0xA6 YSF (DG-ID in CH_RX_GROUP, 0 any), 0xA7 NXDN (RAN in CH_RX_GROUP & 0x3F, 0 any), 0xA8 dPMR (CH_RX_GROUP: 0 any,
else colour code + 1).

P25 (DDM25 Plus on Classic r13, tools/classic_standard.py, third_party/rf_codeplug): a DMR base record
(mode 1, receive only) plus an RFKV tag of kind 2 (P25) in the codeplug settings area. The area starts at EE+0xE000
(16-byte header: "RFKV", version 1, header length, data length u16, CRC-32 u32, the next page u32; entries key u16,
length u16, value); further pages at combined-image offsets 0xAF000-0xCEFFF = FL + offset - 0x20000. A channel's tag
is key 0x0400 + number - 1: kind, system, name check u16 (FNV-1a of the 16-byte name, halves XORed), flags (0x01 TX
allowed, 0x02 the base stands alone, 0x04 a fallback follows: 4 more bytes), then the P25 block: squelch (1 Selective:
the talkgroup filters; 0 Normal: every group on the RX NAC), RX NAC u16 (0x8000 | NAC; 0 = $293), TX NAC u16 (0x8000
| NAC; 0 = "Same as RX": the RX NAC, $293 when RX is Any), byte 5 bit 0 a talkgroup follows (u16 at 6), else the
record's contact. The tag counts when its name check matches and the record is DMR receive only (or flag 0x02). Unit
ID: the record's CH_DMR_ID, else the radio's.

Read compatibility (Plus reads these, saves them in the RFKV form): CH_MODE 0xA5, the P25 record of Classic r4-r7 and
the r7-based Plus (RX NAC = (CH_CC & 0xF) << 8 | CH_RX_GROUP, CH_CC high nibble 0xD; TG = CH_CONTACT; unit ID =
CH_DMR_ID; TX NAC "Same as RX": the r7-based Plus's TX NAC tag 0x5000 | NAC in CH_TX_TONE is no longer read; it was
never written on the radio). Mode 2 is P25 in the DM32-nxdn-p25 multi-mode derivative's format (CONFIG_P25_MODE2,
docs/plus.md): RX / TX NAC in the tone words CH_RX_TONE / CH_TX_TONE (a 12-bit number, shown in hex: "293"
is $293 = 659), TG = CH_CONTACT, unit ID = CH_DMR_ID, CH_FLAG2 bit 6 every talkgroup (the Normal squelch). Plus keeps
both NACs (a TX NAC $F7E and up reads as the derivative's default $293); no talkgroup or no unit ID make it RX only.

Nothing here writes to a radio. The app edits these rows in memory and saves CSV / JSON change lists only.
"""
import csv
import io
import json
import time
import zlib

EE_BASE = 0x700000
FL_BASE = 0x720000

CH_PER_BANK = 128
CH_BANKS = 8
CH_SIZE = 56
BANK0_BITMAP = EE_BASE + 0x3780
BANK1_BITMAP = FL_BASE + 0x7B1B0
BANK_STRIDE = 16 + CH_PER_BANK * CH_SIZE

CH_NAME, CH_RX, CH_TX, CH_MODE, CH_POWER, CH_TOT = 0x00, 0x10, 0x14, 0x18, 0x19, 0x1B
CH_RX_TONE, CH_TX_TONE, CH_LFLAG1, CH_DMR_ID = 0x20, 0x22, 0x26, 0x27
CH_RX_GROUP, CH_CC, CH_CONTACT, CH_FLAG2, CH_FLAG4 = 0x2B, 0x2C, 0x2E, 0x31, 0x33
FLAG2_TS2 = 0x40
FLAG4_VOX, FLAG4_ZONE_SKIP, FLAG4_RX_ONLY, FLAG4_25K = 0x40, 0x20, 0x04, 0x02
LFLAG1_ID = 0x80

ZONE_SLOTS = 68
ZONE_BITMAP = EE_BASE + 0x8010
ZONE_DATA = EE_BASE + 0x8030
ZONE_LAYOUT = EE_BASE + 0x806F

CONTACT_BASE = FL_BASE + 0x87620
CONTACT_SIZE = 24
CONTACT_COUNT = 1024
LIST_COUNTS = FL_BASE + 0x8D620
LIST_BASE = FL_BASE + 0x8D6A0
LIST_SIZE = 80
LIST_COUNT = 76

GEN_CALLSIGN = EE_BASE + 0xE0
GEN_DMR_ID = EE_BASE + 0xE8

MODES = {0: 'FM', 1: 'DMR', 2: 'P25', 0xA5: 'P25', 0xA6: 'YSF', 0xA7: 'NXDN', 0xA8: 'dPMR'}
MODE_P25_OLD = 2        # the multi-mode derivative's P25 record (read only: Plus saves such a channel in the RFKV form)
MODE_P25_R7 = 0xA5      # Classic r4-r7 / the r7-based Plus (read only, as above)
# The mode byte an edit writes: P25 is a DMR base record (mode 1) plus its RFKV tag since the Classic r13 base
MODE_BYTES = {'FM': 0, 'DMR': 1, 'P25': 1, 'YSF': 0xA6, 'NXDN': 0xA7, 'dPMR': 0xA8}
POWER_NAMES = ('L-', 'L', 'L+', 'L++', 'M', 'M+', 'M++', 'H-', 'H')   # boards/dm32/board.c
CONTACT_TYPES = ('Grup', 'Özel', 'Herkes')
NAC_ANY = 0xF7E
NAC_DEFAULT = 0x293
TXNAC_SAME = 'RX ile aynı'

# The RFKV codeplug settings area (third_party/rf_codeplug/core/codeplug/codeplug_kv.[ch], codeplug_tag.h)
KV_ROOT = EE_BASE + 0xE000
KV_MAGIC = 0x564B4652   # "RFKV"
KV_VERSION = 1
KV_BANK_A, KV_BANK_END = 0xAF000, 0xCF000
KV_PAGE = 0x1000
KV_PAGES = 33
KEY_CH_TAG = 0x0400     # + channel number - 1
KEY_MODE_SET = 0x0A00   # + kind: the kind's radio-wide record (P25: the unit ID, u32)
KIND_P25 = 2
TAG_TX, TAG_BASE_OWN, TAG_FALLBACK = 0x01, 0x02, 0x04
P25_NAC_SET = 0x8000

IMAGE_SIZE = 1 << 24


class CodeplugError(ValueError):
    pass


def le16(b, o):
    return b[o] | (b[o + 1] << 8)


def le32(b, o):
    return int.from_bytes(b[o:o + 4], 'little')


def bcd(word, digits):
    v = 0
    for i in range(digits - 1, -1, -1):
        d = (word >> (4 * i)) & 0xF
        if d > 9:
            return None
        v = v * 10 + d
    return v


def freq_hz(rec, off):
    v = bcd(le32(rec, off), 8)
    return None if not v else v * 10


def bcd_id(b, o):
    v = bcd(int.from_bytes(b[o:o + 4], 'big'), 8)
    return 0 if v is None or v > 0xFFFFFF else v


def text(b, o, n):
    out = []
    for c in b[o:o + n]:
        if c in (0x00, 0xFF):
            break
        out.append(c)
    return bytes(out).decode('latin-1')


def tone(v):
    """'' none, '67.0' CTCSS, 'D023N' / 'D023I' DCS (codeplug.c tone())"""
    if v in (0, 0xFFFF):
        return ''
    if v & 0x8000:
        code = 0
        for i in (2, 1, 0):
            d = (v >> (4 * i)) & 0xF
            if d > 7:
                return ''
            code = code * 8 + d
        return 'D%03o%s' % (code, 'I' if v & 0x4000 else 'N')
    if v & 0x4000:
        return ''
    tenths = bcd(v, 4)
    if tenths is None or not 300 <= tenths <= 3000:
        return ''
    return '%d.%d' % (tenths // 10, tenths % 10)


def power_name(byte):
    """The qdmr 10-step power byte as the DM32's 9 levels (codeplug.c power_from_byte, power_count 9)"""
    if byte == 0:
        return 'Varsayılan'
    step = min(byte - 1, len(POWER_NAMES) - 1)
    return POWER_NAMES[step]


def nac_text(nac):
    """As the radio shows it (CONFIG_NAC_DEC): '$293 = 659'; F7E is any NAC"""
    if nac == NAC_ANY:
        return 'Herhangi ($F7E)'
    return '$%03X = %d' % (nac, nac)


def kv_page_addr(off):
    """A page of the RFKV chain: its data-flash address (codeplug_kv.c page_addr), None when off is no page"""
    if off == 0xE000:
        return KV_ROOT
    if KV_BANK_A <= off < KV_BANK_END and not off % KV_PAGE:
        return FL_BASE + off - 0x20000
    return None


def read_kv(image):
    """The RFKV codeplug settings area, every page checked as the firmware checks it (magic, version, lengths,
    CRC-32): {key: value bytes}, the first entry of a key counting. {} when there is none or it is damaged"""
    out, off, seen = {}, 0xE000, set()
    for _ in range(KV_PAGES):
        a = kv_page_addr(off)
        if a is None or off in seen:
            return {}
        seen.add(off)
        h = image[a:a + 16]
        if le32(h, 0) != KV_MAGIC or h[4] != KV_VERSION or h[5] < 16 or h[5] + le16(h, 6) > KV_PAGE:
            return {}
        data = image[a + h[5]:a + h[5] + le16(h, 6)]
        if zlib.crc32(data) != le32(h, 8):
            return {}
        at = 0
        while at < len(data):
            if len(data) - at < 4 or le16(data, at + 2) > len(data) - at - 4:
                return {}
            key, n = le16(data, at), le16(data, at + 2)
            out.setdefault(key, bytes(data[at + 4:at + 4 + n]))
            at += 4 + n
        off = le32(h, 12)
        if not off:
            return out
    return {}


def name_check(raw_name):
    """codeplug_tag.c codeplug_name_check over a record's 16 name bytes (0xFF padding ends the name)"""
    h, end = 2166136261, False
    for c in raw_name[:16]:
        c = 0 if end or c == 0xFF else c
        end = end or c == 0
        h = ((h ^ c) * 16777619) & 0xFFFFFFFF
    return (h ^ (h >> 16)) & 0xFFFF


def p25_tag(kv, number, r):
    """A channel's live RFKV tag (codeplug_tag.c): (kind, flags, parameter bytes) or None"""
    v = kv.get(KEY_CH_TAG + number - 1)
    if not v or len(v) < 5 or v[0] in (0, 0xFF):
        return None
    at = 9 if v[4] & TAG_FALLBACK else 5
    if len(v) < at or le16(v, 2) != name_check(r[CH_NAME:CH_NAME + 16]):
        return None
    if not (v[4] & TAG_BASE_OWN or (r[CH_MODE] == 1 and r[CH_FLAG4] & FLAG4_RX_ONLY)):
        return None
    return v[0], v[4], v[at:]


def nac_field(f, default):
    """p25_codeplug.c nac_field: a stored NAC field's value when set, else default"""
    return f & 0xFFF if f & P25_NAC_SET and not f & 0x7000 else default


def mhz(hz):
    return '' if hz is None else '%.5f' % (hz / 1e6)


# Channel table columns: key, Turkish header
CHANNEL_COLUMNS = (
    ('number', 'No'), ('name', 'Ad'), ('freq', 'Frekans'), ('mode', 'Mod'), ('summary', 'Ayar'),
    ('rx_mhz', 'RX MHz'), ('tx_mhz', 'TX MHz'),
    ('bw', 'Bant'), ('power', 'Güç'), ('rx_only', 'Sadece RX'), ('rx_tone', 'RX ton'), ('tx_tone', 'TX ton'),
    ('cc', 'CC'), ('ts', 'TS'), ('nac', 'NAC'), ('tx_nac', 'TX NAC'), ('tg', 'TG'), ('contact', 'Kişi'), ('rx_list', 'RX grup listesi'),
    ('code', 'DG-ID / RAN / CC'), ('unit_id', 'ID'), ('scan_skip', 'Taramada atla'), ('tot_s', 'TOT sn'),
    ('mode_byte', 'CH_MODE'),
)


def decode_channel(number, r, kv=None, radio_id=0):
    """One 56-byte channel record as a row dict, or None when the frequencies are malformed (not a channel). kv: the
    RFKV area (read_kv), whose tags make a DMR record a P25 channel; radio_id: the radio's DMR ID"""
    rx, tx = freq_hz(r, CH_RX), freq_hz(r, CH_TX)
    if rx is None or tx is None:
        return None
    mb = r[CH_MODE]
    mode = MODES.get(mb) or 'Bilinmiyor (0x%02X)' % mb
    tag = p25_tag(kv or {}, number, r) if mb == 1 else None
    if tag is not None:
        mode = 'P25' if tag[0] == KIND_P25 else 'Bilinmiyor (RFKV tür %d)' % tag[0]
    cc_byte = r[CH_CC]
    row = {k: '' for k, _ in CHANNEL_COLUMNS}
    row.update(number=number, name=text(r, CH_NAME, 16), rx_mhz=mhz(rx), tx_mhz=mhz(tx), mode=mode,
               mode_byte='0x%02X' % mb, bw='25 kHz' if r[CH_FLAG4] & FLAG4_25K else '12.5 kHz',
               power=power_name(r[CH_POWER]), rx_only='Evet' if r[CH_FLAG4] & FLAG4_RX_ONLY else '',
               scan_skip='Evet' if r[CH_FLAG4] & FLAG4_ZONE_SKIP else '', tot_s=str(r[CH_TOT] * 15) if r[CH_TOT] else '')
    unit = (r[CH_DMR_ID] << 16 | r[CH_DMR_ID + 1] << 8 | r[CH_DMR_ID + 2]) if r[CH_LFLAG1] & LFLAG1_ID else 0
    if mode == 'FM':
        row.update(rx_tone=tone(le16(r, CH_RX_TONE)), tx_tone=tone(le16(r, CH_TX_TONE)))
    elif mode == 'DMR':
        row['cc'] = 'Herhangi' if cc_byte & 0x80 else str(cc_byte & 0x0F)
        row['ts'] = 'Herhangi' if cc_byte & 0x40 else ('2' if r[CH_FLAG2] & FLAG2_TS2 else '1')
        row['contact'] = str(le16(r, CH_CONTACT) or '')
        row['rx_list'] = 'Herhangi (tüm TG)' if cc_byte & 0x20 else str(r[CH_RX_GROUP] or '')
        row['unit_id'] = str(unit or '')
        if cc_byte & 0xC0:
            row['rx_only'] = 'Evet'   # any CC / TS: the firmware allows no TX
    elif mode == 'P25' and tag is not None:
        # Classic r13's form (classic_standard.inc): the DMR base record and the RFKV P25 block
        kind, flags, p = tag
        squelch = p[0] & 3 if p else 0
        rx_nac = nac_field(le16(p, 1) if len(p) >= 3 else 0, NAC_DEFAULT)
        if rx_nac == 0xF7F:
            rx_nac = NAC_DEFAULT
        tx_field = le16(p, 3) if len(p) >= 5 else 0
        row['mode_byte'] = '0x01 + RFKV'
        row['nac'] = nac_text(rx_nac)
        row['tx_nac'] = TXNAC_SAME if not tx_field & P25_NAC_SET else nac_text(nac_field(tx_field, rx_nac))
        if len(p) >= 8 and p[5] & 1:
            tg = le16(p, 6)
            row['tg'] = str(tg) if tg else 'Herhangi'
        else:
            row['tg'] = 'kişi %d' % le16(r, CH_CONTACT) if le16(r, CH_CONTACT) else '1'
        if squelch != 1:
            row['tg'] += ' (tüm gruplar)'
        p25_id = 0
        mset = (kv or {}).get(KEY_MODE_SET + KIND_P25, b'')
        if len(mset) >= 4 and 1 <= le32(mset, 0) <= 0xFFFFFE:
            p25_id = le32(mset, 0)
        row['unit_id'] = str(unit or p25_id or radio_id or '')
        row['rx_only'] = '' if flags & TAG_TX else 'Evet'
        row['bw'] = '12.5 kHz'
    elif mode == 'P25' and mb == MODE_P25_OLD:
        # As the firmware reads it (codeplug.c p25_mode2): the derivative's special values, both NACs, RX only cases
        rx_nac, tx_nac = le16(r, CH_RX_TONE), le16(r, CH_TX_TONE)
        if rx_nac > 0xFFF or rx_nac == 0xF7F:
            rx_nac = NAC_ANY
        if tx_nac >= 0xF7E:
            tx_nac = 0x293
        tg = le16(r, CH_CONTACT)
        unit = unit if unit <= 0xFFFFFE else 0
        row['nac'] = nac_text(rx_nac) + '  (CH_MODE 2 biçimi)'
        row['tx_nac'] = TXNAC_SAME if tx_nac == rx_nac else nac_text(tx_nac)
        row['tg'] = str(tg) if tg else 'Herhangi'
        if tg and r[CH_FLAG2] & FLAG2_TS2:
            row['tg'] += ' (tüm gruplar)'     # every talkgroup: the Normal squelch, the talkgroup kept for TX
        row['unit_id'] = str(unit or '')
        row['bw'] = '12.5 kHz'
        if not tg or not unit:
            row['rx_only'] = 'Evet'
    elif mode == 'P25':
        # Classic r4-r7 / the r7-based Plus (0xA5): read as it read there, TX NAC Same as RX
        nac = ((cc_byte & 0x0F) << 8) | r[CH_RX_GROUP]
        row['nac'] = nac_text(nac) + ('' if cc_byte & 0xF0 == 0xD0 else '  (geçersiz kayıt)')
        row['tx_nac'] = TXNAC_SAME
        tg = le16(r, CH_CONTACT)
        row['tg'] = str(tg) if tg else 'Herhangi'
        row['unit_id'] = str(unit or '')
    elif mode == 'YSF':
        dg = r[CH_RX_GROUP] & 0x7F
        row['code'] = 'DG-ID %s' % (dg if dg else 'herhangi')
        row['rx_only'] = 'Evet'
    elif mode == 'NXDN':
        ran = r[CH_RX_GROUP] & 0x3F
        row['code'] = 'RAN %s' % (ran if ran else 'herhangi')
        row['rx_only'] = 'Evet'
    elif mode == 'dPMR':
        c = r[CH_RX_GROUP] & 0x7F
        row['code'] = 'CC %s' % (c - 1 if c else 'herhangi')
        row['rx_only'] = 'Evet'
    row['freq'] = channel_freq(row)
    row['summary'] = channel_summary(row)
    return row


# The simple view's columns: few, plain
SIMPLE_CHANNEL_KEYS = ('number', 'name', 'freq', 'mode', 'summary')
DERIVED_KEYS = ('freq', 'summary')


def channel_freq(row):
    rx, tx = row['rx_mhz'], row['tx_mhz']
    rx_s, tx_s = _short_mhz(rx), _short_mhz(tx)
    return rx_s if rx == tx else '%s / TX %s' % (rx_s, tx_s)


def _short_mhz(text):
    """'145.50000' -> '145.500', '446.10625' kept"""
    while text.endswith('0') and len(text.partition('.')[2]) > 3:
        text = text[:-1]
    return text


def channel_summary(row):
    """One short 'Ayar' text: CC/TS for DMR, NAC/TG for P25, tones for FM, the code for YSF/NXDN/dPMR"""
    mode = row['mode']
    if mode == 'FM':
        tones = [t for t in (row['rx_tone'], row['tx_tone']) if t]
        if not tones:
            parts = ['Ton yok']
        elif row['rx_tone'] == row['tx_tone']:
            parts = ['Ton ' + row['rx_tone']]
        else:
            parts = ['Ton RX %s / TX %s' % (row['rx_tone'] or '-', row['tx_tone'] or '-')]
    elif mode == 'DMR':
        parts = ['CC ' + row['cc'], 'TS ' + row['ts']]
        if row['rx_list'].startswith('Herhangi'):
            parts.append('tüm TG')
        elif row['contact']:
            parts.append('Kişi ' + row['contact'])
    elif mode == 'P25':
        nac = row['nac'] if row['tx_nac'] in ('', TXNAC_SAME) else row['nac'] + ' > TX ' + row['tx_nac']
        parts = ['NAC ' + nac, 'TG ' + row['tg']]
    elif row['code']:
        parts = [row['code']]
    else:
        parts = []
    if row['rx_only'] == 'Evet':
        parts.append('sadece RX')
    return ' · '.join(parts)


def bank_bitmap(bank):
    return BANK0_BITMAP if bank == 0 else BANK1_BITMAP + (bank - 1) * BANK_STRIDE


def channel_addr(number):
    i = number - 1
    return bank_bitmap(i // CH_PER_BANK) + 16 + (i % CH_PER_BANK) * CH_SIZE


class Codeplug:
    def __init__(self):
        self.channels = []
        self.zones = []
        self.contacts = []
        self.lists = []
        self.radio_name = ''
        self.dmr_id = 0
        self.zone_capacity = 80
        self.bad_records = 0
        self.rfkv_entries = 0   # entries in the RFKV codeplug settings area (0: none or damaged)

    def counts(self):
        return {'channels': len(self.channels), 'zones': len(self.zones), 'contacts': len(self.contacts),
                'rx_group_lists': len(self.lists)}

    def mode_counts(self):
        out = {}
        for c in self.channels:
            out[c['mode']] = out.get(c['mode'], 0) + 1
        return out


def decode(image):
    """Decode a 16 MiB data-flash image"""
    if len(image) != IMAGE_SIZE:
        raise CodeplugError('Veri flash imajı 16 MiB olmalı (q128-pass1.bin), bu %d bayt' % len(image))
    cp = Codeplug()
    used = set()
    kv = read_kv(image)
    cp.dmr_id = bcd_id(image, GEN_DMR_ID)
    cp.rfkv_entries = len(kv)
    for bank in range(CH_BANKS):
        bits = image[bank_bitmap(bank):bank_bitmap(bank) + CH_PER_BANK // 8]
        for k in range(CH_PER_BANK):
            if not bits[k // 8] & (1 << (k % 8)):
                continue
            number = bank * CH_PER_BANK + k + 1
            a = channel_addr(number)
            row = decode_channel(number, image[a:a + CH_SIZE], kv, cp.dmr_id)
            if row is None:
                if any(b != 0xFF for b in image[a:a + CH_SIZE]):
                    cp.bad_records += 1
                continue
            used.add(number)
            cp.channels.append(row)
    names = {c['number']: c['name'] for c in cp.channels}
    layout = image[ZONE_LAYOUT]
    cap = 80 if layout <= 4 else 16
    cp.zone_capacity = cap
    record = 16 + 2 * cap
    zbits = image[ZONE_BITMAP:ZONE_BITMAP + (ZONE_SLOTS + 7) // 8]
    for slot in range(ZONE_SLOTS):
        if not zbits[slot // 8] & (1 << (slot % 8)):
            continue
        a = ZONE_DATA + slot * record
        if image[a] in (0x00, 0xFF):
            continue
        members = []
        for i in range(cap):
            n = le16(image, a + 16 + 2 * i)
            if n == 0:
                break
            if n in used:
                members.append(n)
        cp.zones.append({'index': len(cp.zones) + 1, 'slot': slot, 'name': text(image, a, 16), 'count': len(members),
                         'channels': members,
                         'channel_names': ', '.join('%d %s' % (n, names.get(n, '')) for n in members)})
    for i in range(CONTACT_COUNT):
        a = CONTACT_BASE + i * CONTACT_SIZE
        r = image[a:a + CONTACT_SIZE]
        if r[0] == 0xFF or r[0x14] > 2:
            continue
        kind = r[0x14]
        cid = 0xFFFFFF if kind == 2 else bcd_id(r, 0x10)
        if cid == 0:
            continue
        ts = ''
        if r[0x17] != 0xFF and not r[0x17] & 1:
            ts = '2' if r[0x17] & 2 else '1'
        cp.contacts.append({'index': i + 1, 'name': text(r, 0, 16), 'id': cid, 'type': CONTACT_TYPES[kind], 'ts': ts})
    contact_names = {c['index']: c['name'] for c in cp.contacts}
    for i in range(LIST_COUNT):
        n = image[LIST_COUNTS + i]
        if n in (0, 0xFF):
            continue
        a = LIST_BASE + i * LIST_SIZE
        members = []
        for k in range(min(n - 1, 32)):
            c = le16(image, a + 16 + 2 * k)
            if c == 0 or c > CONTACT_COUNT:
                break
            members.append(c)
        cp.lists.append({'index': i + 1, 'name': text(image, a, 16), 'count': len(members), 'contacts': members,
                         'contact_names': ', '.join(contact_names.get(c, '#%d' % c) for c in members)})
    cp.radio_name = text(image, GEN_CALLSIGN, 8)
    return cp


def load(path):
    with open(path, 'rb') as f:
        return decode(f.read())


# --- Tables, exports ------------------------------------------------------------------------------------------------

ZONE_COLUMNS = (('index', 'No'), ('name', 'Ad'), ('count', 'Kanal sayısı'), ('channel_names', 'Kanallar'))
CONTACT_COLUMNS = (('index', 'No'), ('name', 'Ad'), ('id', 'ID / TG'), ('type', 'Tür'), ('ts', 'TS'))
LIST_COLUMNS = (('index', 'No'), ('name', 'Ad'), ('count', 'Üye sayısı'), ('contact_names', 'Kişiler'))


def to_csv(columns, rows):
    buf = io.StringIO()
    w = csv.writer(buf, lineterminator='\n')
    w.writerow([h for _, h in columns])
    for r in rows:
        w.writerow([r.get(k, '') for k, _ in columns])
    return buf.getvalue()


def change_list(source, changes):
    """The JSON 'change list' the user keeps for planning (never written to a radio)"""
    return json.dumps({
        'kind': 'ddm25-codeplug-change-list',
        'note': 'Telsize yazmaz - yazmak için upstream CPS kullanın',
        'source_backup': str(source),
        'created': time.strftime('%Y-%m-%dT%H:%M:%S'),
        'changes': changes,
    }, indent=2, ensure_ascii=False) + '\n'
