# DDM32 Plus programmer — developer and maintainer notes

The Windows program that installs, updates, downgrades, backs up and restores DDM32 Plus on a Baofeng DM-32UV. User
guide (Turkish): [README.md](README.md). Licence: GPL-3.0-or-later (repository `LICENSING.md`).

## Layout

```
pc/
  ddm32_app/      PyQt5 app (Turkish UI). No module here talks to the radio.
  radiotools/     the guarded radio tools, each run as its own process (one user-approved step per run)
  release/        make_release.py: signing and release files (never uploads)
  tests/          unit tests on fakes (no port, no tool process, no network)
  docs/           stock-install.md: first install from stock, design and evidence
  bundle.py, build_exe.ps1, run_ddm32.py, fetch_upstream.py
```

- `radiotools/`: `dm32_readonly_backup.py` (two-pass backup, read-only transport), `install_plus.py` (install over
  the Classic layout, `--from-stock`, `--restore`), `install_firstboot.py` (the `InstallPort` write guard and its
  helpers; command line: stock restore only), `prepare_unit_image.py` (saved backup checks, codec slot composition),
  `radio_detect.py` (what the radio runs, from a backup), `plus_txdiag.py` / `plus_live.py` (read-only debug link),
  `signed_manifest.py` + `ed25519.py` (release gate), `upstream.py` (pinned RadioFoundry files). They are copies of
  the lab tools with owner-specific values removed (COM port, unit `.047` facts, backup paths, the lab's reviewed
  hashes and first-boot entries); every guard is kept. Two changes in substance, both documented in
  `docs/stock-install.md`: the attached flash chips must equal the IDs the radio's own backup recorded (instead of the
  lab unit's), and the release gate below replaces `PLUS_REVIEWED`.
- `ddm32_app/`: `main_window.py`, pages `home_tab`, `update_tab` (backup → detect → plan → install), `versions_tab`,
  `restore_tab`, `health_tab`, `live_tab`, `codeplug_tab` (RFKV read-only decoder `codeplug.py`), `data_tab`
  (developer only); logic without Qt: `wizard.py`, `parsers.py`, `releases.py`, `selfupdate.py`, `ports.py`,
  `paths.py`, `disclaimer.py`.

## Running from source

```powershell
python pc\fetch_upstream.py        # RadioFoundry dfbaf20c into research\upstream\radiofoundry, hashes checked
cd pc
python -m ddm32_app
```

Needs Python 3.10, PyQt5, pyserial, and certifi (optional; the program bundles its Mozilla root list, MPL-2.0, beside
the Windows certificate store so that GitHub downloads work on PCs whose store lacks a root, see
`ddm32_app/tls.py`). `DDM32_UPSTREAM` may point at another RadioFoundry checkout of the pin (every
pinned file is still hash-checked). User data: `%LOCALAPPDATA%\DDM32Plus` (`DDM32_DATA` overrides it).

The **Verileri güncelle** page (RadioID / repeater lists, TLE into the WSL build copy `~/ddm32-plus`, public WSL build
`tools/build_radiofoundry.py --plus --public`) appears only from a source checkout with WSL; the frozen program hides
it. A build made there is installable only after it is published as a signed release.

## Tests

```powershell
cd pc
$env:QT_QPA_PLATFORM = 'offscreen'
python -m unittest discover -s tests
```

All on fakes: the pinned monitor's `FakeRadio` behind the real install guards (simulated install over Classic, from
stock, restores, corrupted read-backs, wrong chips), a fake process runner for the UI, fake port lists, a fake
network, releases signed with a test key in `tests/rtfixtures.py` (never a release key). Tests that need the pinned
RadioFoundry files skip when they are not found.

## Building the program

```powershell
powershell -ExecutionPolicy Bypass -File pc\build_exe.ps1 -Paketle
```

