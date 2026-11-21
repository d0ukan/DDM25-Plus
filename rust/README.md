# DDM32 Plus programmer (Rust)

The program that installs, updates and backs up the DDM32 Plus firmware on a Baofeng DM-32UV, for **Windows, Linux
and macOS**. It replaces the C# / .NET program in `win/` (owner's decision 2026-11-21) and is held byte for byte to it
and to the Python tools in `pc/radiotools`: the same bytes to the radio, the same receipts and backup files, the same
refusals.

Licence: GPL-3.0-or-later (dependencies: [THIRD_PARTY.md](THIRD_PARTY.md)).

## Structure

```
rust/
  Cargo.toml                 workspace (version 3.0.0-alpha.1)
  crates/ddm32-core/         everything that is not the window
    src/crypto/ed25519.rs        Ed25519 verify (a port of the RFC 8032 reference code, as the C# and Python ones)
    src/releases/                signed manifest checks, version order and labels, release list, downloads, local folder
    src/update/self_update.rs    the self-update swap and rollback (the C# program's protocol, for the hand-over)
    src/storage/                 data folder per OS, settings.json, backups (fixed yedekler, automatic discovery)
    src/radio/                   RAM monitor host, guards (ReadOnlyPort, InstallPort, CpsPackPort), tools
                                 (backup, install over Plus / from stock / restore, quick update, no-backup install,
                                 restore stock, data pack writer), codec slot writer, detect, flows, progress, cable
    src/datapacks/               data pack format, RadioID CSV, AD 2.18 text / PDF, building packs from files
    data/                        the pinned RadioFoundry RAM monitor and DM-32 codec tables (MIT; SHA-256 checked)
    tests/                       acceptance tests on the C# program's fixtures (win/tests/DDM32Plus.Core.Tests/Fixtures)
  crates/ddm32-cli/          `ddm32`: a small command line (tests, the owner's first real runs)
  crates/ddm32-gui/          the window (Slint): ui/*.slint, src/lib.rs (controller), src/i18n.rs (tr / en)
  docs/plan.md               stage 2 (GUI) design and stage 3 (packaging) plan
  docs/screenshots/          every screen in the light and the dark theme (written by crates/ddm32-gui/tests/screens.rs)
  packaging/windows/         Inno Setup script (per-user install over the C# program, same AppId and folder)
```

## Radio access

A serial port exists only in a build with the **`radio` feature** (off by default). Without it no code path can open a
port (`RadioAccess::factory` refuses, and the serial code is not compiled in); the tests build without it. Every radio
step starts from its own button or command, the guards sit between every tool and the port, and a failed write is never
retried. See AGENTS.md rules 1-4.

## Where the program keeps things

| OS | data folder |
|---|---|
| Windows | the program's own install folder (`%LOCALAPPDATA%\Programs\DDM32 Plus`, as the C# program) |
| Linux | `$XDG_DATA_HOME/DDM32Plus`, else `~/.local/share/DDM32Plus` |
| macOS | `~/Library/Application Support/DDM32Plus` |

Inside it: `yedekler/` (every backup and every install log: the user never chooses another folder; updates and the
uninstaller never delete it), `surumler/` (downloaded releases), `guncelleme/` (program updates), `paketler/` (data
packs), `settings.json`. On Windows the older programs' backup folders (`%LOCALAPPDATA%\DDM32Plus`, `DDM25Plus`, and the
C# install folder when this program lives elsewhere) are found and read, never changed. `DDM32_DATA` overrides the
folder (tests, screenshots).

## Build and test

Rust 1.88 or newer (tested with 1.100.0).

```sh
cd rust
cargo test                                   # all tests (no serial port, no network)
cargo clippy --all-targets -- -D warnings
cargo fmt --all --check
cargo build --release -p ddm32-cli -p ddm32-gui                                       # no radio access
cargo build --release -p ddm32-cli -p ddm32-gui --features ddm32-cli/radio,ddm32-gui/radio   # the installed program
```

Linux: the window's font lookup links fontconfig, so a build needs `pkg-config` and `libfontconfig-dev`
(`sudo apt install pkg-config libfontconfig-dev`); the window uses Wayland or X11 through winit (loaded at run time). The tests read the C# program's fixtures from `../win/tests/DDM32Plus.Core.Tests/Fixtures`.

Command line: `ddm32 version | paths | backups | detect --backup DIR | plan ... | releases | download VERSION |
pack radioid|air|info ... | tool backup|install_plus|restore_stock|fast_install|datapack ARGS` (`ddm32 --help`).
The window: `ddm32-gui` (`--demo` shows example contents without radio or network).
