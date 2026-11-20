"""Where things are: the program, its radio tools, and the user's data (outside the program folder).

User data lives in %LOCALAPPDATA%\\DDM25Plus (DDM25_DATA overrides it), never in the program folder, so an update of
the program never touches it:
  settings.json     settings, the disclaimer acceptance, what was installed, which backups to restore from
  yedekler/         the radio's backups and every install / restore log and receipt (private: calibration, channels)
  surumler/         downloaded firmware releases, each with its signed manifest (verified again before use)
  guncelleme/       downloaded program updates and the self-update helper's staging folders

Two ways the program runs:
- source: python -m ddm25_app (pc/ of the repository). The radio tools are pc/radiotools/*.py, run with this Python.
- frozen: DDM25Plus.exe (PyInstaller one-folder build, no Python needed). The radio tools are bundled beside the
  .exe and run in a child process of the same .exe: DDM25Plus.exe --run-tool NAME ARGS (toolhost.py).
Either way every radio step is a separate process of an unchanged guarded tool.
"""
import json
import os
from pathlib import Path
import re
import shutil
import sys
import time

from . import APP_NAME, EXE_NAME  # noqa: F401

BACKUPS = 'yedekler'
RELEASES = 'surumler'
UPDATES = 'guncelleme'


def frozen():
    return bool(getattr(sys, 'frozen', False))


def app_dir():
    """The program folder: the .exe's folder (frozen) or pc/ (source)"""
    if frozen():
        return Path(sys.executable).resolve().parent
    return Path(__file__).resolve().parents[1]


def bundle_root():
    """Where the bundled radiotools/ and research/ are: PyInstaller's _MEIPASS (one-folder: the .exe's folder)"""
    return Path(getattr(sys, '_MEIPASS', app_dir())).resolve()


def data_dir():
    env = os.environ.get('DDM25_DATA')
    if env:
        return Path(env)
    base = os.environ.get('LOCALAPPDATA') or os.environ.get('APPDATA') or str(Path.home())
    return Path(base) / EXE_NAME


def settings_path():
    return data_dir() / 'settings.json'


def load_settings():
    try:
        return json.loads(settings_path().read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return {}


def save_setting(key, value):
    s = load_settings()
    s[key] = value
    p = settings_path()
    p.parent.mkdir(parents=True, exist_ok=True)
    tmp = p.with_suffix('.tmp')
    tmp.write_text(json.dumps(s, indent=2, ensure_ascii=False), encoding='utf-8')
    os.replace(tmp, p)


def repo_root():
    """The DDM25 Plus repository around pc/ when running from source (firmware sources at its root), else None"""
    if frozen():
        return None
    root = Path(__file__).resolve().parents[2]
    return root if (root / 'tools' / 'build_radiofoundry.py').is_file() else None


class Layout:
    """How a run of the program starts a tool and where it keeps the user's files.

    root: the tools' working folder (pc/ or the bundle). Paths handed to tools are absolute when outside it.
    """

    def __init__(self, mode, root, data, python=None, runner_exe=None, repo=None):
        self.mode = mode                      # 'source' or 'frozen'
        self.root = Path(root)
        self.data = Path(data)
        self.backups = self.data / BACKUPS
        self.releases = self.data / RELEASES
        self.updates = self.data / UPDATES
        self.python = python
        self.runner_exe = runner_exe
        self.repo = Path(repo) if repo else None
        self.force_build = None               # tests: True/False instead of looking for WSL

    @classmethod
    def source(cls, pc_dir=None, data=None, python=None, repo=None):
        pc_dir = Path(pc_dir) if pc_dir else app_dir()
        return cls('source', pc_dir, data or data_dir(), python=python or sys.executable, repo=repo)

    @classmethod
    def frozen_app(cls, bundle=None, data=None, runner_exe=None):
        return cls('frozen', bundle or bundle_root(), data or data_dir(), runner_exe=runner_exe or sys.executable)

    @property
    def is_frozen(self):
        return self.mode == 'frozen'

    @property
    def can_build(self):
        """The data downloads and the WSL firmware build: only in a source checkout with the firmware tree and WSL"""
        if self.force_build is not None:
            return bool(self.force_build)
        return (self.mode == 'source' and self.repo is not None and
                bool(shutil.which('wsl.exe') or shutil.which('wsl')))

    @property
    def can_self_update(self):
        return self.mode == 'frozen'

    def rel(self, path):
        """A path as handed to a tool: relative to root when inside it, else absolute"""
        p = Path(path)
        try:
            return p.resolve().relative_to(self.root.resolve()).as_posix()
        except ValueError:
            return str(p.resolve())

    def tool_command(self, name, args):
        """(program, argv, shown) to run radiotools/<name>.py with args in root; shown: the command for the log"""
        args = [str(a) for a in args]
        if self.mode == 'source':
            argv = ['-u', f'radiotools/{name}.py'] + args
            return self.python, argv, 'python ' + ' '.join(argv)
        argv = ['--run-tool', name] + args
        return str(self.runner_exe), argv, f'{EXE_NAME} --run-tool ' + ' '.join([name] + args)

    def ensure(self):
        for d in (self.backups, self.releases, self.updates):
            d.mkdir(parents=True, exist_ok=True)
        return self


def detect_layout():
    if frozen():
        return Layout.frozen_app().ensure()
    return Layout.source(repo=repo_root()).ensure()


def unique_folder(parent, base):
    """parent/base, or base-b, base-c ... the first that does not exist (the tools refuse an existing --out)"""
    parent = Path(parent)
    if not (parent / base).exists():
        return parent / base
    for c in 'bcdefghijklmnopqrstuvwxyz':
        if not (parent / f'{base}-{c}').exists():
            return parent / f'{base}-{c}'
    return parent / f'{base}-{int(time.time())}'


def backup_log_path(backup_dir):
    backup_dir = Path(backup_dir)
    m = re.search(r'(\d{8}(?:-[a-z])?)$', backup_dir.name)
    stem = m.group(1) if m else backup_dir.name
    return backup_dir.parent / f'backup-{stem}.log'


def install_log_path(install_dir):
    install_dir = Path(install_dir)
    return install_dir.parent / f'{install_dir.name}.log'


def today():
    return time.strftime('%Y%m%d')


def now_utc():
    return time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())
