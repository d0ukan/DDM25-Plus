#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Make a signed DDM25 Plus release: the firmware zip, optionally the program zip, ddm25-manifest.json and its
Ed25519 signature. Uploads NOTHING: the files land in --out; publishing them on GitHub is a separate, approved step.

  --keygen [--key PATH]
      Creates the release signing key (32 random bytes, hex) at %USERPROFILE%\\.ddm25\\release-signing-key (or PATH,
      never inside the repository), restricts it to the current user (icacls on Windows, mode 600 elsewhere) and
      prints the public key. Paste that into pc/radiotools/signed_manifest.py PUBLIC_KEY_HEX and build the program
      again: only then can the program verify (and so install) anything. Refuses to overwrite an existing key.

  build --version V --channel beta|stable --build-dir DIR --settings-format N --codeplug-format N
        --min-compatible V --notes-tr TEXT --notes-en TEXT [--app-zip ZIP --app-version V
        [--app-notes-tr TEXT --app-notes-en TEXT]] [--tag TAG] --out NEWDIR [--key PATH]
      DIR is a public DDM25 Plus build (tools/build_radiofoundry.py --plus --public: rfoundry_dm32.bin/.json/.map).
      Checks it like the installer does (reproducible Plus build, hashes, codec layout, size) and requires
      public_enabled. The release zip carries the application, a reduced linker map (the __codec_* symbol lines the
      installer checks) and a reduced build manifest (no build-machine paths). Signs the manifest and verifies the
      signature with the key's public half before it finishes.

  verify DIR [--public-key HEX]
      Verifies DIR/ddm25-manifest.json(.sig) and every asset in DIR against the manifest's sizes and hashes.

settings_format / codeplug_format: integers that change when the firmware's stored settings / channel layout changes;
min_compatible_version: the oldest firmware version that reads what this version saves. The program warns (and asks
twice) when a version change goes back across these.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import zipfile

PC = Path(__file__).resolve().parents[1]
REPO = PC.parent
sys.path.insert(0, str(PC))
from radiotools import ed25519  # noqa: E402
from radiotools import install_plus  # noqa: E402
from radiotools import signed_manifest as sm  # noqa: E402

DEFAULT_KEY = Path.home() / '.ddm25' / 'release-signing-key'
FIRMWARE_FILES = ('rfoundry_dm32.bin', 'rfoundry_dm32.json', 'rfoundry_dm32.map')
ZIP_TIME = (2026, 1, 1, 0, 0, 0)
EXE = 'DDM25Plus'


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def _inside_repo(path):
    try:
        Path(path).resolve().relative_to(REPO.resolve())
        return True
    except ValueError:
        return False


