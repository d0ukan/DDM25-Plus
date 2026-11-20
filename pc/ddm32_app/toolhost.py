"""Runs one bundled radio tool inside the frozen program: DDM32Plus.exe --run-tool NAME ARGS; and the self-update
helper: DDM32Plus.exe --self-update ... (selfupdate.py).

The GUI starts a tool as its own child process (QProcess), exactly as it starts `python -u radiotools/NAME.py ARGS`
from source, so a crash or a hang of a tool never blocks the window and the output parsers stay the same. The tool is
the unchanged source file bundled at build time (radiotools/NAME.py beside the .exe), run as __main__ with runpy: its
ROOT (the folder above radiotools/) is the bundle, where the pinned upstream files sit under research/ and are still
loaded through their own SHA-256 checks (radiotools/upstream.py).

Only the tools the app drives may run. No Qt is imported here.
"""
import os
from pathlib import Path
import runpy
import sys
import traceback

TOOLS = ('install_plus', 'install_firstboot', 'dm32_readonly_backup', 'radio_detect', 'plus_txdiag', 'plus_live')
SELF_CHECK = '_selfcheck'


def _bundle():
    return Path(getattr(sys, '_MEIPASS', Path(__file__).resolve().parents[1])).resolve()


def _stdio():
    """A windowed .exe may start without usable std streams: attach the inherited handles, line buffered, UTF-8"""
    for name, fd, mode in (('stdout', 1, 'w'), ('stderr', 2, 'w')):
        stream = getattr(sys, name)
        if stream is None:
            try:
                stream = open(fd, mode, encoding='utf-8', errors='replace', closefd=False)
            except OSError:
                stream = open(os.devnull, mode)
            setattr(sys, name, stream)
        try:
            stream.reconfigure(encoding='utf-8', errors='replace', line_buffering=True)
        except (AttributeError, ValueError):
            pass


def self_check(bundle):
    """Loads what the tools load, without a port: the bundled tools, every pinned upstream file (hash), the monitor's
    RAM image, the codec tool with its DM32 windows, pyserial and the release key. Exit code 0 when all pass"""
    sys.path.insert(0, str(bundle))
    ok = True
    for name in TOOLS:
        path = bundle / 'radiotools' / f'{name}.py'
        print(f'tool {name}: {"ok" if path.is_file() else "MISSING"}')
        ok &= path.is_file()
    try:
        from radiotools import dm32_readonly_backup as backup, prepare_unit_image as unit, signed_manifest as sm
        from radiotools import upstream
        import importlib.util
        root = upstream.verify()
        print(f'pinned RadioFoundry files: {len(upstream.PINNED)} sha256 ok ({upstream.PIN[:8]}, '
              f'{"bundled" if Path(root).resolve().is_relative_to(bundle) else root})')
        backup.load_upstream()
        print('pinned monitor (stubtool.py, RAM image): sha256 ok')
        spec = importlib.util.spec_from_file_location('selfcheck_debug', upstream.path(upstream.DEBUG))
        spec.loader.exec_module(importlib.util.module_from_spec(spec))
        print('pinned debug protocol (rfoundry_debug.py): loads')
        codec = unit.load_codec()
        n = sum(1 for p in codec.PROFILES.values() if p.get('board') == 'dm32')
        print(f'codec slot writer (hrc7000_codec.py): DM32 windows ok, {n} stock profiles')
        import serial  # noqa: F401
        from serial.tools import list_ports  # noqa: F401
        print('pyserial: ok')
        from radiotools import ed25519
        sk = bytes(range(32))
        good = ed25519.verify(ed25519.secret_to_public(sk), b'selfcheck', ed25519.sign(sk, b'selfcheck'))
        print(f'ed25519 verify: {"ok" if good else "FAIL"}')
        ok &= good
        if sm.PUBLIC_KEY_HEX:
            print(f'release public key: {sm.key_id(sm.public_key())} (embedded)')
        else:
            print('release public key: NOT EMBEDDED (nothing can be installed until the maintainer embeds it)')
    except Exception as exc:        # reported, exit code 1
        print(f'FAIL: {type(exc).__name__}: {exc}')
        ok = False
    print('SELF-CHECK ' + ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


def run_tool(name, argv):
    """Exit code of radiotools/<name>.py run as __main__ with argv"""
    _stdio()
    sys.dont_write_bytecode = True      # nothing written into the program folder
    bundle = _bundle()
    if name == SELF_CHECK:
        return self_check(bundle)
    if name not in TOOLS:
        print(f'Unknown tool {name!r}; allowed: {", ".join(TOOLS)}', file=sys.stderr)
        return 2
    path = bundle / 'radiotools' / f'{name}.py'
    if not path.is_file():
        print(f'Bundled tool missing: {path}', file=sys.stderr)
        return 2
    sys.path.insert(0, str(bundle))
    sys.argv = [str(path)] + list(argv)
    try:
        runpy.run_path(str(path), run_name='__main__')
    except SystemExit as exc:
        code = exc.code
        if code is None:
            return 0
        if isinstance(code, int):
            return code
        print(code, file=sys.stderr)
        return 1
    except BaseException:
        traceback.print_exc()
        return 1
    finally:
        for s in (sys.stdout, sys.stderr):
            try:
                s.flush()
            except (OSError, ValueError, AttributeError):
                pass
    return 0


def main_from_argv(argv):
    """argv is sys.argv; returns an exit code for --run-tool / --self-update, else None (start the window)"""
    if len(argv) >= 3 and argv[1] == '--run-tool':
        return run_tool(argv[2], argv[3:])
    if len(argv) == 2 and argv[1] == '--run-tool':
        print('usage: DDM32Plus --run-tool NAME [ARGS]  (NAME: %s)' % ', '.join(TOOLS))
        return 2
    if len(argv) >= 2 and argv[1] == '--self-update':
        from .selfupdate import helper_main
        return helper_main(argv[2:])
    return None
