<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# dPMR receive (`CONFIG_DPMR`, ours; receive only)

Status 2026-11-18: protocol core, board glue and UI built into Plus and host-tested; **not run on a radio**, no
dPMR transmitter has been heard yet. No transmit.

dPMR (ETSI TS 102 490 dPMR446, TS 102 658 dPMR Mode 1-3) is 6.25 kHz FDMA: 4FSK at 2400 symbols/s, +-1050 /
+-350 Hz, root raised cosine 0.2 with sin(x)/x shaping, AMBE+2 voice at 3600 bit/s (2450 + FEC). That PHY is NXDN48's
to the bit (TS 102 490 12.2.2), so dPMR runs on the NXDN receive path unchanged: the radio stays in FM mode, the
NXDN software demodulator (`src/nxdn/nxdn_demod.c`) reads the HR_C7000's 8 kHz FM RX audio with the flat path on,
and only the frame layer differs.

```
FD6818 discriminator (flat) -> HR_C7000 FM RX RAM 8 kHz -> fm_rx_audio.c ring (APRS's path)
  -> nxdn_demod (dibits) -> src/dpmr: FS1 / FS2 / FS3 / colour code sync -> header (HI) / CCH / END
  -> 4 x 72-bit voice blocks a frame -> the NXDN glue's 20 ms pacing -> ambeVoiceSink() (stock AMBE+2) -> speaker
```

## The air interface as implemented (from the standards)

Clause numbers are TS 102 490 V1.2.1 unless marked 658 (TS 102 658 V2.3.1).

