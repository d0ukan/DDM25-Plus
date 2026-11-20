"""Self-update of the frozen program: swap the program folder for a verified new one, roll back if it does not start.

1. The app downloads the new program zip named in a signed manifest and checks its size and SHA-256
   (releases.download_app), then extracts it safely into %LOCALAPPDATA%\\DDM32Plus\\guncelleme\\app-<version>\\
   (stage()). Nothing in the program folder changes yet.
2. The app starts the NEW program's .exe from that staging folder as the helper:
       DDM32Plus.exe --self-update --pid <old pid> --app-dir <program folder> --staging <staging> --marker <file>
   and closes itself.
3. The helper (swap()) waits for the old program to exit, renames the program folder to <folder>.old (kept for a
   manual rollback), copies the staging folder into the program folder's place, and starts it with
   --update-check <marker>. The new program writes the marker once its window is up (write_marker()).
4. No marker within the time limit: the helper stops the new program, moves it aside to <folder>.failed, renames
   <folder>.old back and starts the old program. Any error before the swap leaves the old folder untouched.
The result goes to guncelleme\\last-update.json; the next start shows it. User data is never in the program folder.
No Qt here: the helper runs before Qt is loaded, and the tests drive swap() on temporary folders.
"""
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import sys
import time
import zipfile

from . import EXE_NAME

HELPER_ARG = '--self-update'
CHECK_ARG = '--update-check'
START_TIMEOUT = 90
EXIT_TIMEOUT = 60
RESULT_FILE = 'last-update.json'


class UpdateError(Exception):
    pass


def safe_extract(zip_path, dest):
    """Extract a program zip whose members all sit under DDM32Plus/ (no absolute paths, no '..'); returns the
    extracted DDM32Plus folder"""
    dest = Path(dest)
    with zipfile.ZipFile(zip_path) as z:
        for info in z.infolist():
            name = info.filename
            parts = PurePosixPath(name).parts
            if (not parts or parts[0] != EXE_NAME or name.startswith('/') or '\\' in name or
                    any(p in ('..', '') or ':' in p for p in parts)):
                raise UpdateError(f'Unsafe or unexpected path in the program zip: {name!r}')
        z.extractall(dest)
    app = dest / EXE_NAME
    if not (app / f'{EXE_NAME}.exe').is_file():
        raise UpdateError(f'{EXE_NAME}.exe missing in the program zip')
    return app


def stage(zip_path, updates_dir, version):
    """The new program, extracted into updates_dir/app-<version>/DDM32Plus"""
    target = Path(updates_dir) / f'app-{version}'
    if target.exists():
        shutil.rmtree(target)
    try:
        return safe_extract(zip_path, target)
    except BaseException:
        shutil.rmtree(target, ignore_errors=True)
        raise


def helper_command(staged_app, pid, app_dir, marker, result):
    exe = Path(staged_app) / f'{EXE_NAME}.exe'
    return str(exe), [HELPER_ARG, '--pid', str(pid), '--app-dir', str(app_dir), '--staging', str(staged_app),
                      '--marker', str(marker), '--result', str(result)]


def write_marker(path, version):
    p = Path(path)
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps({'started': True, 'version': version, 'pid': os.getpid()}), encoding='utf-8')


