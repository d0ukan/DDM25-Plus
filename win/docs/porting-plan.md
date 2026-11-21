# Porting the radio tools to C#

This page says how each radio tool of the Python program (`pc/radiotools/`) is ported, which guards it must keep, and
how the C# port is proven equal to the Python tool **before** it is ever pointed at a radio.

## Status (2.0.0-beta.1, 2026-11-21)

| Tool | C# (`src/DDM32Plus.Core/Radio/`) | Verified against Python |
|---|---|---|
| radio_detect, prepare_unit_image, the DM-32UV codec slot writer | `Detect`, `UnitImage`, `Codec` | layer 1: 10 programs + the composed first-install image, same JSON / same SHA-256 |
| dm32_readonly_backup (+ the pinned monitor host, `ReadOnlyPort`) | `BackupTool`, `MonitorRadio`, `Guards` | layer 2: 6 recorded runs (ok, radio not switched on, ROM NAK, wrong chip, damaged frames, refused argument) |
| install_plus over Classic, from stock, restore (`InstallPort`) | `InstallTool`, `InstallPlan` | layer 2: 17 recorded runs (ok, plans, changed radio or data chip, corrupted readback, slot changed, wrong chip, refusals) |
| install_firstboot --restore-stock | `RestoreStockTool` | layer 2: 4 recorded runs |
| the guards' frame rules | `ReadOnlyPort`, `InstallPort` | layer 3: 89 frames + 5 step sequences, same verdicts and messages |
| quick update and install without backup (C# only, owner's decision 2026-11-20) | `FastInstallTool` | the C# FakeRadio (`FakeRadio.cs`), itself checked to draw the Python tools' exact bytes on 11 recorded runs; 17 tests of the new paths and guard rules |
| data packs (tools/datapack.py; the CPS write of docs/plus.md "User data packs") | `DataPacks/`, `CpsPack.cs` | the builder byte for byte on synthetic inputs (`make_datapack_fixtures.py`, `DataPackTests.cs`); the CPS write on a fake radio as `cps_openfw.c` behaves (`CpsPackTests.cs`); not yet run on the radio |
| health / live panel (plus_live) | not yet | |

Layer 2 means: `tests/DDM32Plus.Core.Tests/Fixtures/make_radio_fixtures.py` runs the Python tool against the pinned
monitor's FakeRadio under a virtual clock and records every port event; the C# tool replays it and must write the same
bytes in the same order, print the same lines, raise the same error and write the same files (receipts and backup
manifests byte for byte, Windows line endings as Python writes them; only `write_seconds` is left out). 27 runs, all
equal. The recording is made on Windows (Python's text files have CRLF there).

The serial port is behind `RadioAccess` (`Ports.cs`). Since 2.0.0-beta.1 (owner's decision 2026-11-21: installs
are done with this program) the release build has radio access and writes on; `-p:RadioAccess=false` builds a program
that opens no port. The developer build (`-p:Developer=true`) made the first real backup and two real installs.

## Rules that do not change

- Every radio step (open a port, read flash, write flash, restart) needs the owner's explicit approval in chat for
  that specific step. An approval never carries over to the next step. The program itself starts a radio step only
  from its own button (one click = one step) and never retries a failed write.
- Never write program flash below `0xC000` (bootloader) and never write the 16 MiB data chip (calibration, channels,
  settings) with the flash tools. The one exception is the data pack region 0xE70000-0xEFFFFF (owner, 2026-11-20),
  written only through the running firmware's CPS protocol behind `CpsPackPort`. Writes are bounded to the planned application sectors (and the codec slot `0xE8000..0x100000` for a
  first install from stock).
- Before any write: a verified two-pass backup of the radio as it is now, and a plan that lists the changed sectors.
  Before the write the live program flash must equal the backup and every data chip sector CRC must equal the
  backup's. After the write: full readback; the restart is allowed only when the readback matches.
- Only releases listed in a manifest signed with the embedded key (`ReleaseConstants.PublicKeyHex`, key id
  `dda8c4a85c4600df`) can be installed; the gate is checked again by the installer itself, every run.
- Never commit, upload or put in a fixture: real backups, anything derived from them, stock firmware, the radio's own
  codec bytes, the release signing key. All fixtures below are synthetic (the same generators as `pc/tests/`).

## Order

1. **detect** (`radio_detect.py`): files only, no port. First, because it is pure.
2. **backup** (`dm32_readonly_backup.py`): the read-only serial transport and the RAM monitor.
3. **install** (`install_plus.py` with `install_firstboot.InstallPort`, `prepare_unit_image.py`): plan, then writes.
4. **restore** (`install_plus.py --restore`, `install_firstboot.py --restore-stock --use-saved-backup`).

The health and live panels (`plus_live.py`) come after these, with the same transport and their own read-only
allowlist.

## How a port is verified

Three layers, all on recorded fixtures, in `win/tests` (xUnit), before any real radio use:

1. **Pure functions, same answers.** A Python script next to the fixtures (like
   `tests/DDM32Plus.Core.Tests/Fixtures/make_fixtures.py` for the release code) runs the Python function on
   synthetic inputs and writes the inputs and results to JSON. The C# test feeds the same inputs and must give the
   same result: equal JSON for detect and plans, equal bytes (SHA-256 and full compare) for composed images.
2. **Wire transcripts, byte for byte.** The Python tool runs against the pinned RAM monitor's `FakeRadio`
   (`pc/tests/rtfixtures.fake_radio`, which already sits behind the real guards) through a recording serial port.
   The recording holds every byte written and every byte read, in order, plus the tool's printed JSON and the files
   it wrote. The C# tool replays the same read bytes and must write **exactly** the same bytes in the same order,
   print the same result and write the same files. One fixture per path: success, radio not switched on in time,
   wrong chip ID, backup that no longer matches the radio, corrupted readback (`corrupt_after_writes`), short write.
3. **Guards refuse the same things.** Every frame the Python guard (`ReadOnlyPort.validate`,
   `InstallPort.validate`) refuses is fed to the C# guard, which must refuse it too (a table of frames generated
   from the Python guard: unknown opcodes, reads outside the chip, writes below `0xC000`, writes to the data chip,
   pages out of plan order, a second restart, reads of unwritten sectors in saved-backup mode, ...). The C# guard
   sits between the tool and the port, so a bug in the tool cannot send a refused frame.

