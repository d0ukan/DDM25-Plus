<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# YSF and NXDN transmit: feasibility, and YSF Voice FR transmit (`CONFIG_YSF_TX`, ours)

Status 2026-11-19 (round 3, item 19): feasibility studied for YSF VW, YSF DN, NXDN48 and NXDN96. **Built: YSF VW
(Voice FR, IMBE) transmit** behind `CONFIG_YSF_TX`, **off in the Plus preset** until the owner approves on-air tests
(`--feature YSF_TX=y` builds it). Host-tested, cross-checked against MMDVMHost, never run on a radio. Nothing else
transmits differently: with the switch off the build behaves as before (a YSF channel stays RX only).

| Mode | Verdict | Why (details below) |
|---|---|---|
| YSF VW (Voice FR, IMBE, DT 3) | **feasible, built** | same 4800 sym/s 4FSK stream, same dibit map, same IMBE encoder and channel code as our proven P25 TX; only framing differs. Deviation is the DMR calibration (~±1.94 kHz), between YSF's normal ±2.7 and half ±1.35 kHz: to be checked with a Yaesu receiver |
| YSF DN (V/D mode 2, AMBE+2) | feasible on paper, **not built** | framing and FEC are done and cross-checked; the voice would come from the stock AMBE+2 encoder through RadioFoundry's `ambeVoice` stream while the native modem owns the HR_C7000, a combination never run (not even DN receive audio has been heard on the radio yet), and a channel would need a DN/VW choice |
| NXDN48 (2400 sym/s) | **not feasible** on the native modem; not built | the native stream is fixed at 4800 sym/s; doubling symbols keeps DMR's deviation, ~1.85x NXDN48's (off-channel); the only other route (software 4FSK on FM TX RAM) is unmeasured and reported not interoperable |
| NXDN96 (4800 sym/s) | possible in principle, **not built** | the stream could carry it like YSF; but no NXDN96 receiver exists here, no amateur use, and its full-rate (EFR) voice is not the codec we have |

## 1. What the P25 transmit path is, and why YSF can ride on it

The HR_C7000 "native" 4FSK path (`src/modem/native_test.h`, `integration/radiofoundry/native/hrc_native_chip.c`,
docs `../docs/native-modem-tests.md` in the lab repo) moves raw 36-byte blocks: one block per 30 ms slot
(`NT_BYTES=36`, `NT_SLOT_US=30000`) = 144 dibits = **4800 symbols/s**. The CPU supplies every bit: P25's sync,
NID, LDUs are all built in software (`p25_phy_tx_block` packs the dibits MSB first, no reordering, no inserted sync or
CACH). The driver sets `WORK_MODE 0x02`, L2 control `0x82`, one-send `0x80`, writes each block to both TX RAM halves,
stages the block head early on PHY_RDY (the 2026-11-06 head fix, PROGRESS.md) and **does not touch RF_MODE,
RF_MOD_BIAS, DEV_LIMITER or the RX FIR**: modulation level and shaping are the board's DMR calibration
(`hrc_native_chip.c` header; `native-modem-tests.md` "does not directly change RF_MODE, modulation bias/deviation").
The stream is therefore payload-agnostic: any 4800 sym/s 4FSK framing can be sent as long as the CPU keeps the 8-block
ring fed.

The live P25 path (`p25_lab.c` `live_run`): PTT -> `native_lab.c` checks (`radio_sm_tx_check`, battery, ownership)
-> `nt_arm_live` -> worker: mic route on, the board's PCM callback fills an 18-block FIFO (`mic_block`), the worker
encodes 20 ms at a time with the O3 speed codec (13.0 ms average, 18.6 ms worst on the radio, PROGRESS.md), packs
blocks, starts the modem at 7 queued, ends with a sealed 0x77 block; the channel time-out (`tot_s`), the modem
lease, stall and drain timers stop it. P25 TX passed on the radio (plus-radio-test T3; the Harris decodes it).

## 2. YSF VW (Voice FR) — feasible, built

