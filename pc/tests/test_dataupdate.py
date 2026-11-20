import base64
import datetime
import json
from pathlib import Path
import tempfile
import unittest

import fixtures as fx
from ddm32_app import dataupdate as du

SAT_FREQ = """# comment
#
25544      ISS     437.800   145.990   67.0   0
27607      SO-50   436.795   145.850   67.0   74.4
"""
OLD_TLE = """# CelesTrak elements, epoch 01.01.2026 (fetched 02.01.2026): refresh before a release
# DDM32 Plus Tools > Satellites (kept comment)
ISS (ZARYA)
"""


class FakeNet:
    def __init__(self, answers):
        self.answers = answers
        self.urls = []

    def __call__(self, url):
        self.urls.append(url)
        a = self.answers[url]
        if isinstance(a, Exception):
            raise a
        return a


class DataUpdateTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.p = du.firmware_paths(self.root)
        self.p['sat_freq'].parent.mkdir(parents=True)
        self.p['sat_freq'].write_text(SAT_FREQ)
        self.p['tle'].write_text(OLD_TLE)
        self.p['user_csv'].parent.mkdir(parents=True)
        self.p['user_csv'].write_bytes(b'RADIO_ID,CALLSIGN,FIRST_NAME\n1,OLD,x\n')

    def tearDown(self):
        self.tmp.cleanup()

    def test_user_csv(self):
        data = b'RADIO_ID,CALLSIGN,FIRST_NAME,LAST_NAME,CITY,STATE,COUNTRY\n1,A,,,,,X\n2,B,,,,,Y\n'
        net = FakeNet({du.USER_CSV_URL: data})
        self.assertEqual(du.update_user_csv(self.p, net), 2)
        self.assertEqual(self.p['user_csv'].read_bytes(), data)

    def test_user_csv_bad_keeps_old(self):
        old = self.p['user_csv'].read_bytes()
        net = FakeNet({du.USER_CSV_URL: b'<html>error</html>'})
        with self.assertRaises(du.DataError):
            du.update_user_csv(self.p, net)
        self.assertEqual(self.p['user_csv'].read_bytes(), old)

    def test_rptrs(self):
        doc = {'rptrs': [{'country': 'Turkiye'}, {'country': 'Germany'}, {'country': 'Turkey'}]}
        net = FakeNet({du.RPTRS_URL: json.dumps(doc).encode()})
        self.assertEqual(du.update_rptrs(self.p, net), (3, 2))
        with self.assertRaises(du.DataError):
            du.check_rptrs(b'{"x": 1}')

    def amateur(self, *sets):
        """A CelesTrak amateur group answer: other satellites around ours"""
        return (fx.tle_set('OSCAR 7 (AO-7)', 7530, '26322.50000000') + ''.join(sets) +
                fx.tle_set('FUNCUBE-1 (AO-73)', 39444, '26322.50000000')).encode()

    def write_current(self, iss='26318.10000000', so50='26318.10000000'):
        self.p['tle'].write_text(OLD_TLE.replace('ISS (ZARYA)\n', '') + fx.tle_set('ISS (ZARYA)', 25544, iss) +
                                 fx.tle_set('SO-50', 27607, so50))

    def test_tle_check_amateur_group(self):
        self.write_current()
        net = FakeNet({du.AMATEUR_URL: self.amateur(fx.tle_set('ISS (ZARYA)             ', 25544, '26322.33455779'),
                                                    fx.tle_set('SAUDISAT 1C (SO-50)', 27607, '26318.10000000'))})
        before = self.p['tle'].read_text()
        c = du.tle_check(self.p, net, today=datetime.date(2026, 11, 19))
        self.assertEqual(net.urls, [du.AMATEUR_URL])                 # one request for the whole group
        self.assertEqual(self.p['tle'].read_text(), before)          # the repository copy is never written
        rows = {r['catalog']: r for r in c.rows}
        self.assertEqual(rows[25544]['status'], 'new')
        self.assertEqual(rows[25544]['current_age'], 5)               # 14.11.2026
        self.assertEqual(rows[25544]['celestrak'], datetime.date(2026, 11, 18))
        self.assertEqual(rows[27607]['status'], 'same')
        self.assertTrue(c.changed)
        self.assertEqual([r['name'] for r in c.newer], ['ISS'])
        sets = du.parse_tle_text(c.text)
        self.assertEqual([s['catalog'] for s in sets], [25544, 27607])
        self.assertEqual(sets[0]['epoch'], datetime.date(2026, 11, 18))
        lines = c.text.splitlines()
        self.assertTrue(lines[0].startswith('# CelesTrak elements, epoch 14.11.2026 (fetched 19.11.2026)'))
        self.assertEqual(lines[1], '# DDM32 Plus Tools > Satellites (kept comment)')
        self.assertEqual(lines[2], 'ISS (ZARYA)')

    def test_tle_older_or_missing_keeps_current(self):
        self.write_current(iss='26322.10000000')
        net = FakeNet({du.AMATEUR_URL: self.amateur(fx.tle_set('ISS', 25544, '26320.10000000'))})
        c = du.tle_check(self.p, net, today=datetime.date(2026, 11, 19))
        rows = {r['catalog']: r for r in c.rows}
        self.assertEqual(rows[25544]['status'], 'older')
        self.assertEqual(rows[27607]['status'], 'missing')
        self.assertFalse(c.changed)
        self.assertEqual(du.parse_tle_text(c.text)[0]['epoch'], datetime.date(2026, 11, 18))

    def test_tle_wsl_copy_counts_as_current(self):
        self.write_current()
        net = FakeNet({du.AMATEUR_URL: self.amateur(fx.tle_set('ISS', 25544, '26322.33455779'))})
        c = du.tle_check(self.p, net, wsl={'25544': '2026-11-18'}, today=datetime.date(2026, 11, 19))
        self.assertFalse(c.changed)                                   # already written into WSL
        self.assertEqual({r['catalog']: r['status'] for r in c.rows}[25544], 'same')

    def test_tle_bad_checksum_is_not_used(self):
        self.write_current()
        bad = fx.tle_set('ISS', 25544, '26322.33455779').splitlines()
        bad[1] = bad[1][:-1] + str((int(bad[1][-1]) + 1) % 10)
        net = FakeNet({du.AMATEUR_URL: self.amateur('\n'.join(bad) + '\n')})
        c = du.tle_check(self.p, net, today=datetime.date(2026, 11, 19))
        row = [r for r in c.rows if r['catalog'] == 25544][0]
        self.assertEqual(row['status'], 'missing')
        self.assertIn('sağlama', row['error'])
        self.assertFalse(c.changed)

    def test_tle_not_a_tle_answer(self):
        net = FakeNet({du.AMATEUR_URL: b'No GP data found'})
        with self.assertRaises(du.DataError):
            du.tle_check(self.p, net)

    def test_network_error_keeps_file(self):
        net = FakeNet({du.AMATEUR_URL: OSError('offline')})
        with self.assertRaises(OSError):
            du.tle_check(self.p, net)
        self.assertEqual(self.p['tle'].read_text(), OLD_TLE)

    def test_tle_cache_two_hours(self):
        self.write_current()
        cache = self.root / 'cache' / 'amateur.txt'
        net = FakeNet({du.AMATEUR_URL: self.amateur(fx.tle_set('ISS', 25544, '26322.33455779'))})
        c1 = du.tle_check(self.p, net, cache=cache, now=1_000_000)
        self.assertFalse(c1.from_cache)
        c2 = du.tle_check(self.p, net, cache=cache, now=1_000_000 + 3600)
        self.assertTrue(c2.from_cache)
        self.assertEqual(len(net.urls), 1)
        du.tle_check(self.p, net, cache=cache, now=1_000_000 + 3 * 3600)
        self.assertEqual(len(net.urls), 2)

    def test_tle_write_command_wsl_only(self):
        self.write_current()
        net = FakeNet({du.AMATEUR_URL: self.amateur(fx.tle_set('ISS', 25544, '26322.33455779'))})
        c = du.tle_check(self.p, net)
        program, args = du.tle_write_command(c.text)
        self.assertEqual((program, args[:5]), ('wsl.exe', ['-d', 'Ubuntu', '-e', 'bash', '-lc']))
        script = args[5]
        self.assertIn('cd ~/ddm32-plus', script)
        self.assertIn('python3 tools/sat_tle.py --tle integration/radiofoundry/sat/tle.txt.download', script)
        self.assertIn('mv integration/radiofoundry/sat/tle.txt.download integration/radiofoundry/sat/tle.txt',
                      script)
        self.assertNotIn('/mnt/', script)                             # never the Windows tree
        b64 = script.split("printf %s '")[1].split("'")[0]
        self.assertEqual(base64.b64decode(b64).decode('ascii'), c.text)
        self.assertTrue(du.tle_written([f'{c.sha256}  integration/radiofoundry/sat/tle.txt'], c.sha256))
        self.assertFalse(du.tle_written(['x'], c.sha256))

    def test_turkish_users_and_summary(self):
        data = b'RADIO_ID,CALLSIGN,FIRST_NAME,LAST_NAME,CITY,STATE,COUNTRY\r\n1,A,,,,,Turkey\r\n2,B,,,,,Turkiye\r\n3,C,,,,,Spain\r\n'
        self.assertEqual(du.turkish_users(data), 2)
        info = {'user_csv': {'count': 8700, 'country': 8694}, 'rptrs_json': {'count': 11453, 'country': 100},
                'tle': {'count': 4, 'epoch': datetime.date(2026, 11, 16)}}
        self.assertEqual(du.summary_text(info, today=datetime.date(2026, 11, 19)),
                         'ID: 8.694 Türk kullanıcı, Röle: 100, Uydu verisi: 3 gün önce')
        self.assertEqual(du.age_text(datetime.date(2026, 11, 19), datetime.date(2026, 11, 19)), 'bugün')

    def test_update_all_reports_each(self):
        net = FakeNet({du.USER_CSV_URL: b'RADIO_ID,CALLSIGN\n1,A\n', du.RPTRS_URL: OSError('offline')})
        errors = du.update_all(self.p, net)
        self.assertEqual(errors['user_csv'], '')
        self.assertIn('offline', errors['rptrs_json'])
        self.assertNotIn('tle', errors)                         # the TLE is its own check and WSL write
        self.assertEqual(self.p['tle'].read_text(), OLD_TLE)

    def test_epoch(self):
        self.assertEqual(du.tle_epoch('1 25544U 98067A   26321.33455779'), datetime.date(2026, 11, 17))

    def test_committed_tle_parses(self):
        real = Path(__file__).resolve().parents[2] / 'integration/radiofoundry/sat/tle.txt'
        if real.is_file():
            self.assertGreaterEqual(len(du.parse_tle_text(real.read_text())), 1)

    def test_counts(self):
        info = du.file_counts(self.p)
        self.assertEqual(info['user_csv']['count'], 1)
        self.assertIsNone(info['rptrs_json']['count'])

    def test_build_command(self):
        program, args = du.build_command(Path('E:/GIT-REPO/DDM32-Plus'))
        self.assertEqual(program, 'wsl.exe')
        self.assertEqual(args[:5], ['-d', 'Ubuntu', '-e', 'bash', '-lc'])
        script = args[5]
        self.assertIn("'/mnt/e/GIT-REPO/DDM32-Plus/' ~/ddm32-plus/", script)
        for ex in ('/research', '/toolchain', '/build/', '/logs/', '/artifacts/', '__pycache__/'):
            self.assertIn('--exclude ' + ex, script)
        self.assertIn('build_radiofoundry.py --plus --public --stack-check --verify-rebuild', script)
        self.assertNotIn('--tx-bench', script)
        self.assertIn('--exclude /integration/radiofoundry/sat/tle.txt', script)
        self.assertIn("rsync -a --update '/mnt/e/GIT-REPO/DDM32-Plus/integration/radiofoundry/sat/tle.txt' "
                      '~/ddm32-plus/integration/radiofoundry/sat/tle.txt', script)


if __name__ == '__main__':
    unittest.main()
