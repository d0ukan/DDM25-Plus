"""Where the program keeps things and how it starts a tool (source and frozen), and the --run-tool host."""
import contextlib
import io
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

import fixtures  # noqa: F401
import rtfixtures as rt
from ddm25_app import paths, toolhost


class LayoutTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.top = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_source_command(self):
        lay = paths.Layout.source(self.top / 'pc', data=self.top / 'data', python='py.exe')
        program, argv, shown = lay.tool_command('install_plus', ['--port', 'COM7'])
        self.assertEqual((program, argv), ('py.exe', ['-u', 'radiotools/install_plus.py', '--port', 'COM7']))
        self.assertTrue(shown.startswith('python -u radiotools/install_plus.py'))
        self.assertFalse(lay.can_self_update)

    def test_frozen_command(self):
        lay = paths.Layout.frozen_app(bundle=self.top / 'app', data=self.top / 'data', runner_exe='C:/x/DDM25Plus.exe')
        program, argv, shown = lay.tool_command('dm32_readonly_backup', ['--port', 'COM7'])
        self.assertEqual(program, 'C:/x/DDM25Plus.exe')
        self.assertEqual(argv, ['--run-tool', 'dm32_readonly_backup', '--port', 'COM7'])
        self.assertTrue(lay.can_self_update)
        self.assertFalse(lay.can_build)

    def test_user_data_outside_the_program_folder(self):
        with mock.patch.dict(os.environ, {'LOCALAPPDATA': str(self.top / 'local')}):
            os.environ.pop('DDM25_DATA', None)
            try:
                self.assertEqual(paths.data_dir(), self.top / 'local' / 'DDM25Plus')
            finally:
                os.environ['DDM25_DATA'] = fixtures.DATA_DIR
        lay = paths.Layout.frozen_app(bundle=self.top / 'app', data=self.top / 'data').ensure()
        for d in (lay.backups, lay.releases, lay.updates):
            self.assertTrue(d.is_dir())
            self.assertFalse(d.resolve().is_relative_to((self.top / 'app').resolve()))
        self.assertEqual(lay.rel(lay.backups / 'b'), str((lay.backups / 'b').resolve()))
        self.assertEqual(paths.settings_path().parent, paths.data_dir())

    def test_can_build_needs_repo_and_wsl(self):
        lay = paths.Layout.source(self.top, data=self.top / 'd')
        self.assertFalse(lay.can_build)                         # no repository
        lay = paths.Layout.source(self.top, data=self.top / 'd', repo=self.top)
        with mock.patch('shutil.which', return_value=None):
            self.assertFalse(lay.can_build)
        with mock.patch('shutil.which', return_value='C:/Windows/System32/wsl.exe'):
            self.assertTrue(lay.can_build)

    def test_unique_folder(self):
        (self.top / 'dm32-20990101').mkdir()
        self.assertEqual(paths.unique_folder(self.top, 'dm32-20990101').name, 'dm32-20990101-b')


class ToolHostTest(unittest.TestCase):
    def run_host(self, argv):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = toolhost.main_from_argv(argv)
        return code, out.getvalue(), err.getvalue()

    def test_not_a_tool_request(self):
        self.assertIsNone(toolhost.main_from_argv(['DDM25Plus.exe']))

    def test_unknown_tool_refused(self):
        code, _, err = self.run_host(['x', '--run-tool', 'install_firstboot_lab'])
        self.assertEqual(code, 2)
        self.assertIn('Unknown tool', err)
        code, out, _ = self.run_host(['x', '--run-tool'])
        self.assertEqual(code, 2)

    def test_runs_tool_main_in_a_bundle(self):
        with tempfile.TemporaryDirectory() as td:
            bundle = Path(td)
            (bundle / 'radiotools').mkdir()
            (bundle / 'radiotools' / 'radio_detect.py').write_text(
                'import sys\nprint("ran", sys.argv[1:])\nsys.exit(3)\n')
            with mock.patch.object(toolhost, '_bundle', lambda: bundle):
                saved = sys.argv[:]
                try:
                    code, out, _ = self.run_host(['x', '--run-tool', 'radio_detect', '--backup', 'b'])
                finally:
                    sys.argv = saved
            self.assertEqual(code, 3)
            self.assertIn("ran ['--backup', 'b']", out)

    @rt.need_upstream
    def test_self_check_from_source(self):
        code, out, _ = self.run_host(['x', '--run-tool', '_selfcheck'])
        self.assertIn('SELF-CHECK PASS', out, out)
        self.assertEqual(code, 0)
        self.assertIn('release public key: NOT EMBEDDED', out)
        self.assertIn('stock profiles', out)


if __name__ == '__main__':
    unittest.main()
