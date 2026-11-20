# Changelog

DDM25 Plus is a fork of Open25-DM32 Classic (r13) for the Baofeng DM-32UV. See the disclaimer in
[README.md](README.md) before installing anything.

## v1.0.0-beta.1 (unreleased)

First public pre-release. **Beta:** everything below is built and covered by host tests, but most of it has not been
field-tested on a radio yet.

### Base
- Classic r13 (Open25-DM32), split RX / TX NAC (upstream RFKV format), Turkish-first UI, DDM25 Plus name and boot
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
