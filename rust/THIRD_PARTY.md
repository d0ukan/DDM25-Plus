# Third-party code in the Rust programmer

The program (`crates/*`) is GPL-3.0-or-later. Everything below is compatible with it. `Cargo.lock` pins every exact
version; `cargo tree -e normal --format "{p} {l}"` lists all of them (about 380 crates, most MIT OR Apache-2.0).

## Embedded data

| File | Source | Licence |
|---|---|---|
| `crates/ddm32-core/data/ram-monitor.bin` | RadioFoundry stub (pinned upstream `dfbaf20c`), SHA-256 `eae5b986…aee9`, checked before use | MIT (`LICENSES/RadioFoundry-MIT.txt`) |
| `crates/ddm32-core/data/codec-dm32.json` | RadioFoundry `tools/hrc7000_codec.py` / `rfoundry/codecmap.py` profiles and maps (addresses and hashes, no codec byte) | MIT |

## Direct dependencies

| Crate | Used for | Licence |
|---|---|---|
| num-bigint, num-integer, num-traits | Ed25519 field arithmetic (a port of the RFC 8032 reference code, see below) | MIT OR Apache-2.0 |
| sha2 | SHA-256 / SHA-512 | MIT OR Apache-2.0 |
| crc32fast | zlib CRC-32 | MIT OR Apache-2.0 |
| serde, serde_json | JSON (manifests, receipts, settings) | MIT OR Apache-2.0 |
| ureq (+ rustls, ring, webpki-roots) | HTTPS to GitHub and radioid.net only; redirects checked by the program | MIT OR Apache-2.0; rustls Apache-2.0 OR ISC OR MIT; ring Apache-2.0 AND ISC; webpki-roots CDLA-Permissive-2.0 (the Mozilla root list) |
| zip, flate2 | release and program zips | MIT; MIT OR Apache-2.0 |
| regex, fancy-regex | the linker map and the AD 2.18 text rules (look-behind) | MIT OR Apache-2.0; MIT |
| unicode-normalization | NFKD for the RadioID name folding | MIT OR Apache-2.0 |
| pdf-extract, lopdf | AIP PDF text | MIT |
| chrono | the local date of backup folder names | MIT OR Apache-2.0 |
| hex, base64 | encodings | MIT OR Apache-2.0 |
| serialport (feature `radio` only) | the serial port | MPL-2.0 (file-level copyleft; unchanged, used as a library) |
| slint, slint-build (window) | the user interface | GPL-3.0-only OR Slint Royalty-free 2.0 OR Slint Software 3.0; used under **GPL-3.0** (this program is GPL-3.0-or-later, so the combined program is distributed under GPL-3.0); "Made with Slint" is shown in About |
| rfd (window; xdg-portal + async-std on Linux) | file pickers for data pack inputs | MIT |
| tempfile, png (tests only) | temporary folders, screenshots | MIT OR Apache-2.0 |

Indirect dependencies of note: clipboard-win and error-code (Windows clipboard in winit) BSL-1.0; ICU4X data
Unicode-3.0; encoding_rs (Apache-2.0 OR MIT) AND BSD-3-Clause.

## Why Ed25519 is a port and not ed25519-dalek

The program must accept and refuse exactly what the Python (`pc/radiotools/ed25519.py`) and C# programs do,
including the reference code's edge cases (a non-canonical public key or R is refused, s >= q is refused, the
cofactorless check). A line-by-line port of the RFC 8032 section 6 reference code gives that by construction and is
checked against the RFC vectors and 36 signatures and the verify cases made by the Python code. Only public data is
verified, so constant time does not matter; signing exists for the tests only.
