# Changelog

DDM32 Plus is a fork of Open25-DM32 Classic (r13) for the Baofeng DM-32UV. See the disclaimer in
[README.md](README.md) before installing anything.

## Unreleased

- Windows programmer 2.0.0-beta.2: new pages Radio health, Live panel and My channels (codeplug viewer with search
  and CSV export); one radio step at a time across all pages; read-only guard on the health and live readers.
- Airfields by province: Tools > Airfields opens on "All airfields" and the provinces that have airfields, in plate
  order with their counts ("06 Ankara (5)"), as Tools > Repeaters does; OK lists that province's airfields nearest
  first, with the details, listening and air watch as before. Each airfield's province comes from its position (the
  map's provinces; at a border OurAirports' own province decides: Sabiha Gökçen is in İstanbul, Çukurova in Mersin).
- Spectrum in colour: the waterfall and the 3D waterfall draw signal strength as a colour ramp (dark blue, blue,
  cyan, green, yellow, orange, red; the light theme its own shades) instead of a few grey and theme colours.
- The status strip's signal bars light only while something is heard (squelch open, the monitor key, or a DMR call at
  its own level). Before, they followed the receiver's noise floor and climbed and fell on any channel as if a call
  or data came in.
- DMR receive audio diagnostics, round 2: on Tools > Diagnostics key 8 steps a digital gain on the decoded voice
  (x1, x2, x4, x8, saturating; beeps keep their level) and a sixth line shows the samples it clipped and the peak after
  it; the dropped-blocks counter counts only blocks lost while the speaker plays (the idle stream between calls filled
  it before). Power-on is x1: nothing changes until the owner presses 8. For the radio rows DA-7..DA-11.
- Turkish: the texts the catalogue missed are translated: the multi-watch ring line ("Döngü 0.6 s"), its # menu
  ("Konuş: B", "Değiş: C", "Durdur, kal: B") and toasts, the cross-band form's Slot row, the caller line before the
  first P25 / YSF / NXDN link control, the battery's time left ("4 sa 20 dk"), upstream's zone toasts and "In Zones"
  list, About's "Based on" lines. A host test now fails when any text the radio can show has no Turkish.
- tools/datapack.py finds the aerodrome code in AIP file names such as LT_AD_2_LTAC_en.pdf (an underscore before
  the code hid it), else in the page text before AD 2.18, as the Windows programmer does.

## v1.0.0-beta.4 (2026-11-21)

Installed and checked on the author's radio with the new Windows programmer, published with it (2.0.0-beta.1: `DDM32Plus-Setup-2.0.0-beta.1.exe`).

- DOTA Transfer pairs without typing: both roles start on the same frequency (Default 433.450 MHz, a 70 cm FM
  simplex channel) and tone (88.5 Hz); the frequency can also come from the current channel (when it is a simplex
  amateur one), the last session or be typed; the tone from a short list. DMR uses colour code 1 and slot 1, no longer
  shown or asked.
- APRS Mic-E beacons: Settings > APRS > Beacon format (Standard / Mic-E) and Mic-E message (Off duty, En route,
  In service, Returning, Committed, Special, Priority; En route first). Manual, automatic and SmartBeaconing beacons
  (and the ISS one) go out as Mic-E positions with speed, heading, altitude and the comment: shorter packets. The
  emergency stays the SOS's own. Standard by default.
- APRS station page: OK on a station in Tools > APRS > Stations heard opens its page: the callsign and its symbol by
  name (car, house, ...), distance and direction from us, position (degrees and locator), altitude, speed and
  heading, comment, when it was heard (direct or through a digipeater) and how many packets, and the last messages
  from it and the one we sent it. OK: Go-To follows it; 0: the map centred on it with its details; #: a message to
  it; Up / Down: the previous / next station.
- The SOS key's page with no action chosen fits the screen ("No SOS actions") and points to Settings > Keys, where the
  actions are picked now (it still said Tools > SOS key).
- Airfields: the frequencies that are not in an aerodrome's official publication are removed (61, among them
  another unit's approach frequencies and the unofficial ones of Bolu); nothing is added. An airfield keeps all of
  its frequencies (up to 64), and the list line shows "+1" when the service shown has more ("TWR 125.000 +1").
- Air watch: on an airfield's page, # puts up to six of its frequencies (not the ATIS) on the multi-watch ring, each
  letter its own VFO, received in AM; the radio hops between them and stops on activity. Stopping the watch brings
  everything back as it was (your channel or VFO, your own dual / multi-watch); nothing of it is saved.
- Spectrum: a third view, a 3D waterfall (the last sweeps as ridges in perspective, the newest in front); * cycles
  waterfall, 3D, bars alone. Multi-watch rows in the air band say "AM".
