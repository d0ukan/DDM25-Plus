# Changelog

DDM32 Plus is a fork of Open25-DM32 Classic (r13) for the Baofeng DM-32UV. See the disclaimer in
[README.md](README.md) before installing anything.

## Unreleased

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