# --- key -----------------------------------------------------------------------------------------------------------
def restrict(path):
    """Only the current user may read the key"""
    if os.name == 'nt':
        user = os.environ.get('USERNAME') or ''
        if user:
            subprocess.run(['icacls', str(path), '/inheritance:r', '/grant:r', f'{user}:F'], check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    else:
        os.chmod(path, 0o600)


def keygen(path, restrict_fn=restrict):
    path = Path(path)
    if _inside_repo(path):
        raise SystemExit('Refusing: the signing key must live outside the repository')
    path.parent.mkdir(parents=True, exist_ok=True)
    secret = os.urandom(32)
    fd = os.open(str(path), os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)     # never overwrite a key
    with os.fdopen(fd, 'w') as f:
        f.write(secret.hex() + '\n')
    try:
        restrict_fn(path)
        restricted = True
    except Exception:
        restricted = False
    public = ed25519.secret_to_public(secret).hex()
    return public, restricted


def load_key(path):
    path = Path(path)
    if _inside_repo(path):
        raise SystemExit('Refusing: the signing key must live outside the repository')
    text = path.read_text().strip().lower()
    if not re.fullmatch(r'[0-9a-f]{64}', text):
        raise SystemExit('The signing key file is malformed')
    return bytes.fromhex(text)


# --- release assets ------------------------------------------------------------------------------------------------
def _zip_write(z, name, data):
    info = zipfile.ZipInfo(name, ZIP_TIME)
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = 0o644 << 16
    z.writestr(info, data)


def firmware_package(build_dir, version):
    """(zip bytes, application bytes, release build manifest) for a checked public build"""
    build_dir = Path(build_dir)
    app = install_plus.build_application(build_dir)
    build = json.loads((build_dir / 'rfoundry_dm32.json').read_text())
    if not build.get('public_enabled'):
        raise SystemExit('Refusing: not a public build (tools/build_radiofoundry.py --plus --public)')
    if build.get('compiler_warnings') not in (None, 0):
        raise SystemExit('Refusing: the build has compiler warnings')
    full_map = (build_dir / 'rfoundry_dm32.map').read_bytes()
    lines = [m.group(0).strip() for m in
             re.finditer(r'^\s*0x[0-9a-fA-F]+\s+__codec_\w+\s*=.*$', full_map.decode('utf-8', 'replace'), re.M)]
    reduced_map = ('# DDM25 Plus %s: codec layout symbols from the linker map (full map: source build)\n' % version +
                   '\n'.join('                ' + l for l in lines) + '\n').encode()
    release_build = {
        'kind': 'DDM25 Plus release', 'version': version, 'plus_enabled': True, 'public_enabled': True,
        'forced_rebuild_identical': True, 'upstream_commit': build.get('upstream_commit'),
        'outputs': {'rfoundry_dm32.bin': sha256(app), 'rfoundry_dm32.map': sha256(reduced_map)},
        'source_build_manifest_sha256': sha256((build_dir / 'rfoundry_dm32.json').read_bytes()),
        'source_map_sha256': sha256(full_map)}
    release_json = (json.dumps(release_build, indent=2) + '\n').encode()
    from io import BytesIO
    buf = BytesIO()
    with zipfile.ZipFile(buf, 'w') as z:
        _zip_write(z, 'rfoundry_dm32.bin', app)
        _zip_write(z, 'rfoundry_dm32.json', release_json)
        _zip_write(z, 'rfoundry_dm32.map', reduced_map)
    data = buf.getvalue()
    # the release folder must pass the installer's own build checks
    import tempfile
    with tempfile.TemporaryDirectory() as td:
        with zipfile.ZipFile(BytesIO(data)) as z:
            z.extractall(td)
        if install_plus.build_application(td) != app:
            raise SystemExit('Release package does not pass the installer\'s build checks')
    return data, app, release_build


def check_app_zip(path):
    with zipfile.ZipFile(path) as z:
        names = z.namelist()
    if f'{EXE}/{EXE}.exe' not in names or any(not n.startswith(f'{EXE}/') or '..' in n for n in names):
        raise SystemExit(f'The program zip must hold {EXE}/{EXE}.exe and nothing outside {EXE}/')
    # Same rules as pc/bundle.py: a backup folder, a backup manifest, raw images or logs anywhere. The backup tool
    # itself (radiotools/dm32_readonly_backup.py) is public source and belongs in the program.
    private_dirs = {'backups', 'backup', 'yedekler'}
    bad = [n for n in names if Path(n).suffix.lower() in ('.bin', '.log')
           or private_dirs & {p.lower() for p in n.split('/')[:-1]}
           or Path(n).name.lower() == 'backup-manifest.json']
    if bad:
        raise SystemExit('The program zip holds files that must not be published: ' + ', '.join(bad[:5]))
    return Path(path).read_bytes()


def build(args, secret):
    out = Path(args.out)
    if out.exists():
        raise SystemExit(f'Refusing an existing --out: {out}')
    if _inside_repo(out) and not str(out.resolve()).startswith(str((PC / 'dist').resolve())):
        raise SystemExit('Put the release under pc/dist/ or outside the repository')
    sm.parse_version(args.version)
    sm.parse_version(args.min_compatible)
    tag = args.tag or f'v{args.version}'
    fw_zip, app, _ = firmware_package(args.build_dir, args.version)
    fw_name = f'ddm25-plus-{args.version}.zip'
    entry = {'version': args.version, 'channel': args.channel, 'file': fw_name,
             'url': f'{sm.URL_PREFIX}{tag}/{fw_name}', 'sha256': sha256(fw_zip), 'size': len(fw_zip),
             'application_sha256': sha256(app), 'application_size': len(app),
             'settings_format': args.settings_format, 'codeplug_format': args.codeplug_format,
             'min_compatible_version': args.min_compatible,
             'notes': {'tr': args.notes_tr, 'en': args.notes_en}}
    manifest = {'format': sm.FORMAT, 'repo': sm.REPO, 'release': tag,
                'created_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), 'firmware': [entry]}
    assets = {fw_name: fw_zip}
    if args.app_zip:
        if not args.app_version:
            raise SystemExit('--app-zip needs --app-version')
        sm.parse_version(args.app_version)
        data = check_app_zip(args.app_zip)
        app_name = f'{EXE}-{args.app_version}.zip'
        manifest['app'] = {'version': args.app_version, 'file': app_name, 'url': f'{sm.URL_PREFIX}{tag}/{app_name}',
                           'sha256': sha256(data), 'size': len(data),
                           'notes': {'tr': args.app_notes_tr or '', 'en': args.app_notes_en or ''}}
        assets[app_name] = data
    sm.check_manifest(manifest)
    data = (json.dumps(manifest, indent=2, ensure_ascii=False) + '\n').encode('utf-8')
    sig = sm.sign_bytes(data, secret)
    public = ed25519.secret_to_public(secret).hex()
    sm.verify_bytes(data, sig, public)           # the signature verifies before anything is kept
    out.mkdir(parents=True)
    for name, body in assets.items():
        (out / name).write_bytes(body)
    (out / sm.MANIFEST_NAME).write_bytes(data)
    (out / sm.SIG_NAME).write_text(sig)
    return out, public, manifest


def verify(folder, public_hex=None):
    folder = Path(folder)
    m = sm.verify_files(folder, public_hex)
    for e in m['firmware'] + ([m['app']] if m.get('app') else []):
        body = (folder / e['file']).read_bytes()
        if len(body) != e['size'] or sha256(body) != e['sha256']:
            raise SystemExit(f'{e["file"]}: size or SHA-256 differs from the manifest')
    return m


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--keygen', action='store_true')
    ap.add_argument('--key', type=Path, default=DEFAULT_KEY)
    sub = ap.add_subparsers(dest='cmd')
    b = sub.add_parser('build')
    b.add_argument('--version', required=True)
    b.add_argument('--channel', choices=sm.CHANNELS, required=True)
    b.add_argument('--build-dir', type=Path, required=True)
    b.add_argument('--settings-format', type=int, required=True)
    b.add_argument('--codeplug-format', type=int, required=True)
    b.add_argument('--min-compatible', required=True)
    b.add_argument('--notes-tr', required=True)
    b.add_argument('--notes-en', required=True)
    b.add_argument('--app-zip', type=Path)
    b.add_argument('--app-version')
    b.add_argument('--app-notes-tr')
    b.add_argument('--app-notes-en')
    b.add_argument('--tag')
    b.add_argument('--out', type=Path, required=True)
    b.add_argument('--key', dest='build_key', type=Path, default=None)
    v = sub.add_parser('verify')
    v.add_argument('folder', type=Path)
    v.add_argument('--public-key')
    args = ap.parse_args(argv)
    if args.keygen:
        public, restricted = keygen(args.key)
        print(f'Signing key written: {args.key}' + ('' if restricted else '  (WARNING: could not restrict its ACL)'))
        print(f'Public key: {public}')
        print('Paste it into pc/radiotools/signed_manifest.py PUBLIC_KEY_HEX and build the program again.')
        print('Keep the key file private and backed up offline; it is never part of the repository.')
        return 0
    if args.cmd == 'build':
        secret = load_key(args.build_key or args.key)
        out, public, m = build(args, secret)
        print(f'Release files in {out} (key id {sm.key_id(bytes.fromhex(public))}):')
        for p in sorted(out.iterdir()):
            print(f'  {p.name}  {p.stat().st_size} B')
        print(f'Tag: {m["release"]}. Nothing was uploaded. Publish these files on GitHub as the assets of that '
              'release (a separate, approved step).')
        return 0
    if args.cmd == 'verify':
        m = verify(args.folder, args.public_key)
        print(f'OK: {m["release"]}, {len(m["firmware"])} firmware' + (', program' if m.get('app') else ''))
        return 0
    ap.print_help()
    return 2


if __name__ == '__main__':
    sys.exit(main())
