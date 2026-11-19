<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Refreshing the satellite elements without a hand-made build (design note)

Status: a note, no code. Written 2026-11-19 with the Lone worker alarm action job.

## What we have

- Tools > Satellites (`CONFIG_SATELLITES`, docs/plus.md) reads `sat_table[]` (`core/radio/sat_data.c`), generated
  on every build by `tools/sat_tle.py` from `integration/radiofoundry/sat/tle.txt` (three-line sets, checksums
  verified) and `sat_freq.txt`. The table is `const`, in program flash: 92 B a satellite (`sat_info_t`: name,
  catalog number, frequencies, tones, eight doubles of elements), 368 B for the four shipped, 736 B for the
  maximum of eight (`SAT_MAX`), plus a note string of about 90 B. The raw text is about 165 B a satellite.
- Elements of the low FM satellites are good for a week or two; the screen shows their age and warns after 14
  days. So a refresh is wanted about twice a month, for under 1 KB of data.
- Limits: static RAM has about 3.4 KB free (docs/plus.md); our tools never write the 16 MiB data chip (AGENTS.md
  rule 2); a build is installed only when its hash is in `PLUS_REVIEWED` (`tools/install_plus.py`), and every
  serial / flash step needs the owner's approval. Program flash: the application may use 0xC000 .. 0xE8000
  (880 KB); the image is 777,200 B, so about 124 KB (30 sectors of 4 KB) are free below the codec slot.
- Upstream's storage layer (`core/storage`, the settings ring, the journal, the codeplug) lives on the data chip
  only. The running firmware has no driver that writes program flash: that is done by the boot loader / the
  installer's RAM stub with the radio in loader mode.

## Options

| | Option | Firmware change | Size | Who writes what | Risks |
|---|---|---|---|---|---|
| A | PC app: "refresh TLE, rebuild, install" in one flow | none | image as now | `install_plus.py`, application sectors (as every install) | every refresh is a new image hash: either the review step is repeated each time, or the tool must prove the new image differs from a reviewed one only in the table (below); a full install (about 190 sectors, a few minutes, loader mode) for 1 KB of data |
| B | An elements sector in program flash, outside the image: the last application sector (0xE7000 .. 0xE7FFF) holds a small record (magic, version, count, the `sat_info_t` rows, the note, CRC-32); the firmware uses it when valid, else the built-in table | about 200-300 B of code, 0 B RAM (read in place: program flash is memory mapped); the linker gives up one sector (880 -> 876 KB) | one 4 KB sector | a new small tool mode: erase and program that one sector through the installer's existing stub, backup and plan first, readback verify; the application image and its reviewed hash stay untouched | a new write target for the installer (must be exactly that sector, guarded like the others); `install_plus.py` must leave the sector alone (or rewrite it) when installing an image; a bad record must never stop the boot (CRC, bounds, fall back to the built-in table); still needs loader mode (power cycle) |
| C | Over the debug link while the radio runs, kept in RAM | parser + a RAM table | 368 .. 736 B of the 3.4 KB free RAM | nothing in flash | lost at power off (useless in the field); eats 10-20 % of the remaining RAM; rejected |
| D | Over the debug link, stored by the firmware's own storage layer on the data chip | a new record type or region | 1 sector | the firmware (not our tools) | the settings record is full and its layout must not change; a new region on the data chip sits in space the stock firmware and the codeplug own (the foreign-region guard exists for exactly this); a mistake costs calibration or the codeplug; rejected |
| E | The running firmware programs its own flash (option B's sector) from a debug link command | a program-flash driver running from RAM with interrupts off | ~1 KB code, a 4 KB sector buffer or streaming | the firmware | the application executes in place from the same flash: erase / program must run from RAM with everything stopped; a new, hard to test way to brick the application; upstream keeps this out of normal builds (`CONFIG_DEV_UPDATE` off); rejected |
| F | Typing the elements on the keypad | an editor | small | the firmware, settings | 138 characters a satellite; nobody will; rejected |

## The hash question for A

The review gate exists so that only an image someone looked at is flashed. A refresh changes `sat_table`, the
note string and, because the linker packs read-only data, possibly nothing else, but the tool cannot assume
that. Two ways to keep the gate honest:

1. Treat a TLE refresh as a normal build: build, `forced_rebuild_identical`, host tests, add the hash to
   `PLUS_REVIEWED`, install. Safe and already possible; the "one click" is then: fetch, build, test, show the
   hash and the diff of `tle.txt`, wait for the owner's OK, install (each radio step with its own approval).
2. A "data-only" proof: build the reviewed sources twice, once with the old and once with the new `tle.txt`, and
   accept the new image without a new review only if the two images differ solely inside the address range of
   `sat_table` and `sat_tle_note` taken from the map file (give them their own aligned section so that their
   size cannot move anything else). The installer then writes only the sectors that differ (one or two).

## Recommendation

**A, in its first form, now; B only if the owner refreshes often enough to mind.**

- A needs no firmware change, no new write target and no RAM. The PC app already has "data update + WSL build"
  and drives the guarded installer one step a button; a "Satellites: refresh elements" entry is a fetch of
  CelesTrak's amateur list into `tle.txt` (the generator already refuses a bad checksum and keeps only the
  catalog numbers in `sat_freq.txt`), then the existing build, test and install steps. Cost: a full install
  twice a month at most, and one more reviewed hash each time.
- The data-only proof (2) is a cheap later improvement of A: it removes the repeated review and shrinks the
  write to one or two sectors, with no firmware code and no new flash region.
- B is the cleanest on the radio (one sector, the image never changes) but adds a new flash write target and a
  record parser to qualify on hardware, to save a few minutes twice a month. Worth it only together with other
  field-updatable data (the repeater list and the ID database have the same refresh problem and are far bigger:
  11 KB and 78 KB); then one "data pack" region with a directory should be designed for all three, not a sector
  for the satellites alone.
- C, D, E and F are rejected for the reasons in the table.

Open points before any code: where the PC app keeps the fetched file's date and source for the on-screen note;
whether a refresh may also add or drop satellites (`sat_freq.txt` decides today); CelesTrak's request limits
(fetch at most once a day, keep the last good file).