**Framing.** A YSF frame is 960 bits (100 ms at 4800 sym/s): 40-bit sync `D471C9634D`, 200-bit FICH (Golay (24,12)
x4 + CRC + K=5 rate 1/2 + 20x5 interleave), 720-bit payload. In Voice FR each 144-bit payload section is exactly a
P25 IMBE codeword (P25's Golay/Hamming FEC, PN whitening and the same 144-bit interleave as the LDU voice:
`src/ysf/ysf_vw.h`, cross-checked with MMDVMHost and YSF2P25 in round 2). The header and terminator carry the
callsign data channels CSD1 (destination + source) and CSD2 (down/uplink); the first communications frame after the
header (FN 0, FT 1) carries CSD3 (rem1..rem4) and only two IMBE frames (MMDVMHost `CYSFControl::processVWData`
"the first packet after the header is odd"; DSDcc treats the frame after a header the same way); every later frame
carries five. MMDVMHost forces FN 0 / FT 0 for a VW frame whose FICH it can't read, which tells what the normal
frames carry. All of these encoders now exist in `src/ysf` and agree with MMDVMHost bit for bit (section 5).

**Symbol mapping.** The HR_C7000 maps dibit 00 -> +1, 01 -> +3, 10 -> -1, 11 -> -3 (guide via `docs/modem.md`
"Physical TX"), the standard 4FSK map. MMDVM (G4KLX) modulates DMR, P25 and YSF with one and the same byte-to-level
switch (`DMRTX.cpp`, `P25TX.cpp`, `YSFTX.cpp` `writeByte`: 0xC0 -> A, 0x80 -> B, 0x00 -> C, else D), so YSF uses
P25's dibit map. Our P25 TX decodes on a Harris and our P25 RX decodes it: the native stream's polarity is right
for P25, hence for YSF too, both ways. The round-2 note "symbol polarity of the C7000 raw stream for YSF unverified"
is therefore a low risk: YSF receive reads its bits through the very same `p25_phy_rx_air` mapping as P25.

