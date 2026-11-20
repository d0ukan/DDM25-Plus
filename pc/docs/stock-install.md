# First install from stock firmware

How the programmer puts DDM32 Plus on a DM-32UV that still runs Baofeng's stock firmware, and why each step is safe.
Implemented in `pc/radiotools/install_plus.py --from-stock` and `pc/radiotools/prepare_unit_image.py`; tested on the
pinned monitor's simulated radio (`pc/tests/test_install_plus.py`, `FromStockTests`). **Not yet run on a real radio
through this program** (see "Evidence" for what has run on hardware).

## The problem

DDM32 Plus uses the Classic flash layout: the application from `0xC000` to `0xE8000` and the radio's own DMR (AMBE)
codec in a reserved slot `0xE8000..0x100000` (a 128-byte header, the code, the tables and a relocation list; format
v2, `codec_slot_t`). The codec belongs to DVSI / Baofeng and is never shipped. On a stock radio the slot is empty and
the codec sits inside the stock application, at an address that depends on the stock version. `install_plus.py`'s
normal mode only writes below the slot and refuses a radio without a prepared slot, so a stock radio needs one more
step: prepare the slot from the radio's own flash.

## The path

1. **Backup** (`dm32_readonly_backup.py`, radio step 1): both chips read twice through the pinned RAM monitor, both
   passes identical, per-sector hashes in `backup-manifest.json` with the chips' JEDEC IDs. No write command exists
   in that transport.
2. **Detect** (`radio_detect.py`, no port): no valid slot at `0xE8000`, and the pinned RadioFoundry codec tool
   (`tools/hrc7000_codec.py` at `dfbaf20c`) finds the codec by content where a known stock build keeps it
   (`identify` / `locate`: code signature words, code SHA-256, table head CRC and table SHA-256). Known DM-32UV
   profiles: .032, .037, .039, .040, .046 (three builds), .047, L01.048, .049 (three builds). .047 is the one DDM32
   Plus has been installed over; the others get a warning. Anything else (for example NRFB .050, which the pinned
   tool does not know) is refused: install Classic with the upstream Flasher first, then DDM32 Plus over it.
3. **Compose** (`prepare_unit_image.compose_from_stock`, in memory, no port): exactly what the lab's
   `tools/prepare_unit_image.py` did for its unit images — `codec.extract(program, board='dm32')` then
   `codec.merge(application, profile, code, tables)` — with the same checks: the slot parses back
   (`codec.parse_slot`) with the stock tag, the code and table SHA-256 of the profile; `install_plus.slot_check`
   (header, version, CRC over code + tables + relocations) returns the same tag. Before loading the codec tool every
   file it reads (the tool, the board registry, the codec maps) must match its SHA-256 pin, and the board's windows
   must equal the Classic / Plus linker symbols (`__codec_slot`, `__codec_run_lo/hi`, table window). The target image
   is `boot area (unchanged) + application + 0xFF + slot + 0xFF`.
4. **Plan** (`install_plus.py --from-stock --plan-only`, no port): changed sectors, all `>= 0xC000`; boot area
   unchanged (refused otherwise, `make_plan`); `codec_slot_prepared_from_backup: true`; `data_chip_written: false`.
   The app accepts the plan only in this exact shape (`parsers.plan_safe(plan, 'install_plus_from_stock')`).
5. **Install** (radio step 2, its own click and confirmation): the unchanged `install_firstboot.InstallPort` guard
   (only the planned erase/program frames, in order, once, never below `0xC000`, never the data chip), after the
   live checks: attached chip IDs equal the backup's, the whole program flash equals the backup, every data-chip
   sector CRC equals the backup's. Each sector is read back after writing; no automatic retry.
6. **Verify** (same run): full program read-back must equal the target, the slot must read back exactly as prepared
   and pass `slot_check` with the stock tag, the data chip CRCs must be unchanged; only then is the restart allowed.

The release gate is the same as for every install: the application must be listed in a manifest signed with the
embedded key (`signed_manifest.release_gate`).

## Evidence

- **Same writer, same region, on hardware.** The lab installed its first-boot images on the author's .047 radio from
  stock with `install_firstboot.py` (2026-11-13/14: r1 and r2 from stock, r2 again after a stock restore, then r3
  and r4 as updates): the write plan covered the
  application and the slot area from `0xC000`, through the same `InstallPort` guard, with full read-back. Each
  image held the radio's own codec merged by `hrc7000_codec.merge` (`prepare_unit_image.py`), the same function used
  here. The boot area and the data chip read back unchanged every time.
- **Same restore, on hardware.** Stock restore from the saved backup (`--restore-stock`, all 244 application sectors)
  was proven on that radio (byte-exact read-back, normal stock boot), so a failed or unwanted install can be undone
  from the user's own backup ("Stok yazılıma dön").
- **Same slot format as the firmware reads.** DDM32 Plus is built from RadioFoundry `dfbaf20c`, the same pin as the
  codec tool; its slot reader (`platform/hrc7000/codec_slot.h`, Classic's `tests/codec/test_slot.c`) checks the same
  header, version 2, the relocation list and the CRC. Classic only widened the table window end to `0x18003E38` (for
  NRFB .050); every profile the pinned tool accepts ends at or below `0x18003E28`.
- **Upstream does the same.** Classic's own Flasher prepares the radio's codec into the blank slot on first install
  from recognised stock layouts and keeps the bootloader and the data flash untouched ("Installing Classic" in
  `docs/UPSTREAM-README.md`); the author installed Classic over stock .047 that way.

## What is different from the lab, and why it is not weaker

| Lab | Public | Why |
|---|---|---|
| `PLUS_REVIEWED` (hashes in the tool) | signed manifest lists the application | same idea, publishable; checked inside the tool |
| JEDEC IDs fixed to the lab unit | equal to the IDs this radio's backup recorded | binds the run to the backed-up radio |
| `prepare_unit_image` writes a candidate file under `artifacts/` | composed in memory, never saved | the codec bytes never leave the install run |
| git pin check of the upstream tree | SHA-256 of every file loaded | works in the frozen program, same strength |

## Known limits

- First boot of DDM32 Plus on a stock data chip: like Classic (r5+), the firmware asks "Start new" before it writes
  its own settings over the stock codeplug area. The installer never writes the data chip; the backup holds it.
- "Stok yazılıma dön" restores program flash only. Channels saved under DDM32 Plus stay on the data chip (the tools
  never write it); the stock firmware may show them differently. Use the stock CPS to rewrite them if needed.
- A backup is bound to its radio by the chip IDs only; the program keeps its own backups per radio in
  `%LOCALAPPDATA%\DDM32Plus\yedekler` and tells the user to restore only that radio's backup.