| Item | What we do | Reference |
|---|---|---|
| Symbols | 01 +3, 00 +1, 10 -1, 11 -3; either polarity (hunted, learned per transmission) | 12.2.2.1 |
| Frame | 80 ms, 384 bits; superframe of 4 frames (320 ms) | 4.2.2 |
| Header | preamble >= 72 bits (658 6.1.6: 5F repeated), FS1 48, HI0 120, CC 24, HI1 120 | 4.2.2, 9.5 |
| Payload frame | FS2 24 (frames 1, 3) or the colour code 24 (frames 2, 4), CCH 72, TCH 4 x 72 | 4.2.2, 9.1 |
| End | FS3 24, END 36 twice | 5.3, 9.6, 658 11.6 |
| Syncs | FS1 57FF5F75D577, FS2 5FF77D, FS3 7DDFF5, FS4 FD55F5DF7FDD (= FS1 inverted: packet data header, not followed) | 6.1 |
| Colour / channel code | 64 codes of 12 symbols (+3 / -3 only), 3 symbols apart; numbered 0-63 as 658 Table 6.1. dPMR446 V1.2.1 radios take their code from the channel (Group A / B, Table 6.1.5): "Any" hears them all | 6.1.5, 658 6.1.5 |
| HI | header type 4, called ID 24, own ID 24, communication mode 3, format 4 (658: version 2 + format 2), reserved 2 (658: emergency, preservation), call information 11; CRC8 x^8 + x^2 + x + 1; ten (12,8) shortened Hamming words (x^4 + x + 1, Table 7.3); 12 x 10 interleave (columns in, rows out); scrambled | 5.2, 9.5, 7 |
| CCH | frame number 2, ID quarter 12 (FN 0/1 the called ID's upper / lower half, FN 2/3 the own ID's), mode 3, format 4, reserved 2, slow data 18; CRC7 x^7 + x^3 + 1; six Hamming words; 12 x 6 interleave; scrambled | 5.1, 9.1 |
| END | end type 2, ARQ 2, wait 4, status 5, reserved 4; CRC7; three Hamming words, no interleave; scrambled; sent twice | 9.6 |
| Scrambler | x^9 + x^5 + 1 preset to all ones, started again for every block (HI0, HI1, CCH, each END copy); bit order as dsdcc's DSDdPMR (o[n+9] = o[n] ^ o[n+4]) | 7.4, 658 7.3 / 11.6 |
| TCH (voice) | not scrambled; each 72-bit block is the DMR AMBE+2 air frame (Golay (24,12) C0, (23,12) C1 whitened, C2, C3 in DMR's interleave), passed to the DMR vocoder as is | 9.1; dsdcc uses DMR's tables on it |
| Modes | 000 voice, 001 voice + slow data, 101 voice + appended data (frames whose slow data says DP 11 carry data: not played), 010 / 011 / 100 data (reported, never played) | 5.7, 5.9.2 |
| Format | low two bits 00 = all call (shown "ALL"), 01 = peer to peer | 5.8, 658 5.5.6 |
| IDs as text | 0xFFFFFF "ALL"; 8-bit common ID + 0xFFFF (dPMR446 initial services) "CID n"; up to 999**** the seven characters of 658 annex A.1.2 (B2: K1 x 1464100 + K2 x 146410 + K3 x 14641 + K4 x 1331 + K5 x 121 + K6 x 11 + K7, * = 10); else the number | 8.1.1, 658 A.1.2 |

No field in either standard marks encryption (vendors' voice scramblers are proprietary), so nothing can be shown as
"Encrypted": a scrambled call plays as noise-like audio.

The vocoder is not specified by the standards (the dPMR MoU chose AMBE+2 3600 / 2450, as DMR); that its 72-bit
block is DMR's air frame bit for bit is what open decoders do (dsdcc, F4EXB: DSDdPMR feeds it to the DMR AMBE
deinterleave), not something we could check against a capture.

## Receiver behaviour (`src/dpmr/dpmr_rx.c`)

- Hunt: FS1 (at most 3 wrong symbol signs of 24 and 8 wrong bits of 48) starts a header; FS2 (at most 2 wrong bits of
  24) a tentative payload frame. A tentative frame is confirmed only by the next frame's sync field 192 symbols on
  (a colour code within 1 bit or FS2 within 2): its CCH is held until then, its voice not played, so noise never
  starts a call. Parts of FS1 are within two bits of FS2: an FS1 seen during a tentative frame takes over.
- Synced: the sync field is checked where it must be (FS2 / FS3 before frames 1 and 3, a colour code before 2 and 4)
  on symbol signs (noise mostly turns +3 into +1); three misses in a row are ridden over, the fourth loses sync.
- The header is decoded from HI0, else HI1, else word by word from whichever copy decodes (and, where neither does,
  the bits both copies agree on with the 2^k ways of the k <= 4 others), checked by CRC8. With the 72-bit minimum
  preamble the NXDN demodulator is still settling: in a moderately noisy channel about 1 header in 20 is lost (host
  test), and the call then comes by late entry, one superframe later.
- Late entry: a voice CCH starts the call; the IDs come from the CCH quarters (called after FN 0 + 1, own after
  FN 2 + 3), the colour code from frames 2 / 4.
- The colour code filter: the channel's code (or any). A foreign code ends the call and hunts again; with a code set,
  voice plays only once the code has been seen to be ours.
- 16 symbols of preamble keep the demodulator from re-acquiring (its onset re-acquisition) until FS1 is through.

## Files

| File | What |
|---|---|
| `src/dpmr/dpmr.{c,h}` | the coding: scrambler, CRC7 / CRC8, (12,8) Hamming, interleaves, colour code table, HI / CCH / END decode, ID text |
| `src/dpmr/dpmr_rx.{c,h}` | sync, framing, the call, the receiver wrapper (NXDN's demodulator + this frame layer) |
| `integration/radiofoundry/dpmr/radio_dpmr.h` | the radio task API (poll, heard, caller, Home's caller line) |
| `integration/radiofoundry/nxdn/radio_nxdn.c` | holds the dPMR glue too (below) |
| `tests/plus/test_dpmr.c` | host test (`make test`; runs the NXDN suite first) |
| `tests/plus/test_dpmr_channel.c` | the channel record through the real codeplug store |
| `tests/plus/test_ui_dpmr.c` | the channel editor flows in the upstream UI simulator |

All ours (GPL-3.0-or-later). The standards were read for the structure; dsdcc's dPMR code (GPL-3.0) was read for
the scrambler's bit order and the voice block layout only, nothing copied.

## Sharing with NXDN (RAM)

`radio_nxdn.c` keeps one receiver object (static in Plus, `RADIO_NXDN_HEAP=0`) whose receiver part is a union of
`nxdn_receiver_t` (600 B on the host, 576 on the CK803S) and `dpmr_receiver_t` (424 B host: the shared demodulator
plus a 192 B frame layer); `_Static_assert`s keep dPMR inside NXDN's size and the voice block sizes equal. The pacing
queue, read buffer, flat path, retry and voice sink code are the same. Exclusivity is by construction: `kind` says
which receiver the object holds; both `radio_nxdn_poll()` and `radio_dpmr_poll()` run in the radio task, one after
the other, a channel has one mode, and a poll whose mode differs from the running one stops it before starting its
own (a poll that doesn't want its mode leaves the other alone). The FM audio ring is the APRS / FM caller ID one,
free on a non-FM channel as for NXDN.

## Size (csky-elf-gcc 14.3 `-Os`, Plus build)

| | Flash | Static RAM |
|---|---:|---:|
| Plus with `DPMR=n` | 710,236 | 204,520 |
| Plus with dPMR | 716,344 (+6,108) | 204,640 (+120: Home's caller line 80, the call-history copy and caller 40) |

Stacks unchanged (radio 828 B free, UI 368, audio 1608): the frame layer runs in the radio task below
`radio_nxdn_poll`'s frame and is shallower than NXDN's (no Viterbi); Home's caller line is built by
`radio_dpmr_caller_line()` into a static buffer.

## Integration (`prepare_dpmr` in `tools/plus_features.py`, the last of `prepare()`)

- `CONFIG_DPMR` (needs `NXDN=y`), `core/dpmr` in the build, `core/radio/radio_dpmr.h`.
- Codeplug: `RADIO_MODE_DPMR = 5`, record **`CH_MODE 0xA8`** (P25 0xA5, YSF 0xA6, NXDN 0xA7), the colour code filter in
  `CH_RX_GROUP` = `ch->p25_tg`: 0 any, else code number + 1 (1-64); always RX only, 12.5 kHz. Stored only when valid.
  A build without `CONFIG_DPMR` (Classic, older Plus) does not take a 0xA8 record (the channel is not shown), as for
  NXDN; the upstream CPS shows and keeps only modes 0 / 1, so a CPS edit of such a channel is not expected to keep
  it (same as YSF / NXDN; unverified with the CPS). Frequencies are stored in 10 Hz steps: dPMR446's 446.103125
  becomes 446.10312 (5 Hz, nothing next to the crystal's error).
- Radio: `radio_dpmr_poll()` beside NXDN's, same conditions (not TX, FM board mode, no scan / sweep / search /
  broadcast); `outputs()` keeps the analog speaker shut, the green LED on while dPMR frames are heard; PTT on a dPMR
  channel: TX denied. Battery save stays off (as NXDN: not an FM channel). Scan and dual watch treat a dPMR channel
  as NXDN's (no decoding while the scan holds it).
- UI: Mode cycles FM -> DMR -> P25 -> YSF -> NXDN -> dPMR -> FM; the talkgroup row becomes "CC" (typed 0-63, "Any"
  for 0 stored, the context key while typing sets Any; past 63: "CC goes up to 63"), NAC / ID rows hidden; "dPMR" on
  Home, the channel list and the debug link (`dpmr`); Home's bottom line "dPMR 1234567 > 7654321 CC 16" (shorter
  alternates "1234567>7654321 CC16", "1234567>7654321"; "dPMR call CC 16" before the IDs; "dPMR data ..." for data);
  call history entries "dPMR 1234567" / "> 7654321 CC 16 5m" ("> ALL" for an all call), without PTT or actions.

## Host tests

`test_dpmr` (clang ASan + UBSan and gcc, all passed; ~0.4 s): the scrambler regenerated by an LFSR, FS4 = FS1
inverted, the 64 codes (+-3 only, 3 symbols apart, one wrong symbol corrected), CRC7 / CRC8 against long division,
the Hamming code (cyclic, all bytes, every single error), HI / CCH round trips through an independent encoder (from
Table 7.3's rows and the interleave matrix as written), random blocks rarely taken (7 of 4000), END copies, ID texts
(658 Table A.2). Whole transmissions through the float 4FSK modulator to 8 kHz samples: clean call (48/48 voice
blocks bit exact, IDs, CC, end), own / foreign colour code, noise + 150 Hz carrier error + 50 ppm + 20 % less
deviation (and 40 runs of it: headers 38/40, IDs 40/40, voice 98 %), 300 ppm, 300 Hz rms noise, 750 us de-emphasis
undone by the demodulator's inverse (exact; not undone: nothing decodes, hence the flat path), inverted
discriminator, an FS4 packet header not taken for an inverted voice header, late entry (IDs and CC, 52 of 64 blocks),
late entry on a foreign code, a data call (reported, muted), voice + appended data (data frames muted), emergency
flag, two callers back to back, 20 s of noise at three levels / speech-like FM / an NXDN48 call: no call and no
voice; NXDN still decodes the same NXDN call, and dPMR audio gives NXDN no call. `test_dpmr_channel` and
`test_ui_dpmr` (9 editor flows) in `make test`.

## What the radio must confirm (all unverified)

1. Everything NXDN's list in docs/nxdn.md says about the flat FM audio path (it is the same path).
2. A dPMR446 radio (or an SDR sending dPMR) at close range on 446.1-446.2 MHz: header decoded (IDs and CC on Home at
   once, not a superframe later), voice clear. If headers never decode but late entry works, or nothing decodes,
   suspect the scrambler's bit order first (the mirror sequence, x^9 + x^4 + 1 read the other way), then the HI
   field order.
3. Voice: if frames decode (IDs shown) but the audio is garbled, the TCH block's layout differs from DMR's air frame
   (e.g. scrambled or another interleave) - compare with DSD-FME on a capture.
4. The colour code number our Home shows for the dPMR446 radio's channel (658 numbering), and that a wrong code set
   in the channel stays silent.
5. Real dPMR446 radios' preamble length (longer than 72 bits makes headers safer), and reception with the channel on
   446.10312 etc. (the record's 10 Hz step) through the 12.5 kHz filter.
