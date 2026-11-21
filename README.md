<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 Open25-DM32 Contributors -->

# Open25-DM32 Classic r14

Classic is firmware for the Baofeng DM-32UV with the RadioFoundry interface,
analog FM, DMR, conventional P25 and NXDN 4800/9600. It is based on
[RadioFoundry](https://github.com/RadioFoundry/RadioFoundry), MIT licensed.
Original license, author and per-file notices are retained.

Classic has its own Open25 signal-ring emblem on the boot screen and a
persistent Classic identity on the home screen. These use the native drawing
and layout owners; the upstream license and About-page attribution remain.

[Flasher v3](https://kd8zmm.com/open25-dm32/v3/firmware.html) installs firmware and
provides backup and recovery. Use the matched
[Classic CPS](https://kd8zmm.com/open25-dm32/classic-cps/) for programming.
[Report firmware issues](https://github.com/tylerwatt12/Open25-DM32/issues).

This source release keeps a small, explicit overlay on the pinned upstream
revision in [sources.lock.json](sources.lock.json). The preparation helper fetches
the exact public upstream and FreeRTOS revisions, verifies their commit/tree
identities, and applies the listed files. The overlay contains the Classic
product preset, band policy, required audio/protocol/state fixes and regression
tests. See [BUILD.md](BUILD.md) for reconstruction and reproducible-build checks.
Generated source, toolchains, images and test results remain under ignored
`build/`. Build and test commands do not connect to a radio.

Extended receive coverage is always enabled. FM, DMR and NXDN transmit within
136–174 and 400–480 MHz. P25 transmit is limited to 144 ≤ MHz < 148,
420 ≤ MHz < 450, 151.820–151.940, 154.570–154.600, 462.550–462.725 and
467.550–467.725 MHz. The four service ranges include their endpoints and the
frequencies between channels. Added 700/800 MHz reception remains receive-only
and is not hardware-qualified across the supported range. Saved RX-only flags
and normal transmit-admission checks remain active.

r14 uses native RFKV records and contact-based P25 talkgroups. An upgrade from
the exact published r13 application must use migration-aware Flasher v3, which
checks source identity and programming, presents the conversion plan, then saves
and reopens a required recovery ZIP before any write. Supported programming is
preserved; unknown sources, unsupported records or insufficient capacity stop
the upgrade. The radio contains no r13 converter. An r13 CPS backup must not be
written over native r14 programming. Preserve the recovery file and use the same
physical radio if restoring both the prior application and affected programming.

The package contains an application and attribution notices. It uses the radio's
existing compatible codec and preserves the bootloader, codec and calibration.
No codec, stock firmware, radio dump, key, recording or firmware binary is
included in this source release. The original component licenses are in
[LICENSE](LICENSE), [LICENSES](LICENSES), the prepared upstream tree and
[notices](notices). See [FORKING.md](FORKING.md) for the upstream attribution
requirements.

This remains experimental firmware. Bounded four-mode and prepared-r13-channel
migration/recovery checks are not long-term endurance, physical power-cut or
microphone loudness-parity qualification. Hardware revisions and operating
conditions may differ.
