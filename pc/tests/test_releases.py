"""Signed releases: catalog, download with hash checks, the gate in the downloaded folder, the installed version and
the compatibility warnings. Fake network only."""
import io
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

import fixtures as fx
import rtfixtures as rt
from ddm25_app import releases


def release_zip(app):
    src = Path(tempfile.mkdtemp(prefix='ddm25-zip-'))
    rt.write_plus(src, app, signed_for=False)
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, 'w') as z:
        for n in releases.PACKAGE_FILES:
            z.writestr(n, (src / n).read_bytes())
    return buf.getvalue()


class ReleasesTest(unittest.TestCase):
    def setUp(self):
        rt.use_test_key(self)
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name) / 'surumler'
        self.root.mkdir()
        self.app = fx.release_app('1.2.0')
        self.zip = release_zip(self.app)
        self.entry = rt.firmware_entry(self.app, version='1.2.0', zip_bytes=self.zip, release='v1.2.0')
        self.m = rt.manifest([self.entry], release='v1.2.0')
        self.data, self.sig = rt.signed(self.m)
        self.files = {}
        self._l = []
        self.add_release('v1.2.0', self.data, self.sig)

    def tearDown(self):
        self.tmp.cleanup()

    def add_release(self, tag, data, sig):
        self.files[rt.url(tag, 'ddm25-manifest.json')] = data
        self.files[rt.url(tag, 'ddm25-manifest.json.sig')] = sig.encode() if isinstance(sig, str) else sig
        self._l.append({'tag_name': tag, 'assets': [
            {'name': 'ddm25-manifest.json', 'browser_download_url': rt.url(tag, 'ddm25-manifest.json')},
            {'name': 'ddm25-manifest.json.sig', 'browser_download_url': rt.url(tag, 'ddm25-manifest.json.sig')}]})

    def fetch(self, url, limit=None):
        return self.files[url]

    def catalog(self):
        self.files[releases.API_URL] = json.dumps(self._l).encode()
        return releases.load_catalog(self.root, self.fetch)

    def download_bytes(self, payload):
        def download(url, dest, size, progress=None):
            self.assertEqual(url, self.entry['url'])
            Path(dest).write_bytes(payload)
            return dest
        return download

    # --- catalog --------------------------------------------------------------------------------------------------
    def test_good_and_bad_signatures(self):
        other = rt.manifest([rt.firmware_entry(fx.release_app('1.3.0'), version='1.3.0', release='v1.3.0')],
                            release='v1.3.0')
        d, s = rt.signed(other, rt.OTHER_SECRET)
        self.add_release('v1.3.0', d, s)                                   # wrong key
        wrong_tag = rt.manifest([rt.firmware_entry(fx.release_app('1.4.0'), version='1.4.0')], release='v9.9.9')
        d, s = rt.signed(wrong_tag)
        self.add_release('v1.4.0', d, s)                                   # signed, but for another release
        d, s = rt.signed(self.m)
        self.add_release('v1.5.0', d.replace(b'1.2.0', b'1.5.0'), s)       # changed after signing
        cat = self.catalog()
        self.assertEqual([f.version for f in cat.firmware], ['1.2.0'])
        rej = dict(cat.rejected)
        self.assertIn('signature is not valid', rej['v1.3.0'])
        self.assertIn('not v1.4.0', rej['v1.4.0'])
        self.assertIn('signature is not valid', rej['v1.5.0'])
        self.assertEqual(cat.source, 'github')
        cached = releases.cached_catalog(self.root)                        # verified again from disk
        self.assertEqual([f.version for f in cached.firmware], ['1.2.0'])

    def test_asset_outside_the_release_is_refused(self):
        self._l[0]['assets'][0]['browser_download_url'] = 'https://github.com/someone/else/releases/download/x/m.json'
        cat = self.catalog()
        self.assertEqual(cat.firmware, [])
        self.assertIn('outside this release', dict(cat.rejected)['v1.2.0'])

    def test_no_key_lists_nothing(self):
        from unittest.mock import patch
        with patch.object(rt.sm, 'PUBLIC_KEY_HEX', ''):
            cat = self.catalog()
        self.assertEqual(cat.firmware, [])
        self.assertIn('No release signing key', dict(cat.rejected)['v1.2.0'])

    def test_cache_tampering_is_caught(self):
        self.catalog()
        p = self.root / 'manifests' / 'v1.2.0' / 'ddm25-manifest.json'
        p.write_bytes(p.read_bytes().replace(b'"stable"', b'"beta"  '))
        cat = releases.cached_catalog(self.root)
        self.assertEqual(cat.firmware, [])
        self.assertIn('signature', dict(cat.rejected)['v1.2.0'])

    # --- download ---------------------------------------------------------------------------------------------------
    def test_download_verifies_hash_and_contents(self):
        fv = self.catalog().find('1.2.0')
        folder = releases.download_firmware(fv, self.root, self.download_bytes(self.zip))
        p = releases.Package(folder)
        self.assertTrue(p.installable)
        self.assertEqual(p.sha256, rt.sm.sha256(self.app))
        self.assertEqual(sorted(x.name for x in folder.iterdir()),
                         sorted(releases.PACKAGE_FILES + ('ddm25-manifest.json', 'ddm25-manifest.json.sig')))
        # the installer's own gate accepts the folder
        from radiotools import install_plus
        self.assertEqual(install_plus.plus_application(folder), self.app)
        self.assertEqual(releases.download_firmware(fv, self.root, None), folder)   # already there: no download

    def test_hash_mismatch_leaves_nothing(self):
        fv = self.catalog().find('1.2.0')
        bad = bytearray(self.zip)
        bad[-30] ^= 1
        for payload in (bytes(bad), self.zip + b'x', self.zip[:-1]):
            with self.subTest(n=len(payload)), self.assertRaises(releases.VerifyError):
                releases.download_firmware(fv, self.root, self.download_bytes(payload))
            self.assertEqual([p.name for p in self.root.iterdir()], ['manifests'])

    def test_unexpected_zip_contents(self):
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, 'w') as z:
            z.writestr('rfoundry_dm32.bin', self.app)
            z.writestr('../evil.py', b'x')
        evil = buf.getvalue()
        entry = dict(self.entry, sha256=rt.sm.sha256(evil), size=len(evil))
        m = rt.manifest([entry], release='v1.2.0')
        d, s = rt.signed(m)
        self._l.clear()
        self.add_release('v1.2.0', d, s)
        fv = self.catalog().find('1.2.0')
        with self.assertRaisesRegex(releases.VerifyError, 'üç dosya'):
            releases.download_firmware(fv, self.root, self.download_bytes(evil))
        self.assertFalse((Path(self.tmp.name) / 'evil.py').exists())

    def test_package_with_tampered_manifest_is_not_installable(self):
        fv = self.catalog().find('1.2.0')
        folder = releases.download_firmware(fv, self.root, self.download_bytes(self.zip))
        (folder / 'ddm25-manifest.json.sig').write_text(rt.signed(self.m, rt.OTHER_SECRET)[1])
        p = releases.Package(folder)
        self.assertFalse(p.installable)
        self.assertIn('imza', p.status_text())

    def test_https_github_only(self):
        for url in ('http://github.com/x', 'https://example.com/x', 'ftp://github.com/x',
                    'https://github.com.evil.example/x'):
            with self.subTest(url=url), self.assertRaises(releases.NetError):
                releases._check_url(url)
        releases._check_url('https://objects.githubusercontent.com/github-production-release-asset/x')


