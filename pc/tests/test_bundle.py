"""The stand-alone program's contents: every file the bundled tools read is listed, the pinned upstream files match
their hashes, and no private data can reach a program folder or its zip. The scan also runs on pc/dist/DDM25Plus
when a build is there."""
import contextlib
import hashlib
import io
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile

import rtfixtures as rt

HERE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HERE))
import bundle  # noqa: E402
from radiotools import upstream  # noqa: E402

DIST = HERE / 'dist' / bundle.NAME


@rt.need_upstream
class BundleInputsTest(unittest.TestCase):
    def test_files_exist_and_pins_match(self):
        files = bundle.bundled_files()
        self.assertIn((HERE / 'radiotools' / 'install_plus.py', 'radiotools'), files)
        root = upstream.root()
        for rel, want in upstream.PINNED.items():
            self.assertEqual(hashlib.sha256((root / rel).read_bytes()).hexdigest(), want, rel)
            self.assertIn((root / rel, str(Path(bundle.UPSTREAM_DEST, rel).parent).replace('\\', '/')), files)
        bundle.verify_pins()

    def test_tool_imports_closed(self):
        """Every 'from radiotools import X' of a bundled tool is bundled too (else the frozen tool fails)"""
        for t in bundle.TOOLS:
            for name in bundle.imports_of(HERE / 'radiotools' / f'{t}.py'):
                parts = name.split('.')
                if parts[0] == 'radiotools' and len(parts) > 1 and (HERE / 'radiotools' / f'{parts[1]}.py').is_file():
                    self.assertIn(parts[1], bundle.TOOLS, f'{t} imports radiotools.{parts[1]}')
        on_disk = {p.stem for p in (HERE / 'radiotools').glob('*.py')}
        self.assertEqual(on_disk, set(bundle.TOOLS))

    def test_app_tools_bundled(self):
        from ddm25_app import toolhost
        self.assertTrue(set(toolhost.TOOLS) <= set(bundle.TOOLS))

    def test_hidden_imports_cover_serial(self):
        h = bundle.hidden_imports()
        for m in ('serial', 'serial.serialwin32', 'serial.tools.list_ports', 'zlib', 'struct', 'runpy', 'hashlib'):
            self.assertIn(m, h)
        self.assertFalse([m for m in h if m.split('.')[0] in bundle.LOCAL_MODULES])

    def test_never_private_inputs(self):
        for src, dest in bundle.bundled_files():
            text = src.resolve().as_posix()
            for bad in ('backups/', 'artifacts/', 'research-data', 'research/originals', '/build/', 'yedekler'):
                self.assertNotIn(bad, text)
            self.assertNotIn(src.suffix, ('.bin', '.map', '.sig'))

    def test_args(self):
        args = bundle.pyinstaller_args()
        self.assertIn('--windowed', args)
        self.assertEqual(args[-1], str(HERE / 'run_ddm25.py'))
        self.assertIn('radiotools', [args[i + 1] for i, a in enumerate(args) if a == '--exclude-module'])
        datas = [args[i + 1] for i, a in enumerate(args) if a == '--add-data']
        self.assertEqual(len(datas), len(bundle.bundled_files()))


class ScanTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name) / bundle.NAME
        self.dir.mkdir()
        (self.dir / 'DDM25Plus.exe').write_bytes(b'MZ')
        (self.dir / 'radiotools').mkdir()
        (self.dir / 'radiotools' / 'install_plus.py').write_text('x = 1\n')
        reg = self.dir / bundle.UPSTREAM_DEST / 'tools/rfoundry/rfoundry/registry.json'
        reg.parent.mkdir(parents=True)
        reg.write_text('{}')
        (self.dir / bundle.README_NAME).write_text(bundle.README_TEXT, encoding='utf-8')

    def tearDown(self):
        self.tmp.cleanup()

    def put(self, rel, data=b'x'):
        p = self.dir / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
        return p

    def test_clean(self):
        self.assertEqual(bundle.scan(self.dir), [])

    def test_ca_bundle_is_the_only_pem_allowed(self):
        self.put('_internal/certifi/cacert.pem', b'-----BEGIN CERTIFICATE-----')
        self.put('certifi/cacert.pem', b'-----BEGIN CERTIFICATE-----')
        self.assertEqual(bundle.scan(self.dir), [])
        self.put('_internal/certifi/signing.pem')
        self.put('_internal/keys/cacert.pem')
        self.assertEqual({rel for rel, _ in bundle.scan(self.dir)},
                         {'_internal/certifi/signing.pem', '_internal/keys/cacert.pem'})

    def test_private_things_found(self):
        cases = ('backups/dm32/q128-pass1.bin', 'yedekler/dm32-20261119/backup-manifest.json',
                 'surumler/1.0.0/rfoundry_dm32.bin', 'research-data/user.csv', 'firmware/rptrs.json',
                 'artifacts/plus-x/rfoundry_dm32.map', 'research/originals/DM32.01.01.047.bin', 'codec/slot.raw',
                 'settings.json', 'logs/build.log', 'lists/x.txt', 'radiotools/__pycache__/a.pyc',
                 'release-signing-key', 'keys/signing.pem', 'ddm25-manifest.json.sig', 'guncelleme/app.zip')
        for rel in cases:
            self.put(rel, b'RADIO_ID,CALLSIGN,FIRST\n' if rel.endswith('.txt') else b'x')
        flagged = {rel for rel, _ in bundle.scan(self.dir)}
        for rel in cases:
            parts = rel.split('/')
            ancestors = {'/'.join(parts[:i]) for i in range(1, len(parts) + 1)}
            self.assertTrue(ancestors & flagged, rel)

    def test_package_refuses_and_zips(self):
        out = Path(self.tmp.name) / 'p.zip'
        with contextlib.redirect_stdout(io.StringIO()):
            z = bundle.package(self.dir, out)
        names = zipfile.ZipFile(z).namelist()
        self.assertIn('DDM25Plus/DDM25Plus.exe', names)
        self.assertTrue(all(n.startswith('DDM25Plus/') for n in names))   # the self-update helper expects this
        self.put('yedekler/dm32/q128-pass1.bin')
        out.unlink()
        with self.assertRaises(SystemExit), contextlib.redirect_stdout(io.StringIO()):
            bundle.package(self.dir, out)
        self.assertFalse(out.exists())


@unittest.skipUnless((DIST / 'DDM25Plus.exe').is_file(), 'no build in pc/dist/DDM25Plus')
class BuiltProgramTest(unittest.TestCase):
    def test_dist_has_no_private_data(self):
        self.assertEqual(bundle.scan(DIST), [])

    def test_dist_has_tools_and_pins(self):
        for t in bundle.TOOLS:
            self.assertTrue((DIST / 'radiotools' / f'{t}.py').is_file(), t)
        for rel, want in upstream.PINNED.items():
            self.assertEqual(hashlib.sha256((DIST / bundle.UPSTREAM_DEST / rel).read_bytes()).hexdigest(), want, rel)

    def test_dist_zips(self):
        for z in (HERE / 'dist').glob(f'{bundle.NAME}-*.zip'):
            names = zipfile.ZipFile(z).namelist()
            self.assertFalse([n for n in names if Path(n).suffix.lower() in bundle.FORBIDDEN_SUFFIXES
                              and n.split('/', 1)[1] not in bundle.CA_BUNDLE_FILES], z.name)
            self.assertFalse([n for n in names if set(n.lower().split('/')) & bundle.FORBIDDEN_DIRS], z.name)


if __name__ == '__main__':
    unittest.main()
