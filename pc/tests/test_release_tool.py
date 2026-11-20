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

from ddm32_app import releases  # noqa: E402
from radiotools import ed25519, install_plus  # noqa: E402


class ReleaseToolTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.top = Path(self.tmp.name)
        self.key = self.top / 'keys' / 'release-signing-key'
        self.restricted = []
        self.public, ok = mr.keygen(self.key, restrict_fn=self.restricted.append)
        self.assertTrue(ok)
        self.build = rt.write_plus(self.top / 'build', fx.release_app('1.0.0') + b'DDM32 Plus', signed_for=False)

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
        # The new place, or the key made before the rename when only that one exists (never in the repository)
        self.assertIn(mr.DEFAULT_KEY, (Path.home() / '.ddm32' / 'release-signing-key',
                                       Path.home() / '.ddm25' / 'release-signing-key'))
        if mr.DEFAULT_KEY == mr.LEGACY_KEY:
            self.assertFalse((Path.home() / '.ddm32' / 'release-signing-key').exists())

    def test_build_signs_and_verifies(self):
        code, out, text = self.run_build()
        self.assertEqual(code, 0)
        self.assertIn('Nothing was uploaded', text)
        names = sorted(p.name for p in out.iterdir())
        self.assertEqual(names, ['ddm32-manifest.json', 'ddm32-manifest.json.sig', 'ddm32-plus-1.0.0-beta.1.zip'])
        m = mr.verify(out, self.public)
        e = m['firmware'][0]
        self.assertEqual((e['channel'], e['settings_format'], e['codeplug_format']), ('beta', 1, 2))
        self.assertEqual(e['url'], 'https://github.com/d0ukan/DDM32-Plus/releases/download/v1.0.0-beta.1/'
                                   'ddm32-plus-1.0.0-beta.1.zip')
        self.assertEqual(e['notes'], {'tr': 'İlk beta', 'en': 'First beta'})
        with self.assertRaises(rt.sm.ManifestError):
            mr.verify(out, rt.TEST_PUBLIC_HEX)                       # another key does not verify it
        with zipfile.ZipFile(out / 'ddm32-plus-1.0.0-beta.1.zip') as z:
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
                for n in ('ddm32-manifest.json', 'ddm32-manifest.json.sig')]}]).encode()
            root = self.top / 'surumler'
            cat = releases.load_catalog(root, lambda url, limit=None: files[url])
            fv = cat.find('1.0.0-beta.1')

            def download(url, dest, size, progress=None):
                Path(dest).write_bytes(files[url])
            folder = releases.download_firmware(fv, root, download)
            self.assertEqual(install_plus.plus_application(folder), (self.build / 'rfoundry_dm32.bin').read_bytes())

    def test_program_zip(self):
        appzip = self.top / 'DDM32Plus-1.0.0-beta.2.zip'
        with zipfile.ZipFile(appzip, 'w') as z:
            z.writestr('DDM32Plus/DDM32Plus.exe', b'MZ')
            z.writestr('DDM32Plus/radiotools/dm32_readonly_backup.py', b'# the backup tool is public source')
        code, out, _ = self.run_build('--app-zip', str(appzip), '--app-version', '1.0.0-beta.2')
        m = mr.verify(out, self.public)
        self.assertEqual(m['app']['version'], '1.0.0-beta.2')
        self.assertTrue((out / 'DDM32Plus-1.0.0-beta.2.zip').is_file())
        bad = self.top / 'bad.zip'
        with zipfile.ZipFile(bad, 'w') as z:
            z.writestr('DDM32Plus/DDM32Plus.exe', b'MZ')
            z.writestr('DDM32Plus/yedekler/progmem-pass1.bin', b'private')
        with self.assertRaisesRegex(SystemExit, 'must not be published'):
            self.run_build('--app-zip', str(bad), '--app-version', '1.0.0-beta.2', out=self.top / 'out2')
        for i, private in enumerate(('DDM32Plus/Backups/notes.txt', 'DDM32Plus/x/backup-manifest.json')):
            bad = self.top / f'bad{i}.zip'
            with zipfile.ZipFile(bad, 'w') as z:
                z.writestr('DDM32Plus/DDM32Plus.exe', b'MZ')
                z.writestr(private, b'private')
            with self.subTest(private=private), self.assertRaisesRegex(SystemExit, 'must not be published'):
                self.run_build('--app-zip', str(bad), '--app-version', '1.0.0-beta.2', out=self.top / f'o{i}')

    def net_zip(self, name, *members):
        path = self.top / name
        with zipfile.ZipFile(path, 'w') as z:
            z.writestr('DDM32Plus/DDM32Plus.exe', b'MZ net')
            for m in members:
                z.writestr(m, b'x')
        return path

    def test_windows_program_zip(self):
        """app_net: the Windows (.NET) program, beside the Python program's app entry; each its own zip"""
        py = self.net_zip('py.zip', 'DDM32Plus/radiotools/install_plus.py')
        net = self.net_zip('net.zip')
        code, out, text = self.run_build('--app-zip', str(py), '--app-version', '1.0.0-beta.2',
                                         '--app-net-zip', str(net), '--app-net-version', '2.0.0-alpha.2',
                                         '--app-net-notes-tr', 'Windows programı', '--app-net-notes-en', 'Windows program')
        self.assertEqual(code, 0)
        m = mr.verify(out, self.public)
        self.assertEqual(m['app']['version'], '1.0.0-beta.2')
        self.assertEqual(m['app_net']['version'], '2.0.0-alpha.2')
        self.assertEqual(m['app_net']['file'], 'DDM32Plus-win-2.0.0-alpha.2.zip')
        self.assertEqual(m['app_net']['notes'], {'tr': 'Windows programı', 'en': 'Windows program'})
        self.assertEqual((out / 'DDM32Plus-win-2.0.0-alpha.2.zip').read_bytes(), net.read_bytes())
        # the Python program's catalog still takes only "app" as its own update
        cat = releases.Catalog()
        cat.add(m['release'], m, out)
        self.assertEqual([a[0]['version'] for a in cat.apps], ['1.0.0-beta.2'])
        # a changed Windows zip fails verify
        (out / 'DDM32Plus-win-2.0.0-alpha.2.zip').write_bytes(b'changed')
        with self.assertRaisesRegex(SystemExit, 'differs from the manifest'):
            mr.verify(out, self.public)

    def test_windows_program_zip_refusals(self):
        for i, member in enumerate(('DDM32Plus/yedekler/x.txt', 'DDM32Plus/settings.json', 'DDM32Plus/Guncelleme/a',
                                    'DDM32Plus/unins000.exe', 'Other/x.txt', 'DDM32Plus/x/backup-manifest.json')):
            bad = self.net_zip(f'bad-net{i}.zip', member)
            with self.subTest(member=member), self.assertRaises(SystemExit):
                self.run_build('--app-net-zip', str(bad), '--app-net-version', '2.0.0', out=self.top / f'n{i}')
            self.assertFalse((self.top / f'n{i}').exists())
        with self.assertRaisesRegex(SystemExit, '--app-net-version'):
            self.run_build('--app-net-zip', str(self.net_zip('ok.zip')), out=self.top / 'n-noversion')

    def test_app_net_schema_is_checked(self):
        base = rt.manifest([rt.firmware_entry(fx.release_app('1.0.0'))], app=rt.app_entry('1.0.0', b'z'))
        good = dict(base, app_net=dict(rt.app_entry('2.0.0', b'n'), file='DDM32Plus-win-2.0.0.zip',
                                       url=rt.url('v2.0.0', 'DDM32Plus-win-2.0.0.zip')))
        data, sig = rt.signed(good)
        self.assertEqual(rt.sm.verify_bytes(data, sig, rt.TEST_PUBLIC_HEX)['app_net']['version'], '2.0.0')
        for field, value in (('url', 'https://example.com/DDM32Plus-win-2.0.0.zip'), ('sha256', 'x'),
                             ('notes', 'text'), ('version', '2.0')):
            bad = json.loads(json.dumps(good))
            bad['app_net'][field] = value
            data, sig = rt.signed(bad)
            with self.subTest(field=field), self.assertRaises(rt.sm.ManifestError):
                rt.sm.verify_bytes(data, sig, rt.TEST_PUBLIC_HEX)

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
