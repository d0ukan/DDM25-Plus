"""pc/release/make_release.py with a test key in a temporary folder: key generation (never run on the real key path
here), the signed release, and the whole chain from release files to the installer's gate. Nothing is uploaded."""
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

import fixtures as fx
import rtfixtures as rt

sys.path.insert(0, str(rt.PC / 'release'))
import make_release as mr  # noqa: E402

from ddm25_app import releases  # noqa: E402
from radiotools import ed25519, install_plus  # noqa: E402


class ReleaseToolTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.top = Path(self.tmp.name)
        self.key = self.top / 'keys' / 'release-signing-key'
        self.restricted = []
        self.public, ok = mr.keygen(self.key, restrict_fn=self.restricted.append)
        self.assertTrue(ok)
        self.build = rt.write_plus(self.top / 'build', fx.release_app('1.0.0') + b'DDM25 Plus', signed_for=False)

    def tearDown(self):
        self.tmp.cleanup()

    def run_build(self, *extra, out=None):
        out = out or self.top / 'out'
        args = ['build', '--version', '1.0.0-beta.1', '--channel', 'beta', '--build-dir', str(self.build),
                '--settings-format', '1', '--codeplug-format', '2', '--min-compatible', '1.0.0-beta.1',
                '--notes-tr', 'İlk beta', '--notes-en', 'First beta', '--out', str(out), '--key', str(self.key),
                *extra]
        with patch('sys.stdout', io.StringIO()) as o:
            code = mr.main(args)
        return code, out, o.getvalue()

    def test_keygen(self):
        text = self.key.read_text().strip()
        self.assertEqual(len(text), 64)
        self.assertEqual(ed25519.secret_to_public(bytes.fromhex(text)).hex(), self.public)
        self.assertEqual(self.restricted, [self.key])
        with self.assertRaises(FileExistsError):
            mr.keygen(self.key, restrict_fn=lambda p: None)          # never overwrites a key
        with self.assertRaisesRegex(SystemExit, 'outside the repository'):
            mr.keygen(rt.PC / 'release-signing-key', restrict_fn=lambda p: None)
        self.assertFalse((rt.PC / 'release-signing-key').exists())
        self.assertEqual(mr.DEFAULT_KEY, Path.home() / '.ddm25' / 'release-signing-key')

    def test_build_signs_and_verifies(self):
        code, out, text = self.run_build()
        self.assertEqual(code, 0)
        self.assertIn('Nothing was uploaded', text)
        names = sorted(p.name for p in out.iterdir())
        self.assertEqual(names, ['ddm25-manifest.json', 'ddm25-manifest.json.sig', 'ddm25-plus-1.0.0-beta.1.zip'])
        m = mr.verify(out, self.public)
        e = m['firmware'][0]
        self.assertEqual((e['channel'], e['settings_format'], e['codeplug_format']), ('beta', 1, 2))
        self.assertEqual(e['url'], 'https://github.com/d0ukan/DDM25-Plus/releases/download/v1.0.0-beta.1/'
                                   'ddm25-plus-1.0.0-beta.1.zip')
        self.assertEqual(e['notes'], {'tr': 'İlk beta', 'en': 'First beta'})
        with self.assertRaises(rt.sm.ManifestError):
            mr.verify(out, rt.TEST_PUBLIC_HEX)                       # another key does not verify it
        with zipfile.ZipFile(out / 'ddm25-plus-1.0.0-beta.1.zip') as z:
            self.assertEqual(sorted(z.namelist()), sorted(releases.PACKAGE_FILES))
            build = json.loads(z.read('rfoundry_dm32.json'))
            self.assertTrue(build['public_enabled'])
            self.assertEqual(set(build), {'kind', 'version', 'plus_enabled', 'public_enabled',
                                          'forced_rebuild_identical', 'upstream_commit', 'outputs',
                                          'source_build_manifest_sha256', 'source_map_sha256'})
            self.assertTrue(all(l.strip().startswith(('#', '0x')) for l in
                                z.read('rfoundry_dm32.map').decode().splitlines() if l.strip()))

    def test_release_files_pass_the_app_and_the_installer(self):
        """make_release output -> the app's download and hash checks -> install_plus's signed gate"""
        code, out, _ = self.run_build()
        with patch.object(rt.sm, 'PUBLIC_KEY_HEX', self.public):
            files = {}
            tag = 'v1.0.0-beta.1'
            for p in out.iterdir():
                files[rt.url(tag, p.name)] = p.read_bytes()
            files[releases.API_URL] = json.dumps([{'tag_name': tag, 'assets': [
                {'name': n, 'browser_download_url': rt.url(tag, n)}
                for n in ('ddm25-manifest.json', 'ddm25-manifest.json.sig')]}]).encode()
            root = self.top / 'surumler'
            cat = releases.load_catalog(root, lambda url, limit=None: files[url])
            fv = cat.find('1.0.0-beta.1')

            def download(url, dest, size, progress=None):
                Path(dest).write_bytes(files[url])
            folder = releases.download_firmware(fv, root, download)
            self.assertEqual(install_plus.plus_application(folder), (self.build / 'rfoundry_dm32.bin').read_bytes())

    def test_program_zip(self):
        appzip = self.top / 'DDM25Plus-1.0.0-beta.2.zip'
        with zipfile.ZipFile(appzip, 'w') as z:
            z.writestr('DDM25Plus/DDM25Plus.exe', b'MZ')
            z.writestr('DDM25Plus/radiotools/dm32_readonly_backup.py', b'# the backup tool is public source')
        code, out, _ = self.run_build('--app-zip', str(appzip), '--app-version', '1.0.0-beta.2')
        m = mr.verify(out, self.public)
        self.assertEqual(m['app']['version'], '1.0.0-beta.2')
        self.assertTrue((out / 'DDM25Plus-1.0.0-beta.2.zip').is_file())
        bad = self.top / 'bad.zip'
        with zipfile.ZipFile(bad, 'w') as z:
            z.writestr('DDM25Plus/DDM25Plus.exe', b'MZ')
            z.writestr('DDM25Plus/yedekler/progmem-pass1.bin', b'private')
        with self.assertRaisesRegex(SystemExit, 'must not be published'):
            self.run_build('--app-zip', str(bad), '--app-version', '1.0.0-beta.2', out=self.top / 'out2')
        for i, private in enumerate(('DDM25Plus/Backups/notes.txt', 'DDM25Plus/x/backup-manifest.json')):
            bad = self.top / f'bad{i}.zip'
            with zipfile.ZipFile(bad, 'w') as z:
                z.writestr('DDM25Plus/DDM25Plus.exe', b'MZ')
                z.writestr(private, b'private')
            with self.subTest(private=private), self.assertRaisesRegex(SystemExit, 'must not be published'):
                self.run_build('--app-zip', str(bad), '--app-version', '1.0.0-beta.2', out=self.top / f'o{i}')

    def test_refusals(self):
        nonpublic = rt.write_plus(self.top / 'np', fx.release_app('1.0.0'), signed_for=False)
        j = json.loads((nonpublic / 'rfoundry_dm32.json').read_text())
        j['public_enabled'] = False
        (nonpublic / 'rfoundry_dm32.json').write_text(json.dumps(j))
        self.build = nonpublic
        with self.assertRaisesRegex(SystemExit, 'not a public build'):
            self.run_build(out=self.top / 'o1')
        self.build = rt.write_plus(self.top / 'bad', fx.release_app('1.0.0'), identical=False, signed_for=False)
        with self.assertRaisesRegex(ValueError, 'reproducible'):
            self.run_build(out=self.top / 'o2')
        self.build = rt.write_plus(self.top / 'ok', fx.release_app('1.0.0'), signed_for=False)
        (self.top / 'exists').mkdir()
        with self.assertRaisesRegex(SystemExit, 'existing --out'):
            self.run_build(out=self.top / 'exists')
        with self.assertRaisesRegex(SystemExit, 'pc/dist'):
            self.run_build(out=rt.PC / 'release' / 'never')
        self.assertFalse((rt.PC / 'release' / 'never').exists())


if __name__ == '__main__':
    unittest.main()