def swap(app_dir, staging, start, wait_exit, wait_marker, stop, marker, log=print):
    """The helper's work. start(exe, args) -> handle; wait_exit() -> bool (old program gone);
    wait_marker(marker) -> bool; stop(handle). Returns 'ok', 'rolled_back' or 'aborted'"""
    app_dir, staging, marker = Path(app_dir), Path(staging), Path(marker)
    old = app_dir.with_name(app_dir.name + '.old')
    failed = app_dir.with_name(app_dir.name + '.failed')
    exe = f'{EXE_NAME}.exe'
    if not (staging / exe).is_file():
        log('aborted: the staged program has no .exe')
        return 'aborted'
    if not wait_exit():
        log('aborted: the old program did not exit; nothing changed')
        return 'aborted'
    if marker.exists():
        marker.unlink()
    try:
        if old.exists():
            shutil.rmtree(old)
        os.replace(app_dir, old)
    except OSError as exc:
        log(f'aborted: cannot move the program folder aside ({exc}); nothing changed')
        start(app_dir / exe, [])
        return 'aborted'
    try:
        shutil.copytree(staging, app_dir)
    except OSError as exc:
        log(f'copy failed ({exc}); putting the old program back')
        shutil.rmtree(app_dir, ignore_errors=True)
        os.replace(old, app_dir)
        start(app_dir / exe, [])
        return 'rolled_back'
    handle = start(app_dir / exe, [CHECK_ARG, str(marker)])
    if wait_marker(marker):
        log('ok: the new program started')
        return 'ok'
    log('the new program did not start in time; rolling back')
    stop(handle)
    try:
        if failed.exists():
            shutil.rmtree(failed)
        os.replace(app_dir, failed)
    except OSError:
        shutil.rmtree(app_dir, ignore_errors=True)
    os.replace(old, app_dir)
    start(app_dir / exe, [])
    return 'rolled_back'


# --- the real process functions (Windows) ---------------------------------------------------------------------------
def _wait_pid(pid, timeout):
    if sys.platform != 'win32':
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            try:
                os.kill(pid, 0)
            except OSError:
                return True
            time.sleep(0.5)
        return False
    import ctypes
    synchronize = 0x00100000
    handle = ctypes.windll.kernel32.OpenProcess(synchronize, False, int(pid))
    if not handle:
        return True             # already gone
    try:
        return ctypes.windll.kernel32.WaitForSingleObject(handle, int(timeout * 1000)) == 0
    finally:
        ctypes.windll.kernel32.CloseHandle(handle)


def _start(exe, args):
    return subprocess.Popen([str(exe)] + [str(a) for a in args], cwd=str(Path(exe).parent),
                            close_fds=True)


def _wait_marker(marker, timeout=START_TIMEOUT):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if Path(marker).is_file():
            return True
        time.sleep(0.5)
    return False


def _stop(handle):
    try:
        handle.kill()
    except Exception:
        pass
    time.sleep(1)


def helper_main(argv):
    """DDM32Plus.exe --self-update ...: exit code 0 when the new program runs"""
    import argparse
    ap = argparse.ArgumentParser(prog=f'{EXE_NAME} {HELPER_ARG}')
    ap.add_argument('--pid', type=int, required=True)
    ap.add_argument('--app-dir', type=Path, required=True)
    ap.add_argument('--staging', type=Path, required=True)
    ap.add_argument('--marker', type=Path, required=True)
    ap.add_argument('--result', type=Path, required=True)
    args = ap.parse_args(argv)
    lines = []

    def log(text):
        lines.append(f'{time.strftime("%H:%M:%S")} {text}')
    result = swap(args.app_dir, args.staging, _start, lambda: _wait_pid(args.pid, EXIT_TIMEOUT), _wait_marker,
                  _stop, args.marker, log)
    try:
        args.result.parent.mkdir(parents=True, exist_ok=True)
        args.result.write_text(json.dumps({'result': result, 'log': lines,
                                           'finished': time.strftime('%Y-%m-%d %H:%M:%S')}, ensure_ascii=False),
                               encoding='utf-8')
    except OSError:
        pass
    return 0 if result == 'ok' else 1


def read_result(updates_dir):
    """The last helper result, once (the file is removed after reading)"""
    p = Path(updates_dir) / RESULT_FILE
    try:
        data = json.loads(p.read_text(encoding='utf-8'))
        p.unlink()
        return data
    except (OSError, ValueError):
        return None


def cleanup(updates_dir, keep=None):
    """Old staging folders and zips (not the one running now)"""
    updates_dir = Path(updates_dir)
    if not updates_dir.is_dir():
        return
    for p in updates_dir.iterdir():
        if keep and Path(keep).resolve().is_relative_to(p.resolve()):
            continue
        try:
            if p.is_dir() and p.name.startswith('app-'):
                shutil.rmtree(p)
            elif p.suffix in ('.zip', '.partial'):
                p.unlink()
        except OSError:
            pass