class InstalledAndWarningsTest(unittest.TestCase):
    def fv(self, version, sf=1, cf=1, minc=None, channel=None):
        app = fx.release_app(version)
        e = rt.firmware_entry(app, version=version, settings_format=sf, codeplug_format=cf, min_compatible=minc,
                              channel=channel or ('beta' if '-' in version else 'stable'))
        return releases.FirmwareVersion(e, f'v{version}')

    def setUp(self):
        rt.use_test_key(self)

    def test_match_installed(self):
        a, b = self.fv('1.0.0'), self.fv('1.1.0')
        program = rt.make_program(app=fx.release_app('1.1.0'))
        self.assertIs(releases.match_installed(program, [a, b]), b)
        other = rt.make_program(app=fx.release_app('1.1.0') + b'extra')
        self.assertIsNone(releases.match_installed(other, [a, b]))

    def test_downgrade_warnings_from_format_fields(self):
        cur = self.fv('2.0.0', sf=3, cf=2, minc='1.5.0')
        texts = lambda w: ' '.join(t for _, t in w)  # noqa: E731
        w = releases.change_warnings(cur, self.fv('1.9.0', sf=3, cf=2))
        self.assertIn('Eski bir sürüme', texts(w))
        self.assertFalse(releases.needs_second_confirm(w))
        w = releases.change_warnings(cur, self.fv('1.6.0', sf=2, cf=2))
        self.assertIn('Ayar biçimi geriye gidiyor (3 → 2)', texts(w))
        self.assertTrue(releases.needs_second_confirm(w))
        w = releases.change_warnings(cur, self.fv('1.6.0', sf=3, cf=1))
        self.assertIn('P25 kanalı', texts(w))
        w = releases.change_warnings(cur, self.fv('1.4.0', sf=3, cf=2))
        self.assertIn('en az 1.5.0', texts(w))
        self.assertTrue(releases.needs_second_confirm(w))
        w = releases.change_warnings(cur, self.fv('2.0.0', sf=3, cf=2))
        self.assertEqual(w, [(releases.INFO, 'Bu sürüm zaten telsizde yüklü.')])
        w = releases.change_warnings(cur, self.fv('2.1.0', sf=4, cf=2))
        self.assertIn('biçimini yeniler', texts(w))
        self.assertFalse(releases.needs_second_confirm(w))

    def test_beta_stock_and_unknown_warnings(self):
        texts = lambda w: ' '.join(t for _, t in w)  # noqa: E731
        w = releases.change_warnings(None, self.fv('1.0.0-beta.1'), fx.DETECT_STOCK_049)
        self.assertIn('beta', texts(w))
        self.assertIn('.049', texts(w))
        self.assertIn('yalnızca stok .047', texts(w))
        w = releases.change_warnings(None, self.fv('1.0.0'), fx.DETECT_STOCK)
        self.assertNotIn('denenmedi', texts(w))
        w = releases.change_warnings(None, self.fv('1.0.0'), fx.DETECT_CLASSIC)
        self.assertIn('imzalı listede yok', texts(w))


if __name__ == '__main__':
    unittest.main()