PyInstaller one-folder build `pc\dist\DDM32Plus\` (the radio tools and the pinned RadioFoundry files beside the .exe,
hash-checked at build and run time), then `DDM32Plus.exe --run-tool _selfcheck`, then the private-data scan and the
zip `pc\dist\DDM32Plus-<version>.zip` (top folder `DDM32Plus/`, as the self-update expects). The scan refuses
backups, `.bin`/`.map`/`.csv`/`.log`, keys, signatures, RadioID/TRAC lists, stock firmware and unexpected JSON.

## Release signing

- **Format**: each GitHub release carries `ddm32-manifest.json` (format `ddm32-manifest/1`, see
  `radiotools/signed_manifest.py`) and `ddm32-manifest.json.sig`: the Ed25519 signature (RFC 8032) of the manifest's
  exact bytes, as 128 hex digits. Per firmware: version, channel (beta/stable), zip file name, URL, SHA-256, size,
  `application_sha256` and size of `rfoundry_dm32.bin`, `settings_format`, `codeplug_format`,
  `min_compatible_version`, notes TR/EN. Optional `app`: program version, zip, URL, SHA-256, size.
- **Crypto**: verification is the RFC 8032 reference code in plain Python (`radiotools/ed25519.py`, ~150 lines,
  tested against the RFC's section 7.1 vectors). No crypto package is installed or bundled.
- **Key**: `python pc\release\make_release.py --keygen` creates the private key at
  `%USERPROFILE%\.ddm32\release-signing-key` (refuses a path inside the repository and never overwrites; ACL limited
  to the current user with `icacls`) and prints the public key. **Paste the public key into
  `pc/radiotools/signed_manifest.py`, `PUBLIC_KEY_HEX = '...'`**, run the tests, build the program. Until then
  `PUBLIC_KEY_HEX` is empty and the program verifies nothing, so it lists and installs nothing.
- **Release**: build the public firmware (`tools/build_radiofoundry.py --plus --public --stack-check
  --verify-rebuild`), build the program zip, then

  ```powershell
  python pc\release\make_release.py build --version 1.0.0-beta.1 --channel beta `
      --build-dir build\radiofoundry-p25\build\dm32-p25-plus-public `
      --settings-format 1 --codeplug-format 1 --min-compatible 1.0.0-beta.1 `
      --notes-tr "..." --notes-en "..." `
      --app-zip pc\dist\DDM32Plus-1.0.0-beta.1.zip --app-version 1.0.0-beta.1 --out pc\dist\release-1.0.0-beta.1
  python pc\release\make_release.py verify pc\dist\release-1.0.0-beta.1 --public-key <hex>
  ```

  The tool checks the build like the installer (reproducible Plus build, hashes, codec layout, size) and requires
  `public_enabled`; the zip holds the application, a reduced map (the `__codec_*` lines the installer checks) and a
  reduced build manifest without build-machine paths. It signs, verifies the signature, and writes the files to
  `--out`. **It uploads nothing**: create the GitHub release with tag `v<version>` and attach every file from
  `--out` as a separate, approved step.
- **Other programs in the same release** (optional): `--app-net-zip ZIP --app-net-version V` (the C# Windows program,
  `app_net`) and, once per platform, `--app-rs-zip PLATFORM=ZIP` with `--app-rs-version V` (the Rust program,
  `app_rs`; PLATFORM `windows-x86_64`, `linux-x86_64`, `macos-aarch64`, ...; zips from
  `rust/packaging/make_app_zip.py`, published as `DDM32Plus-V-PLATFORM.zip`). Each program updates itself only from
  its own entry. `rust/target/.../ddm32 verify-manifest DIR --public-key <hex>` checks the release as the Rust
  program reads it.
- Raise `settings_format` / `codeplug_format` when the stored settings / channel layout changes, and set
  `min_compatible_version` to the oldest version that reads what the new one saves; the program warns, and asks
  twice, for a downgrade across these.

## The public firmware gate (what `PLUS_REVIEWED` is in the lab)

`install_plus.py` installs a DDM32 Plus application only when (`signed_manifest.release_gate`):

1. the release folder holds `ddm32-manifest.json` and `ddm32-manifest.json.sig`,
2. the signature verifies with `PUBLIC_KEY_HEX` embedded in `radiotools/signed_manifest.py` (checked before the
   JSON is parsed), and the manifest passes the schema check,
3. a firmware entry's `application_sha256` and `application_size` equal the raw `rfoundry_dm32.bin`, and
4. the build checks pass as before (reproducible Plus build manifest, bin and map hashes, codec layout symbols,
   application ends below `0xE8000`).

The tool does this itself on every run (plan and install); the app's own check only decides which buttons are
enabled. The app additionally refuses a firmware zip whose size or SHA-256 differs from the signed manifest, and an
application inside it that differs from `application_sha256`. The lab gate in the owner's lab repository
(`tools/install_plus.py`, `PLUS_REVIEWED`) is unchanged and separate.

## Update check and self-update

- The app reads `https://api.github.com/repos/d0ukan/DDM32-Plus/releases` (one call), then each release's manifest
  and signature (GitHub over HTTPS only, redirects checked). Verified manifests are cached under
  `%LOCALAPPDATA%\DDM32Plus\surumler\manifests\<tag>\` and verified again whenever they are read. The check runs at
  start (switchable off) or on a click; it never installs anything.
- Self-update (`ddm32_app/selfupdate.py`, frozen program only): the new program zip from a signed manifest is
  downloaded and checked (size, SHA-256), extracted safely into `guncelleme\app-<version>\`, and the new .exe runs
  as `DDM32Plus.exe --self-update ...`: it waits for the old program to exit, renames the program folder to
  `<folder>.old`, copies the new one into place and starts it with `--update-check <marker>`. No marker within 90 s:
  the new one is stopped and moved to `<folder>.failed`, the old folder comes back and starts. The result is shown
  once at the next start.

## Safety model (unchanged from the lab)

The app never opens a serial port: listing uses `serial.tools.list_ports`; every radio step is a subprocess of a
guarded tool started from its own button, and every install or restore needs a confirmation that repeats the
disclaimer's short warning. Before the disclaimer is accepted the radio lock refuses every radio step. Only the
backup-based detection and the plan (no port) follow a verified backup by themselves. A failed write is never
retried. The bootloader (`< 0xC000`) and the data chip are never written.