**Deviation and shaping.** Levels in MMDVM's firmware (same scale for all modes): DMR ±1362/±454 (= ±1944/±648 Hz,
ETSI), P25 ±1260/±420 (= ±1.80/±0.60 kHz, TIA-102), YSF "high" ±1893/±631 (≈ **±2.70/±0.90 kHz**), YSF "low"
±948/±316 (≈ ±1.35/±0.45 kHz; the FICH Dev bit, MMDVMHost `LowDeviation`). The HR_C7000 guide gives the DMR
nominal ±1944/±648 Hz (`docs/modem.md`), and the Airspy bench of 2026-11-06 measured about +602, +1878, -709, -2027 Hz
(PROGRESS.md: estimates, not a service monitor). So **our YSF signal is about ±1.94 kHz: 72 % of YSF normal
deviation (-2.9 dB), 144 % of half deviation**. A 4FSK receiver that takes its decision levels from the signal (as
MMDVM does from the sync) is indifferent to that; Yaesu radios decode both normal and half-deviation stations
(MMDVM hotspots with `LowDeviation=1` are used with them), so a level between the two is expected to decode, but this
is the first thing the radio test must show (YT2). Changing the deviation would mean writing calibration registers
the native path deliberately leaves alone: not done. Shaping: MMDVM shapes YSF TX with a root raised cosine
(`rcosdesign(0.2, 8, 5, 'sqrt')`), P25 with a raised cosine; the HR_C7000's own DMR shaping (DMR also uses RRC 0.2)
is the closer match for YSF than for P25, and no programmable TX shaping was found (`docs/modem.md`). The FICH Dev
bit is 0 (normal, MMDVMHost's default); `-DYSF_TX_LOW_DEV=1` sets it for a bench comparison.

**CPU.** VW carries 50 IMBE frames per second, as P25 (9 per 180 ms): the same encoder at the same rate. The
framing adds per frame one FICH encode and per IMBE frame one `p25_imbe_encode` (P25's LDU path does the same plus
Reed-Solomon), and per transmission three CSD encodes (176-bit K=5 code). Host model with the encoder at 18 ms per
frame (radio worst 18.6 ms): the modem queue never fell below 4 blocks (120 ms).

**RAM.** No new buffers: the frame being sent lives in the YSF receive framer's 120-byte buffer (no receive runs
while the worker transmits), the codec context, the mic FIFO and the block are the P25 path's. New static RAM: the
callsign, 8 B. Flash +2,600 B with the switch on (788,512 -> 791,112 B; RAM 208,472 -> 208,480 B), +272 B with it
off (the receive side's header callsign). Stacks: the UI (368 B free) and radio (824 B) tasks' worst paths unchanged,
the P25 worker's 4 B deeper (908 B free).

**Risks it does not take.** The P25 transmit path is unchanged: `live_run` only branches to `ysf_live_run` when the
channel is a YSF one (`p25_lab_set_ysf`), and the two never run at once.

## 3. YSF DN (V/D mode 2, AMBE+2) — feasible on paper, not built

- Framing: done and cross-checked in round 2 (`ysf_vd2_voice_put`, `ysf_vd2_data_put`; `test_ysf_ambe_ref`: our DN
  voice bits equal YSF2DMR's, and RadioFoundry's `ambeFecEncode` of them equals YSF2DMR's 72-bit DMR frame). For TX
  the 72-bit DMR frames from the encoder go back through `ambeFecDecode` (exact for clean frames) to 49 bits.
- Encoder: the stock DVSI AMBE+2 (rate 2450+1150, the DMR one) in the codec slot, driven by RadioFoundry's
  `ambeVoice`: while its source is active, the audio block callback encodes each mic block and queues 72-bit frames
  (`AMBE_VOICE_QUEUE` 8 = 160 ms) for `ambeVoiceSource()->next()`.
- RAM: nothing new (the AMBE state 13.3 KB and its queues exist). CPU: the encoder runs in the audio task as in a DMR
  call; the worker's framing is light.
- Codec ownership: `hal_audio_t` has one block callback. The native P25/VW TX installs its own (`mic_block`), while
  `ambeVoice` owns the stream from its first `begin` until `ambeVoiceStop()` (the YSF receive worker already calls
  that when it leaves a YSF channel). DN TX would start `ambeVoice`'s stream inside a native TX session (WORK_MODE
  0x02, the HR_C7000 not in DMR mode), with `native_mic_route(true)`.
- Why not now: that combination (the stock encoder fed while the native modem owns the chip) has never run; even
  DN receive audio through `ambeVoiceSink` in FM board mode is unheard on the radio (no YSF transmitter); and a
  channel would need a DN/VW setting (a record field, an editor row and its strings). VW reuses exactly the audio
  path that already transmits P25 on the air. Next step once YT1-YT4 pass and DN receive is heard: `ysf_tx` grows a
  DT 2 path (DCH per FN: 0 destination, 1 source, 2 downlink, 3 uplink, 5 rem3/rem4, as MMDVM_CM DMR2YSF), the
  worker pulls `ambeVoiceSource` frames; estimate ~1.5 KB flash, no static RAM.

## 4. NXDN48 and NXDN96

**NXDN48 (6.25 kHz, 2400 sym/s, ±1050/±350 Hz) — "the C7000 4FSK stream can't run at 2400": confirmed for our
native path.** Evidence: the block format is fixed, 36 bytes per 30 ms slot = 4800 sym/s (`native_test.h`; the
guide's TX RAM halves `0x16000000..23` / `..24..47`, "36 bytes represent 144 symbols and nominally 30 ms at 4800
symbols/s", `docs/modem.md`); `hrc_native_chip.c` writes only WORK_MODE, L2 control, TX/RX control, interrupt masks
and the sync matchers, and neither it nor RadioFoundry's `hrc7000.h` has a symbol-rate setting (the 4FSK framer is the
DMR TDMA one). Sending each NXDN symbol twice at 4800 sym/s is possible as bits, but the deviation stays DMR's
(~±1.94 kHz, ~1.85x NXDN48's ±1.05 kHz) and the spectrum DMR's: DM32-nxdn-p25 simulated it (0.3 % BER floor, -34 dB
adjacent channel power against -43.7 dB, `research/ref/DM32-nxdn-p25-ANALYSIS.md`) and rejected it. Their other
route, a software 4FSK modem writing 8 kHz samples to the HR_C7000 FM TX RAM, works for receive but its transmit is
not decoded by Icom, Kenwood, DSD+ or an MMDVM hotspot in their field reports, its level and response are unmeasured.
Not built; AGENTS.md keeps any such TX behind owner approval.

**NXDN96 (12.5 kHz, 4800 sym/s).** The native stream could carry its framing as it carries YSF (deviation would again
be the DMR calibration; NXDN's 9600 bit/s mode is specified wider, about ±2.4 kHz outer, not in our sources). Not
built: we have no NXDN96 receiver at all (our NXDN receive is NXDN48 on the FM path), no amateur NXDN96 use around the
owner and no transmitter to test against; and NXDN96 voice is commonly the full-rate EFR codec, while the stock
vocoder here runs the DMR half rate (EHR-compatible) only.

## 5. What was built (`CONFIG_YSF_TX`)

- `src/ysf/ysf.{c,h}`: the CSD data channels (`ysf_csd_put` / `ysf_csd_get`: CSD1, CSD2 in the header and terminator,
  CSD3 in the VW sub-header; 20 bytes whitened, CRC, K=5, 9x20 interleave). The Viterbi decoder now takes any length
  (FICH and DCH 100 steps, CSD 180), with one shared interleaver; the FICH/DN cross-checks still pass bit for bit.
- `src/ysf/ysf_tx.{c,h}`: the Voice FR transmitter's frame sequence (header, sub-header, voice frames, terminator),
  FICH fields (CS 2, CM group, MR direct, Dev 0, DT 3, SQ = DG-ID), the IMBE sync bit alternating as in P25, and a
  bit-level hand-out to the 288-bit blocks.
- `p25_lab.c`: `ysf_live_run` (P25's live path with YSF framing: 3 preamble blocks, header, sub-header, voice,
  terminator, the rest of its block and one sealing block of the 0x77 preamble; empty PTT never starts the modem;
  release pads the frame begun with encoded silence); `p25_lab_set_ysf_tx` (the callsign); `p25_lab_live_begin`
  refuses a YSF channel without a callsign, and in a build without the switch refuses a YSF channel always. YSF
  receive now also names the caller from a header's CSD1 (VW calls had no callsign before).
- `native_lab.c`: on PTT down on a YSF channel, the callsign from the radio name (as the CW beacon:
  `radio_cwid_callsign`, Settings > My radio) goes to the worker.
- `tools/plus_features.py` `prepare_ysf_tx`: switch plumbing (needs YSF, DEV_TX, SFR_CWID); a YSF record keeps its
  own RX only flag (`codeplug.c`, `codeplug_p25_valid`); `radio_sm_tx_check`: a YSF channel is refused with
  `TXDENY_MODE` without the switch, with `TXDENY_NO_CALL` ("Radio name must be a callsign" / "Settings > My radio",
  existing strings) when the radio name holds no callsign. Every P25 gate applies unchanged: TX inhibit, RX only,
  the P25 TX bands 144-148 / 420-450 MHz (amateur), battery, busy lockout, the channel time-out, the modem lease,
  stall and drain timers. The VFO (round-3 job "vfox") goes through the same `radio_sm_tx_check`.
- Default: `CONFIG_YSF_TX=n` in `tools/build_radiofoundry.py`'s Plus preset.

## 6. Tests (host only; fakes, nothing transmits)

| Test | What |
|---|---|
| `tests/plus/test_ysf_tx.c` (make test) | CSD round trips and error correction, CRC refusal of garbage; FICH bytes; whole transmissions (1 to 42 IMBE frames, odd chunk sizes, 10-character callsign) through our framer, FICH, CSD and VW decoder: identical IMBE frames, headers, terminator |
| `tests/plus/test_ysf_tx_worker.c` (make test, built with the switch and without) | the real worker (`run`, `ysf_live_run`) with a fake modem taking a block every 30 ms (underrun = fault) and a fake microphone: 0.1-5 s calls, encoder at 0/5/13/18 ms per frame (least queue 4 blocks), every IMBE frame equal to a fresh encoder's for the mic blocks taken, then silence; channel time-out; empty PTT never starts the modem; no callsign refused; without the switch a YSF transmission is refused |
| `tests/plus/test_ysftx_radio.inc` (make test, `test_radio_ysftx_0/1`) | the real radio core and native controller: without the switch PTT on a YSF channel is refused (RX only / mode) and nothing is armed; with it refused for no callsign, outside the bands, RX only, inhibit, battery, else armed on the TX frequency with "TA1ABC" handed over, and the time-out stops it |
| `tests/plus/test_ysf_channel.c` (make test, twice) | a YSF record that allows TX: refused without the switch, kept (also after a reboot) with it |
| `tools/test_ysf_tx_ref.sh` (needs `../research/ref/MMDVMHost`) | 120 transmissions, 1,098 frames: MMDVMHost's `CYSFFICH` reads every FICH and re-encodes our bytes; `writeHeader` gives our header/terminator bit for bit; `processHeaderData` reads our callsign; the sub-header passes `processVoiceFRModeData`; `processVoiceFRModeAudio2/5` find 0 errors and change no bit |
| `tools/test_ysf_ref.sh`, `tools/test_ysf_vw_ref.sh`, `test_ysf_worker` | unchanged results after the Viterbi rework |

## 7. On the radio (owner approval for each transmitting step; plus-radio-test.md YT1-YT6)

A YSF receiver is needed: a Yaesu Fusion radio, an MMDVM hotspot in YSF mode, or an SDR with DSD/DSDcc. The first
questions are YT2's (does a Yaesu radio decode ±1.94 kHz, and with Dev 0 or 1) and YT3's (the callsign shown,
DG-ID honoured).
