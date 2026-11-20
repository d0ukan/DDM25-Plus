# DDM32 Plus for Windows (C#, .NET 10, WPF)

The Python-free Windows programmer for DDM32 Plus, replacing the PyQt5 program in `pc/` step by step. Same pages,
same wizard (Hazırlık, Yedek, Kontrol, Yükle, Bitti), Turkish and English.

**Stage 2 (this one): the radio tools are ported, radio access is off.** The backup, install, first install from
stock and restore tools of `pc/radiotools` are ported to C# with every guard and are held to the Python tools byte for
byte on 27 recorded runs (see [docs/porting-plan.md](docs/porting-plan.md)). The wizard and the "Geri dön" page run
them, but the serial port is behind `RadioAccess` and **off in every build**: "Yedek al", "Yükle" and "Geri yükle"
stop before any port is opened and say so. The first real run comes with the owner's approval, one step at a time.
Working without a radio: the CH340 cable and driver check (Windows' device list, no port), a verified backup on disk,
its detection and the write plan, the signed release list, download with hash checks, the program's self-update.

| Language dialog | Main window (Türkçe) | Main window (English) |
|---|---|---|
| ![](docs/screenshots/language-dialog.png) | ![](docs/screenshots/main-tr.png) | ![](docs/screenshots/main-en.png) |

| Step | Türkçe | English |
|---|---|---|
| 1 Hazırlık / Preparation | ![](docs/screenshots/wizard-1-prep-tr.png) | ![](docs/screenshots/wizard-1-prep-en.png) |
| 2 Yedek / Backup | ![](docs/screenshots/wizard-2-backup-tr.png) | ![](docs/screenshots/wizard-2-backup-en.png) |
| 3 Kontrol / Check | ![](docs/screenshots/wizard-3-check-tr.png) | ![](docs/screenshots/wizard-3-check-en.png) |
| 4 Yükle / Install | ![](docs/screenshots/wizard-4-install-tr.png) | ![](docs/screenshots/wizard-4-install-en.png) |
| 5 Bitti / Done | ![](docs/screenshots/wizard-5-done-tr.png) | ![](docs/screenshots/wizard-5-done-en.png) |
| Geri dön / Go back | ![](docs/screenshots/restore-tr.png) | ![](docs/screenshots/restore-en.png) |
| Telsizin durumu / Radio health | ![](docs/screenshots/health-tr.png) | ![](docs/screenshots/health-en.png) |

More: [Sürümler](docs/screenshots/versions-tr.png), [Versions](docs/screenshots/versions-en.png). The check step in
the screenshots shows a synthetic backup chosen as "Mevcut yedeği seç..." does.

## Layout

```
win/
  DDM32Plus.sln
  Directory.Build.props          version (one place, also AppInfo.Version), warnings are errors
  src/DDM32Plus.Core/            no UI: Ed25519, signed manifest, versions, release list, downloads, channel
                                 labels, self-update swap, paths, settings
  src/DDM32Plus.Core/Radio/      the radio tools (ports of pc/radiotools), their guards, the install / restore
                                 flows' rules, the CH340 check; Data/ = the pinned RAM monitor image and the DM32
                                 codec tables (RadioFoundry, MIT, LICENSES/RadioFoundry-MIT.txt)
  src/DDM32Plus/                 the WPF program; Resources/Strings.resx (English) + Strings.tr.resx (Turkish)
  tests/DDM32Plus.Core.Tests/    xUnit; Fixtures/python-results.json = results of the Python code (make_fixtures.py),
                                 Fixtures/radio/ = recorded runs of the Python radio tools (make_radio_fixtures.py)
  tools/make_icon.py             the icon (Assets/ddm32plus.ico) from the owner's drawing art/mark-src.png
  installer/DDM32Plus.iss        Inno Setup script (per-user setup)
  docs/                          porting plan, screenshots
```

## Build and test

Needs the .NET 10 SDK (`dotnet --list-sdks` shows 10.0.x). From `win/`:

```
dotnet build DDM32Plus.sln
dotnet test DDM32Plus.sln
```

Both must finish with zero warnings (warnings are errors). The tests use fakes only: no network, no serial port.

The fixture of Python results is made by running the Python code (from the repository root):

```
python win/tests/DDM32Plus.Core.Tests/Fixtures/make_fixtures.py
```

Run it again when `pc/radiotools/signed_manifest.py`, `ed25519.py` or `pc/ddm32_app/releases.py` change; the C# tests
then show any difference.

The radio tools' recordings (on Windows; needs the pinned RadioFoundry tree):

```
set DDM32_UPSTREAM=E:\GIT-REPO\open25-dm32\research\upstream\radiofoundry
python win/tests/DDM32Plus.Core.Tests/Fixtures/make_radio_fixtures.py
```

It also writes `src/DDM32Plus.Core/Radio/Data/` (the RAM monitor image and the codec tables). Run it again when a
radio tool in `pc/radiotools` changes; the replay tests then show where the C# port differs.

## Radio access (developer flag)

`RadioAccess.Enabled` is a compile-time constant, false in every normal build. Only

```
dotnet build -p:RadioAccess=true
```

compiles the serial port in. Never publish or install such a build without the owner's approval for that specific
step (AGENTS.md hard safety rules); the tests check that the normal build refuses to open a port.

`DDM32Plus.exe --show PAGE[:STEP[:BACKUP]]` opens a page (a wizard step, and a verified backup as "Mevcut yedeği
seç..." takes it) for screenshots and demos; it only navigates and reads files.

## Publish (one exe, nothing to install)

```
dotnet publish src/DDM32Plus/DDM32Plus.csproj -p:PublishProfile=win-x64
```

Output: `win/publish/win-x64/DDM32Plus.exe` (about 60 MB): self-contained (the .NET runtime is inside), single file,
compressed, x64. It runs on a stock Windows 10 (1809 or newer) or 11 with nothing installed.

## Setup (Inno Setup)

Install [Inno Setup](https://jrsoftware.org/isinfo.php) 6.3 or newer (7.1 builds it; checked 2026-11-20), publish as above, then:

```
"C:\Program Files\Inno Setup 7\ISCC.exe" installer\DDM32Plus.iss
```

(Inno Setup 6: `"C:\Program Files (x86)\Inno Setup 6\ISCC.exe"`, or under `%LOCALAPPDATA%\Programs` for a per-user Inno install). Output:
`win/publish/installer/DDM32Plus-Setup.exe`.

The setup installs **per user** to `%LOCALAPPDATA%\Programs\DDM32 Plus` without an administrator prompt, adds a Start
menu and a desktop shortcut, and appears in Settings > Apps. Uninstalling removes the program, its settings and
downloads but **keeps `yedekler`** (the radio's backups) and tells the user where it is.

## Where the program keeps things

Everything in its own install folder:

| Path | What |
|---|---|
| `DDM32Plus.exe` | the program |
| `settings.json` | language, disclaimer acceptance, update check, window place |
| `yedekler\` | the radio's backups and logs (private: calibration, channels). Updates and uninstall never touch it |
| `surumler\` | downloaded firmware releases and the verified manifest cache |
| `guncelleme\` | program update staging, `rollback\` (the previous program files), `failed\` |

`DDM32_DATA=<folder>` moves all of these elsewhere (tests, screenshots).

The Python program kept its data in `%LOCALAPPDATA%\DDM32Plus` (before the rename `DDM25Plus`). This program finds
those folders and shows where their backups are (home and "Geri dön" pages); it never moves or deletes them.

## Releases and signatures

- The release list comes from `https://api.github.com/repos/d0ukan/DDM32-Plus/releases` (HTTPS to GitHub hosts only;
  every redirect is checked again). TLS is Windows' own, so no extra certificate bundle is needed.
- A release is listed only when its `ddm32-manifest.json` verifies with the embedded Ed25519 key (key id
  `dda8c4a85c4600df`) and passes the same schema checks as `pc/radiotools/signed_manifest.py`. A firmware zip is used
  only when its size and SHA-256 equal the signed ones and the application inside equals `application_sha256`.
- The first release, `v1.0.0-beta.1`, still uses the old names (`ddm25-manifest.json`, format `ddm25-manifest/1`,
  repo `d0ukan/DDM25-Plus`). Its signature is verified and it is **listed as "Legacy (DDM25)"**, but this program
  does not install it.
- Labels: **Latest** = the newest stable, **Stable** = an older stable, **Beta**, **Test / Alpha** = a beta whose
  version is `-alpha…`, `-test…` or `-dev…` (asks "are you sure?" before use).
- All constants (format, repo, manifest name, key) are in `src/DDM32Plus.Core/Releases/ReleaseConstants.cs`.

### Program updates

The manifest's `app` entry is the Python program's zip. This program updates itself only from an **`app_net`**
entry (same fields as `app`: version, file, url, sha256, size, notes), so neither program can replace itself with the
other. The release tool writes it: `pc/release/make_release.py build ... --app-net-zip ZIP --app-net-version V`
(asset `DDM32Plus-win-V.zip`); the zip must hold `DDM32Plus/DDM32Plus.exe` and nothing named like the user's data
(`yedekler`, `settings.json`, `surumler`, `guncelleme`, the uninstaller). `pc/radiotools/signed_manifest.py` checks
the entry like `app`; programs from before ignore the key, so adding it breaks no published checker.

Update flow: check (on start unless switched off, or the button) → download → SHA-256 and size against the signed
manifest → safe unzip into `guncelleme\app-VERSION\` → the new exe runs as the helper (`--self-update`), waits for the
old program to exit, moves only the files it replaces to `guncelleme\rollback\`, copies the new ones in and starts
the new program; if it does not report back within 90 s, the old files are put back and the old program starts.
`yedekler` and `settings.json` are never touched.

## SmartScreen

`DDM32Plus.exe` and the setup are not code-signed yet, so Windows SmartScreen warns on the first run ("Windows
protected your PC"). Click **More info**, then **Run anyway** (Türkçe: **Ek bilgi**, sonra **Yine de çalıştır**).
Screenshots of these two clicks will be added here; the warning goes away once a code-signing certificate is used.