- User data packs: a RadioID list and an air band frequency list the user builds on their own PC from sources they
  download themselves (radioid.net; the aerodromes' AIP pages) can live on the radio's data chip, in a region of its
  own (two slots each, switched only when a new pack is complete and checked). DMR callers get their names from the
  RadioID pack, the airfield screens their frequencies from the air band pack; Settings > About shows which pack is in
  force. Loaded through the programming cable with the CPS protocol (tools/datapack.py builds the pack). Without a
  pack nothing changes.
- Data packs from radio to radio: Tools > DOTA Transfer can send the RadioID pack or the air band pack in force
  (What: "RadioID pack" / "Air band pack") to another radio over FM or DMR. The receiver is asked "Data pack: <name>,
  <date>, <size>. Accept?" and keeps its old pack until the new one has arrived whole and checked; a stop, a lost link
  or a power cut leaves the old one. Radios with older firmware simply don't answer (nothing changes on them).
- DOTA over DMR: a receiver's refusal ("Codeplug full", "Other version") now reaches the sender (it was sent to no
  one, so the sender only saw "No answer").
- Last calls (Tools > Last call): the last 8 calls heard from every mode (DMR, P25, YSF, NXDN, dPMR, FM caller IDs)
  and every watched channel, newest first: the watch letter (A-F), the mode, the caller, the talkgroup, how long ago.
  The newest P25 call still plays again with OK; the other modes keep no audio. **Not tried on the radio yet**
  (docs/plus-radio-test.md, rows LC-*).
- Cross-band store and forward (Tools > Cross-band), DMR only: an over heard on frequency A is recorded and sent on
  frequency B (and B to A); both in the amateur bands, never PMR446, a repeater, satellite or APRS frequency. Attended
  operation only; while it runs the keys are locked (Back stops it) and Home shows CROSS. Up to 38 s an over (the
  memory; 3 minutes at most), the callsign in CW on each frequency used, then at least every 10 minutes. FM and P25
  are not possible this way on this radio (docs/plus.md). **Not tried on the radio yet** (rows XB-*).
- DMR receive audio diagnostics (owner's report: received DMR audio crackly and artificial): Tools > Diagnostics shows
  five new lines first (the voice queue, the speaker FIFO, the audio task's timing), and on that page 0 steps the
  speaker FIFO feed between m0 (as before), m1 (one block of margin before the FIFO starts), m2 (paced, as P25) and
  m3 (both); 9 zeroes the counters. Power-on is always m0: nothing changes until the owner presses 0. **For the radio
  test rows DA-*;** the default changes once they show which feed sounds right.
- Zones show their size where channels are added: the zone list's Add channel row "26/80", the channel editor's
  Zones "26/80", Create New Zone "5/68"; a full zone says "Zone full: max 80 channels"; a channel the radio can't
  read shows in its zone as "27 ? Ch 123" instead of vanishing (Home skips it).
- DMR channel editor: RX group list Any now means every talkgroup on the channel's slot (no longer digital
  monitor: no private calls of others, Home no longer says DMon for it); "RX list" shows "All TGs TS1"; the picker offers
  Any right after None and says in one line what Any or None hears; a channel made DMR gets the first group contact; an
  RX only DMR channel saves without a TX contact; Direct mode explains itself.
- Digital monitor: DMon on Home follows the toggle again; a channel whose RX list is Any shows "AllTG" instead, and the
  toggle says what it does ("Digital monitor on: every call", "Monitor off: channel hears all TGs").
- Delete channel: the channel editor's last row and the zone channel menu's last entry; asks first (No focused), takes
  the channel out of every zone and frees its number; refused for the channel in use and while cross-band runs.
- P25 NAC in decimal first: the editor and VFO settings show "659 ($293)" and take the decimal value (0-4095, the
  knob steps by one); # is Any / Same as RX as before, # # the hex digits; 3967 on RX and 3966 / 3967 on TX are refused
  with a reason; Home's VFO row shows the NACs in decimal.
- P25 search and DMR search: any range of the radio's 2 m / 70 cm bands. * (or a digit) opens the range editor: Lower
  and Upper typed or moved by a step (held Left / Right: 1 MHz), a step of 2.5, 5, 6.25, 10, 12.5, 20, 25, 50,
  100 kHz or 1 MHz, and Move to slide the whole range; OK applies it. Each search remembers its range and step while
  the radio is on; Left / Right still pick the presets. **Not tried on the radio yet** (rows SR-*).
- Faster P25 and DMR searches: a quiet step takes one radio poll (25 ms) instead of two in P25 search (430-440 MHz:
  about 20 s a pass instead of 40) and three instead of four in DMR search (60 s instead of 80); a strong reading is
  checked again before the frequency is probed, and a busy FM channel in the range is probed less often (30 s apart
  once it has been checked twice). **Not tried on the radio yet** (rows SF-*).
- Multi-watch with 3 to 6 channels goes round up to three times faster: a quiet channel is left after 50 ms (DMR
  100 ms) instead of the whole 150 ms dwell, so 6 FM channels take 0.3 s a round (was 0.9 s). A P25 channel with a
  signal that isn't ours (FM on its frequency, another NAC) no longer freezes the ring: it is given back after 1.2 s
  and passed over while the signal lasts. Dual watch (2 channels) is unchanged. **Not tried on the radio yet** (rows
  MR-*).

## v1.0.0-beta.3 (2026-11-20)

Installed and checked on the author's radio with the new Windows programmer (C#), the first install it did.

- Frequency on Home: a channel's receive frequency on a row of its own ("RX 438.5000  TX 430.9000", or one
  frequency when both are the same); Large text shows it as a third line (TX in red while transmitting). Not in the
  VFO, whose large line is the frequency.
