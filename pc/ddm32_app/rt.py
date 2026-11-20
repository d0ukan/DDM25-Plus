"""Access to the radio tools' shared modules (radiotools/signed_manifest.py) from the app.

The app itself never imports a module that talks to the radio; it only needs the signed-manifest checks, from the same
source file the installer uses (one copy of the gate and of the embedded public key). From source that is
pc/radiotools; in the frozen program it is the bundled radiotools/ beside the .exe (kept out of PyInstaller's archive,
so the tools and the app load the very same files).
"""
import sys

from . import paths


def _ensure_path():
    root = str(paths.bundle_root() if paths.frozen() else paths.app_dir())
    if root not in sys.path:
        sys.path.insert(0, root)


def signed_manifest():
    _ensure_path()
    from radiotools import signed_manifest as sm
    return sm
