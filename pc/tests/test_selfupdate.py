"""The self-update swap and rollback on temporary folders, with fake processes (nothing is started)."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile

import fixtures  # noqa: F401  (test data folder)
from ddm25_app import selfupdate


def make_app(folder, tag):
    folder.mkdir(parents=True)
    (folder / 'DDM25Plus.exe').write_text(f'exe {tag}')
    (folder / 'lib.dll').write_text(f'lib {tag}')
    return folder


class Procs:
    """Fake start/stop: records what would run; the 'new program' writes its marker when told to"""

    def __init__(self, new_starts=True):
        self.started = []
        self.stopped = []
        self.new_starts = new_starts

    def start(self, exe, args):
        self.started.append((Path(exe), list(args), Path(exe).read_text()))
        if selfupdate.CHECK_ARG in args and self.new_starts:
            selfupdate.write_marker(args[args.index(selfupdate.CHECK_ARG) + 1], '9.9.9')
        return len(self.started)

    def stop(self, handle):
        self.stopped.append(handle)

    def wait_marker(self, marker):
        return Path(marker).is_file()


class SwapTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.top = Path(self.tmp.name)
        self.app = make_app(self.top / 'Programs' / 'DDM25Plus', 'old')
        self.staging = make_app(self.top / 'data' / 'guncelleme' / 'app-9.9.9' / 'DDM25Plus', 'new')
        self.marker = self.top / 'data' / 'guncelleme' / 'started.json'
        self.userdata = self.top / 'data' / 'yedekler'
        self.userdata.mkdir(parents=True)
        (self.userdata / 'backup-manifest.json').write_text('{}')

    def tearDown(self):
        self.tmp.cleanup()

    def swap(self, procs, exited=True):
        return selfupdate.swap(self.app, self.staging, procs.start, lambda: exited, procs.wait_marker, procs.stop,
                               self.marker, log=lambda s: None)

    def test_ok_keeps_old_for_rollback(self):
        p = Procs()
        self.assertEqual(self.swap(p), 'ok')
        self.assertEqual((self.app / 'DDM25Plus.exe').read_text(), 'exe new')
        old = self.app.with_name('DDM25Plus.old')
        self.assertEqual((old / 'DDM25Plus.exe').read_text(), 'exe old')
        self.assertEqual(p.started[-1][1], [selfupdate.CHECK_ARG, str(self.marker)])
        self.assertEqual(p.started[-1][2], 'exe new')
        self.assertTrue((self.userdata / 'backup-manifest.json').is_file())   # user data untouched

    def test_rollback_when_new_does_not_start(self):
        p = Procs(new_starts=False)
        self.assertEqual(self.swap(p), 'rolled_back')
        self.assertEqual((self.app / 'DDM25Plus.exe').read_text(), 'exe old')
        self.assertEqual((self.app.with_name('DDM25Plus.failed') / 'DDM25Plus.exe').read_text(), 'exe new')
        self.assertFalse(self.app.with_name('DDM25Plus.old').exists())
        self.assertEqual(p.stopped, [1])                       # the new program was stopped
        self.assertEqual(p.started[-1][2], 'exe old')         # and the old one started again
        self.assertEqual(p.started[-1][1], [])

    def test_abort_when_old_program_keeps_running(self):
        p = Procs()
        self.assertEqual(self.swap(p, exited=False), 'aborted')
        self.assertEqual((self.app / 'DDM25Plus.exe').read_text(), 'exe old')
        self.assertEqual(p.started, [])
        self.assertFalse(self.app.with_name('DDM25Plus.old').exists())

    def test_copy_failure_puts_old_back(self):
        p = Procs()
        with patch('shutil.copytree', side_effect=OSError('disk full')):
            self.assertEqual(self.swap(p), 'rolled_back')
        self.assertEqual((self.app / 'DDM25Plus.exe').read_text(), 'exe old')
        self.assertEqual(p.started[-1][2], 'exe old')

    def test_cannot_move_folder_aside(self):
        p = Procs()
        with patch('os.replace', side_effect=PermissionError('in use')):
            self.assertEqual(self.swap(p), 'aborted')
        self.assertEqual((self.app / 'DDM25Plus.exe').read_text(), 'exe old')

    def test_previous_rollback_copy_is_replaced(self):
        make_app(self.app.with_name('DDM25Plus.old'), 'older')
        self.assertEqual(self.swap(Procs()), 'ok')
        self.assertEqual((self.app.with_name('DDM25Plus.old') / 'DDM25Plus.exe').read_text(), 'exe old')

    def test_staging_without_exe(self):
        (self.staging / 'DDM25Plus.exe').unlink()
        self.assertEqual(self.swap(Procs()), 'aborted')
        self.assertEqual((self.app / 'DDM25Plus.exe').read_text(), 'exe old')

    def test_helper_main_writes_result(self):
        p = Procs()
        result = self.top / 'data' / 'guncelleme' / 'last-update.json'
        with patch.object(selfupdate, '_start', p.start), patch.object(selfupdate, '_wait_pid', lambda pid, t: True), \
                patch.object(selfupdate, '_wait_marker', p.wait_marker), patch.object(selfupdate, '_stop', p.stop):
            code = selfupdate.helper_main(['--pid', '123', '--app-dir', str(self.app), '--staging', str(self.staging),
                                           '--marker', str(self.marker), '--result', str(result)])
        self.assertEqual(code, 0)
        self.assertEqual(json.loads(result.read_text(encoding='utf-8'))['result'], 'ok')
        self.assertEqual(selfupdate.read_result(result.parent)['result'], 'ok')
        self.assertIsNone(selfupdate.read_result(result.parent))       # shown once

    def test_toolhost_dispatches_helper(self):
        from ddm25_app import toolhost
        with patch.object(selfupdate, 'helper_main', return_value=7) as h:
            self.assertEqual(toolhost.main_from_argv(['x.exe', '--self-update', '--pid', '1']), 7)
        h.assert_called_once_with(['--pid', '1'])


class ExtractTest(unittest.TestCase):
    def zip(self, names):
        tmp = Path(tempfile.mkdtemp(prefix='ddm25-z-'))
        z = tmp / 'app.zip'
        with zipfile.ZipFile(z, 'w') as f:
            for n in names:
                f.writestr(n, b'x')
        return tmp, z

    def test_good_zip_stages(self):
        tmp, z = self.zip(['DDM25Plus/DDM25Plus.exe', 'DDM25Plus/radiotools/install_plus.py'])
        staged = selfupdate.stage(z, tmp / 'updates', '2.0.0')
        self.assertTrue((staged / 'DDM25Plus.exe').is_file())
        self.assertEqual(staged.parent.name, 'app-2.0.0')

    def test_unsafe_zips(self):
        for names in (['DDM25Plus/DDM25Plus.exe', '../evil.exe'], ['DDM25Plus/DDM25Plus.exe', '/abs.txt'],
                      ['Other/DDM25Plus.exe'], ['DDM25Plus/../../x'], ['DDM25Plus/readme.txt'],
                      ['DDM25Plus/C:evil']):
            tmp, z = self.zip(names)
            with self.subTest(names=names), self.assertRaises(selfupdate.UpdateError):
                selfupdate.stage(z, tmp / 'updates', '2.0.0')
            self.assertFalse((tmp / 'updates' / 'app-2.0.0').exists())
            self.assertFalse((tmp / 'evil.exe').exists())

    def test_cleanup_keeps_running_copy(self):
        tmp = Path(tempfile.mkdtemp(prefix='ddm25-c-'))
        (tmp / 'app-1.0.0' / 'DDM25Plus').mkdir(parents=True)
        (tmp / 'app-2.0.0' / 'DDM25Plus').mkdir(parents=True)
        (tmp / 'x.zip').write_bytes(b'z')
        selfupdate.cleanup(tmp, keep=tmp / 'app-2.0.0' / 'DDM25Plus')
        self.assertFalse((tmp / 'app-1.0.0').exists())
        self.assertTrue((tmp / 'app-2.0.0').exists())
        self.assertFalse((tmp / 'x.zip').exists())


if __name__ == '__main__':
    unittest.main()
