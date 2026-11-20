import csv
import io
import json
import unittest

import fixtures as fx
from ddm32_app import codeplug as cp


class CodeplugDecodeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cp = cp.decode(fx.sample_image())
        cls.ch = {c['number']: c for c in cls.cp.channels}

    def test_counts(self):
        self.assertEqual(self.cp.counts(), {'channels': 9, 'zones': 2, 'contacts': 3, 'rx_group_lists': 1})
        self.assertEqual(self.cp.bad_records, 1)
        self.assertEqual(self.cp.mode_counts(), {'FM': 2, 'DMR': 2, 'P25': 2, 'YSF': 1, 'NXDN': 1, 'dPMR': 1})

    def test_fm(self):
        c = self.ch[1]
        self.assertEqual((c['name'], c['rx_mhz'], c['tx_mhz'], c['mode']), ('FM Simplex', '145.50000', '145.50000', 'FM'))
        self.assertEqual((c['rx_tone'], c['tx_tone'], c['bw'], c['power']), ('67.0', 'D023I', '25 kHz', 'H'))
        self.assertEqual(self.ch[130]['scan_skip'], 'Evet')
        self.assertEqual(self.ch[130]['tot_s'], '60')

    def test_dmr(self):
        c = self.ch[2]
        self.assertEqual((c['mode'], c['cc'], c['ts'], c['contact'], c['rx_list']), ('DMR', '1', '2', '1', '1'))
        self.assertEqual((c['rx_mhz'], c['tx_mhz'], c['power']), ('438.50000', '430.90000', 'L-'))
        self.assertEqual(c['rx_only'], '')

    def test_dmr_any_bits(self):
        c = self.ch[3]
        self.assertEqual(c['cc'], 'Herhangi')
        self.assertEqual(c['ts'], 'Herhangi')
        self.assertTrue(c['rx_list'].startswith('Herhangi'))
        self.assertEqual(c['rx_only'], 'Evet')

    def test_dmr_any_bits_alone(self):
        for bit, field in ((0x80, 'cc'), (0x40, 'ts'), (0x20, 'rx_list')):
            b = fx.ImageBuilder()
            b.channel(1, fx.channel_record('x', 438_000_000, mode=1, cc=bit | 5, rx_group=4))
            c = cp.decode(b.bytes()).channels[0]
            self.assertTrue(c[field].startswith('Herhangi'), (bit, c))
            others = {'cc': '5', 'ts': '1', 'rx_list': '4'}
            for f, v in others.items():
                if f != field:
                    self.assertEqual(c[f], v, (bit, f))

    def test_any_bits_ignored_outside_dmr(self):
        b = fx.ImageBuilder()
        b.channel(1, fx.channel_record('fm', 145_000_000, mode=0, cc=0xE0))
        c = cp.decode(b.bytes()).channels[0]
        self.assertEqual((c['cc'], c['ts'], c['rx_list']), ('', '', ''))

    def test_p25_nac_hex_and_decimal(self):
        c = self.ch[4]
        self.assertEqual(c['mode'], 'P25')
        self.assertEqual(c['nac'], '$293 = 659')
        self.assertEqual(c['tx_nac'], cp.TXNAC_SAME)     # an existing record: TX with the RX NAC
        self.assertEqual(c['summary'], 'NAC $293 = 659 · TG 101')
        self.assertEqual((c['tg'], c['unit_id'], c['mode_byte']), ('101', '1234567', '0xA5'))
        self.assertEqual(cp.nac_text(0x125), '$125 = 293')
        self.assertEqual(cp.nac_text(0x001), '$001 = 1')

    def test_p25_any(self):
        c = self.ch[5]
        self.assertTrue(c['nac'].startswith('Herhangi'))
        self.assertEqual((c['tg'], c['rx_only']), ('Herhangi', 'Evet'))

    def test_p25_bad_marker(self):
        b = fx.ImageBuilder()
        b.channel(1, fx.channel_record('p', 433_000_000, mode=0xA5, cc=0x02, rx_group=0x93, contact=1))
        self.assertIn('geçersiz', cp.decode(b.bytes()).channels[0]['nac'])

    def test_ysf_nxdn_dpmr(self):
        self.assertEqual((self.ch[6]['mode'], self.ch[6]['code'], self.ch[6]['mode_byte']), ('YSF', 'DG-ID 12', '0xA6'))
        self.assertEqual((self.ch[7]['mode'], self.ch[7]['code'], self.ch[7]['mode_byte']), ('NXDN', 'RAN herhangi', '0xA7'))
        self.assertEqual((self.ch[8]['mode'], self.ch[8]['code'], self.ch[8]['mode_byte']), ('dPMR', 'CC 4', '0xA8'))
        for n in (6, 7, 8):
            self.assertEqual(self.ch[n]['rx_only'], 'Evet')

    def test_unknown_mode_shown(self):
        b = fx.ImageBuilder()
        b.channel(1, fx.channel_record('u', 433_000_000, mode=0x03))
        self.assertEqual(cp.decode(b.bytes()).channels[0]['mode'], 'Bilinmiyor (0x03)')

    # Two made-up channels in the DM32-nxdn-p25 multi-mode format (CH_MODE 2, record from offset 0x10)
    MODE2_18 = bytes.fromhex('00 25 31 43 00 25 31 43 02 00 00 00 00 00 00 00 93 02 93 02 00 00 80 00 00 01 00 93'
                             ' 00 00 01 00 00')
    MODE2_19 = bytes.fromhex('50 12 82 43 50 12 82 43 02 00 00 00 00 00 00 00 93 02 93 02 00 00 80 00 00 01 00 93'
                             ' 00 00 f4 01 00')

    @staticmethod
    def mode2_record(name, tail, **at):
        r = bytearray(cp.CH_SIZE)
        fx.put_name(r, cp.CH_NAME, name)
        r[0x10:0x10 + len(tail)] = tail
        for off, v in at.items():
            r[int(off[1:], 16)] = v
        return bytes(r)

    def test_p25_mode2_channels(self):
        b = fx.ImageBuilder()
        b.channel(18, self.mode2_record('433.125', self.MODE2_18))
        b.channel(19, self.mode2_record('438.2125', self.MODE2_19))
        ch = {c['number']: c for c in cp.decode(b.bytes()).channels}
        c = ch[18]
        self.assertEqual((c['mode'], c['mode_byte'], c['rx_mhz'], c['tx_mhz']), ('P25', '0x02', '433.12500', '433.12500'))
        self.assertTrue(c['nac'].startswith('$293 = 659'))
        self.assertIn('CH_MODE 2', c['nac'])
        self.assertEqual((c['tg'], c['unit_id'], c['rx_only'], c['bw']), ('1', '1', '', '12.5 kHz'))
        self.assertEqual((c['rx_tone'], c['tx_tone'], c['cc'], c['ts']), ('', '', '', ''))
        self.assertEqual((ch[19]['tg'], ch[19]['unit_id'], ch[19]['rx_mhz']), ('500', '1', '438.21250'))
        self.assertIn('NAC $293 = 659', c['summary'])
        self.assertEqual(cp.MODE_BYTES['P25'], 1)       # an edit is planned in Classic r13's form (DMR + RFKV tag)

    def test_p25_mode2_rx_only_cases(self):
        cases = (
            ({}, '1', ''),                                  # as written
            ({'x22': 0x25, 'x23': 0x01}, '1', ''),          # TX NAC $125 unlike RX $293: kept, TX with it
            ({'x31': 0x40}, '1 (tüm gruplar)', ''),         # every talkgroup: the Normal squelch, TG 1 for TX
            ({'x26': 0x00}, '1', 'Evet'),                   # no unit ID
            ({'x2E': 0x00}, 'Herhangi', 'Evet'),            # no talkgroup
            ({'x33': cp.FLAG4_RX_ONLY}, '1', 'Evet'),       # the record's own RX only
        )
        for at, tg, rx_only in cases:
            b = fx.ImageBuilder()
            b.channel(1, self.mode2_record('m2', self.MODE2_18, **at))
            c = cp.decode(b.bytes()).channels[0]
            self.assertEqual((c['mode'], c['tg'], c['rx_only']), ('P25', tg, rx_only), at)
        b = fx.ImageBuilder()
        b.channel(1, self.mode2_record('tx', self.MODE2_18, x22=0x25, x23=0x01))
        c = cp.decode(b.bytes()).channels[0]
        self.assertEqual((c['tx_nac'], c['rx_only']), ('$125 = 293', ''))
        self.assertIn('NAC $293 = 659  (CH_MODE 2 biçimi) > TX $125 = 293', c['summary'])
        # RX Any, TX the derivative's default $293: hears every NAC, transmits with $293
        b = fx.ImageBuilder()
        b.channel(1, self.mode2_record('any', self.MODE2_18, x20=0x7E, x21=0x0F))
        c = cp.decode(b.bytes()).channels[0]
        self.assertTrue(c['nac'].startswith('Herhangi'))
        self.assertEqual((c['tx_nac'], c['rx_only']), ('$293 = 659', ''))
        self.assertEqual(self.ch18_tx_nac(), cp.TXNAC_SAME)

    def ch18_tx_nac(self):
        b = fx.ImageBuilder()
        b.channel(18, self.mode2_record('433.125', self.MODE2_18))
        return cp.decode(b.bytes()).channels[0]['tx_nac']

    def test_p25_r7_tx_nac_tag_not_read(self):
        # The r7-based Plus's TX NAC tag (0x5000 | NAC in a 0xA5 record's TX tone word) is not read any more: Same as RX
        for word in (0x5659, 0xFFFF, 0x0000, 0x5000):
            b = fx.ImageBuilder()
            r = bytearray(fx.channel_record('p', 434_250_000, mode=0xA5, cc=0xD2, rx_group=0x93, contact=101,
                                            unit_id=1234567))
            r[cp.CH_TX_TONE:cp.CH_TX_TONE + 2] = word.to_bytes(2, 'little')
            b.channel(1, bytes(r))
            c = cp.decode(b.bytes()).channels[0]
            self.assertEqual((c['nac'], c['tx_nac']), ('$293 = 659', cp.TXNAC_SAME), hex(word))

    # --- Classic r13's P25 channels: a DMR base record (receive only) and an RFKV tag ---

    @staticmethod
    def kv_area(b, entries, crc_ok=True):
        """The RFKV area at EE+0xE000 holding entries {key: value}"""
        import zlib
        data = b''.join(k.to_bytes(2, 'little') + len(v).to_bytes(2, 'little') + v for k, v in entries.items())
        crc = zlib.crc32(data) ^ (0 if crc_ok else 1)
        h = cp.KV_MAGIC.to_bytes(4, 'little') + bytes([1, 16]) + len(data).to_bytes(2, 'little') + \
            crc.to_bytes(4, 'little') + bytes(4)
        b.img[cp.KV_ROOT:cp.KV_ROOT + 16 + len(data)] = h + data

    @staticmethod
    def p25_tag(name, flags=1, squelch=1, rx=0x8293, tx=0, tg=None):
        raw = name.encode('latin-1')[:16].ljust(16, b'\xff')
        block = bytes([squelch]) + rx.to_bytes(2, 'little') + tx.to_bytes(2, 'little')
        block += bytes([1]) + tg.to_bytes(2, 'little') + b'\x00' if tg is not None else bytes(4)
        return bytes([2, 0xFF]) + cp.name_check(raw).to_bytes(2, 'little') + bytes([flags]) + block

    def rfkv(self, tag=None, flag4=cp.FLAG4_RX_ONLY, unit_id=1234567, contact=0, crc_ok=True, number=1):
        b = fx.ImageBuilder()
        b.channel(number, fx.channel_record('Rpt', 434_250_000, mode=1, cc=1, contact=contact, unit_id=unit_id,
                                            flag4=flag4))
        self.kv_area(b, {cp.KEY_CH_TAG + number - 1: tag if tag is not None else self.p25_tag('Rpt', tg=101)},
                     crc_ok)
        return cp.decode(b.bytes()).channels[0]

    def test_p25_rfkv(self):
        c = self.rfkv(self.p25_tag('Rpt', tx=0x8659, tg=101))      # a repeater: RX $293, TX $659
        self.assertEqual((c['mode'], c['mode_byte'], c['nac'], c['tx_nac']),
                         ('P25', '0x01 + RFKV', '$293 = 659', '$659 = 1625'))
        self.assertEqual((c['tg'], c['unit_id'], c['rx_only'], c['bw']), ('101', '1234567', '', '12.5 kHz'))
        self.assertEqual(c['summary'], 'NAC $293 = 659 > TX $659 = 1625 · TG 101')
        c = self.rfkv(self.p25_tag('Rpt', rx=0, tx=0, tg=101))      # both NACs default: $293, Same as RX
        self.assertEqual((c['nac'], c['tx_nac']), ('$293 = 659', cp.TXNAC_SAME))
        c = self.rfkv(self.p25_tag('Rpt', rx=0x8F7E, tx=0x8293, tg=101))   # RX Any, an explicit TX $293
        self.assertTrue(c['nac'].startswith('Herhangi'))
        self.assertEqual(c['tx_nac'], '$293 = 659')
        c = self.rfkv(self.p25_tag('Rpt', squelch=0, tg=101))       # the Normal squelch: every group heard
        self.assertEqual(c['tg'], '101 (tüm gruplar)')
        c = self.rfkv(self.p25_tag('Rpt', flags=0, tg=101))         # TX not allowed by the tag: RX only
        self.assertEqual(c['rx_only'], 'Evet')
        c = self.rfkv(self.p25_tag('Rpt'), contact=3)               # no talkgroup of its own: the record's contact
        self.assertEqual(c['tg'], 'kişi 3')
        c = self.rfkv(unit_id=0)                                    # no unit ID of its own: the radio's
        self.assertEqual(c['unit_id'], '')                          # (a blank image has no radio DMR ID)

    def test_p25_rfkv_not_live(self):
        # The tag counts only for its own name, on a receive-only DMR record, in a sound area
        for kw in ({'tag': self.p25_tag('Other', tg=101)}, {'flag4': 0}, {'crc_ok': False}):
            c = self.rfkv(**kw)
            self.assertEqual(c['mode'], 'DMR', kw)
        c = self.rfkv(bytes([3, 0xFF]) + self.p25_tag('Rpt')[2:])  # a kind this build doesn't know
        self.assertEqual(c['mode'], 'Bilinmiyor (RFKV tür 3)')

    def test_name_check(self):
        # codeplug_tag.c codeplug_name_check: FNV-1a over 16 bytes, padding as zeros, the halves XORed
        h = 2166136261
        for c in b'Rpt' + bytes(13):
            h = ((h ^ c) * 16777619) & 0xFFFFFFFF
        self.assertEqual(cp.name_check(b'Rpt' + b'\xff' * 13), (h ^ (h >> 16)) & 0xFFFF)

    def test_other_modes_unchanged_by_mode2(self):
        # The sample image's FM, DMR, P25 (0xA5), YSF, NXDN and dPMR rows read as before
        self.assertEqual(self.ch[4]['nac'], '$293 = 659')
        self.assertEqual(self.ch[4]['mode_byte'], '0xA5')
        self.assertEqual(self.ch[1]['rx_tone'], '67.0')
        self.assertEqual(self.cp.mode_counts(), {'FM': 2, 'DMR': 2, 'P25': 2, 'YSF': 1, 'NXDN': 1, 'dPMR': 1})

    def test_zones(self):
        z = self.cp.zones
        self.assertEqual([x['name'] for x in z], ['Zone A', 'Zone B'])
        self.assertEqual(z[0]['channels'], [1, 2, 4, 130])
        self.assertEqual(z[1]['channels'], [5, 6])
        self.assertIn('130 Bank1 FM', z[0]['channel_names'])

    def test_zone_16_layout(self):
        b = fx.ImageBuilder()
        b.channel(1, fx.channel_record('a', 145_000_000))
        b.img[cp.ZONE_LAYOUT] = 0x05
        b.zone(2, 'Old', [1], cap=16)
        d = cp.decode(b.bytes())
        self.assertEqual(d.zone_capacity, 16)
        self.assertEqual(d.zones[0]['channels'], [1])

    def test_contacts_and_lists(self):
        c = {x['index']: x for x in self.cp.contacts}
        self.assertEqual((c[1]['id'], c[1]['type'], c[1]['ts']), (91, 'Grup', ''))
        self.assertEqual((c[2]['id'], c[2]['type'], c[2]['ts']), (2860001, 'Özel', '2'))
        self.assertEqual((c[3]['id'], c[3]['type']), (0xFFFFFF, 'Herkes'))
        l = self.cp.lists[0]
        self.assertEqual((l['name'], l['contacts'], l['contact_names']), ('List 1', [1, 3], 'TG 91, All'))

    def test_simple_columns(self):
        self.assertEqual((self.ch[1]['freq'], self.ch[1]['summary']), ('145.500', 'Ton RX 67.0 / TX D023I'))
        self.assertEqual(self.ch[2]['freq'], '438.500 / TX 430.900')
        self.assertEqual(self.ch[2]['summary'], 'CC 1 · TS 2 · Kişi 1')
        self.assertEqual(self.ch[3]['summary'], 'CC Herhangi · TS Herhangi · tüm TG · sadece RX')
        self.assertEqual(self.ch[8]['freq'], '446.10625')
        self.assertEqual(self.ch[130]['summary'], 'Ton yok')

    def test_general(self):
        self.assertEqual((self.cp.radio_name, self.cp.dmr_id), ('N0CALL', 2860000))

    def test_wrong_size(self):
        with self.assertRaises(cp.CodeplugError):
            cp.decode(b'\xff' * 1024)

    def test_blank_image(self):
        d = cp.decode(fx.ImageBuilder().bytes())
        self.assertEqual(d.counts(), {'channels': 0, 'zones': 0, 'contacts': 0, 'rx_group_lists': 0})


class ExportTest(unittest.TestCase):
    def test_csv(self):
        d = cp.decode(fx.sample_image())
        text = cp.to_csv(cp.CHANNEL_COLUMNS, d.channels)
        rows = list(csv.reader(io.StringIO(text)))
        self.assertEqual(rows[0][:5], ['No', 'Ad', 'Frekans', 'Mod', 'Ayar'])
        self.assertEqual(len(rows), 1 + 9)
        nac_col = rows[0].index('NAC')
        self.assertIn('$293 = 659', [r[nac_col] for r in rows])

    def test_change_list(self):
        doc = json.loads(cp.change_list('dm32-x', [{'table': 'channels', 'key': '1', 'field': 'name',
                                                    'old': 'A', 'new': 'B'}]))
        self.assertEqual(doc['kind'], 'ddm32-codeplug-change-list')
        self.assertIn('Telsize yazmaz', doc['note'])
        self.assertEqual(doc['changes'][0]['new'], 'B')


if __name__ == '__main__':
    unittest.main()