- DOTA Transfer (Tools > DOTA Transfer): send one channel, selected channels, a zone, or every zone ("DOTA Clone") with
  the DMR contacts and RX group lists they use to another DM-32UV running DDM32 Plus, over FM (AFSK 1200 with a CTCSS
  tone) or DMR (data to the receiver's ID). The receiver shows a four-digit code the sender types; nothing changes on
  the receiver unless every frame arrived (a stop, a power cut or a lost link leaves its codeplug as it was).
  Imported channels are receive only unless allowed; PTT during a transfer asks "Stop the transfer?". Low / Mid /
  High power, Low by default. **Not tried between two radios yet** (docs/plus-radio-test.md, rows D-*). Design and
  limits: docs/dota.md.

## v1.0.0-beta.2 (2026-11-20)

Second pre-release, the first as DDM32 Plus. Installed and checked on the author's radio (the fixes below and
the new boot screen).

- New boot screen: the author's radio-"D" mark and "DDM32+ Plus", readable on the dark and the light theme.
- Renamed from DDM25 Plus to DDM32 Plus (repository d0ukan/DDM32-Plus). v1.0.0-beta.1 was published as DDM25 Plus;
  its program cannot update itself to a later release (new file names): download the next program zip by hand once.
  Radios running a DDM25 Plus build are still recognized, and an existing `%LOCALAPPDATA%\DDM25Plus` data folder
  (backups, downloads, settings) keeps being used.
- Tools > Marine: duplex channels (18 on) no longer overlap the channel number.
- Boot screen in the saved theme (dark stayed light before).
- Map: APRS stations and repeaters larger, in their own colours; two closer zoom steps (~80 m and ~40 m a pixel).
- Large text: Home's two lines drawn up to twice as large on the DM-32UV's screen.

## v1.0.0-beta.1 (2026-11-20, published as DDM25 Plus)

First public pre-release. **Beta:** everything below is built and covered by host tests, but most of it has not been
field-tested on a radio yet.

### Base
- Classic r13 (Open25-DM32), split RX / TX NAC (upstream RFKV format), Turkish-first UI, DDM32 Plus name and boot
  screen, public build profile without the RadioID ID database.

### Modes
- P25: NAC in hex and decimal, Any NAC / talkgroup, split RX / TX NAC, emergency and unit-to-unit calls, P25 in zone
  scan (short calls caught), P25 search and trunk following, the CH_MODE 2 channels of the DM32-nxdn-p25 format read
  as P25.
- DMR: Any colour code / slot / talkgroup, Tier III trunk following, DMR search.
- Receive: YSF (DN and VW), NXDN48, dPMR, pager (POCSAG), ACARS, SSTV, airband AM, broadcast FM.
- YSF VW transmit (switched off until on-air tests).

### Listening
- Multi-watch 2-6 channels, each letter a channel or its own VFO; one-key multi-watch.
- Every mode in the VFO with all parameters (FM, DMR, P25, YSF, NXDN, dPMR, AM).
- Priority channel, temporary skip, heard list (tune the VFO from a heard call).

### Maps and position
- Offline Turkey map: 81 provinces, 973 districts, repeaters, APRS stations, track and coverage colours.
- 252 repeaters grouped by province, nearest first, auto repeater; airfields; marine VHF table (receive only).
- APRS messages and SOS; Go-To, find my friend (follow an APRS station), locator calculator, GPS dashboard.
- Satellites: passes, pass alarm, Doppler tracking.

### Tools
- Tone finder (CTCSS / DCS), DTMF / SelCall, CW decoder and practice, spectrum with waterfall, fox hunt, CW beacon,
  lone worker with an alarm action, one-key SOS (off by default), save search finds as channels, QSO marks, battery
  graph with time left, clock (stopwatch, countdown, alarm), MMDVM hotspot over the cable.

### PC programmer (Windows, no Python needed)
- Disclaimer to accept, port auto-detect, backup, plan, install (also from stock firmware with the radio's own codec),
  back to the previous version or stock from your own backup, signed updates, version list with downgrade warnings,
  self-update, live panel, channel viewer.