Only when all three pass does a port go to the owner, who decides whether and when a real radio step is tried, one
approved step at a time (backup first, then install), following `docs/plus-radio-test.md`.

## The tools

### detect: `radio_detect.py --backup FOLDER` (no port)

- Input: a verified backup (`backup-manifest.json`, `progmem-pass1.bin`). Output: one JSON object (`layout`,
  `firmware`, `codec_tag`, `stock_profile`, `stock_version`, `stock_tested`, `install_mode`, `supported`, `reason`).
- Needs the codec slot parser (`install_plus.slot_check`) and the stock codec profiles of the pinned RadioFoundry
  codec tool (`tools/hrc7000_codec.py`, `tools/rfoundry/rfoundry/*`, pinned by SHA-256 in `upstream.py`).
- C#: `DDM32Plus.Core/Radio/Detect.cs`; the stock profiles as a data table generated from the pinned RadioFoundry
  files (MIT) by a script, with the source hashes recorded beside it. Verified with layer 1 on the synthetic
  programs of `rtfixtures` (`make_program`, `stock_program` with `synthetic_codec`, unknown layouts).

### backup: `dm32_readonly_backup.py --port COMx --out NEWDIR --installed-firmware TEXT`

- Talks to the radio's boot ROM, downloads the **exact pinned RAM monitor image** (SHA-256 checked), starts it,
  then reads both flash chips twice and compares the passes. Writes `progmem-pass1/2.bin`, `q128-pass1/2.bin` and
  `backup-manifest.json` (`backup_verified`, per-file and per-4 KiB-sector SHA-256, chip JEDEC IDs).
- Guard (`ReadOnlyPort`): only the ROM greeting, the exact RAM image blocks and its start, HELLO, timer read, the
  DM32 configuration frame, JEDEC reads, bounded flash reads and sector CRC/reads inside the identified capacity;
  115200 baud fixed. No write, erase, reboot, GPIO, baud change or RF command exists in it.
- C#: `System.IO.Ports` with the same timeouts; the RAM image is embedded as a resource only after its SHA-256 equals
  the pinned value (it is RadioFoundry's MIT code; the source and hash are recorded in `THIRD_PARTY.md`). Verified
  with layers 2 and 3; the output folder must be byte-identical to the Python tool's for the same transcript.
- The port picker (`ports.py`: CH340 first, never a virtual port) and the CH340 driver check (Device Manager / SetupAPI:
  a USB device `VID_1A86&PID_7523` without a working driver) are ported with it; listing ports does not open one.

### install: `install_plus.py --port COMx --backup B --plus RELEASE --out NEWDIR [--from-stock] [--plan-only]`

- Inputs: the backup of the radio as it is now and a downloaded release folder whose signed manifest lists the
  application (`ReleaseGate`, already ported and tested in `DDM32Plus.Core`).
- `--plan-only` (no port): the sector plan and its JSON; the program shows it before asking for the install click.
- Over Classic / DDM32 Plus: only application sectors below the slot are written; boot area, codec slot and the data
  chip must read back unchanged. From stock: the radio's own codec is taken from **this** backup and composed into
  the slot (`prepare_unit_image.compose_from_stock`, the pinned codec writer); the composed image exists only in
  memory and is never saved.
- Guard (`InstallPort`): only the changed sectors at `0xC000` and up, their exact planned page bytes, in order, once;
  writes enabled only after the live pre-write comparison; restart registers only after the readback matched.
- C#: plan and compose with layer 1 (plan JSON and composed image equal to Python's on synthetic backups and
  releases); the transport with layers 2 and 3, including the failure transcripts (no write started; write started,
  then stopped: the program locks the install and points to "Geri dön").

### restore

- `install_plus.py --restore`: every application and slot sector back from a Classic-layout backup.
- `install_firstboot.py --restore-stock --use-saved-backup`: every application sector back from the user's own
  stock backup (the restore proven on hardware: byte-exact readback, normal stock boot). In saved-backup mode the
  guard refuses all reads of the original firmware and data and allows reading back only sectors already written.
- C#: same three layers; the restore uses the same `InstallPort` port as the install.

## What the owner approves, step by step

1. The C# backup tool on the real radio (read only), compared with a backup made by the Python tool the same day:
   all four images must be byte-identical.
2. The C# install `--plan-only` on that backup (no port) compared with the Python plan.
3. Only then, a real install with the C# tool, as its own approved step, with the Python restore ready.
