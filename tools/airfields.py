#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""DM32 Plus Tools > Airfields (CONFIG_AIRFIELDS, docs/plus.md "Airfields"): the Turkish airfields of OurAirports
with their air band frequencies, as the compact C table integration/radiofoundry/air/airfield_data.c/.h (committed).

Source: OurAirports open data (https://ourairports.com/data/), the daily dumps its author publishes at
https://davidmegginson.github.io/ourairports-data/ (airports.csv, airport-frequencies.csv). Licence: the
ourairports-data repository is under the Unlicense (a public domain dedication; GitHub's licence field "Unlicense",
checked 2026-11-19) and ourairports.com/data describes the data as public domain with no guarantee of accuracy or
fitness for use (that page was not reachable on 2026-11-19: its TLS certificate had expired). Not for navigation:
frequencies change; the radio only listens.

Selection: iso_country TR; type large_airport and medium_airport always (an airfield without a frequency in the data
is listed with none), small_airport only with at least one usable frequency; heliports, closed fields, balloon and
seaplane bases never. Frequencies: the VHF aeronautical mobile band the radio receives in AM (118.000-136.990 MHz,
inside radio_am.h's 108-137 MHz), written as the channel name pilots use: on the 25 kHz grid or an 8.33 kHz channel
name (x.x05, x.x10, x.x15, x.x30, ... : the names ending 20, 45, 70, 95 do not exist). The tuned frequency is the
name's 8.33 kHz grid point (x.005 -> x.000, x.010 -> x.00833, x.015 -> x.01667; rounded to the Hz as radio_am_step
rounds it), so Up / Down on the radio's 8.33 kHz step stays on the same grid. Refused and counted: outside that band
(the military UHF air band, typing errors such as "25.125", a PMR 433 entry), off the grid, a duplicate. Types are
normalised (the last word, upper case: "Gokcen TWR" -> TWR, "Clearance" -> DEL, "APP/DEP" -> APP, ...).

Removed (EXCLUDE, owner decision 2026-11-20, docs/plus.md "Airfield table cleanup"): the entries of the data that are
not in the aerodrome's official publication (its AD 2.18 table), each by code and channel name with the reason. Only
removed: nothing is added, changed or reclassified from that publication (it is copyright). The list must match the
pinned data (an exclusion that matches nothing fails the run, so a refresh reviews it). A small airfield left without a
frequency is not listed, as the selection says.

Province (CONFIG_AIR_PROV, DDM32 Plus round 16: Tools > Airfields groups by it): the plate 1..81 (0 not known) of
each airfield from its position, by the same lookup the repeater list uses (tools/rpt_list.py Provinces: the map's own
province outlines, then a district centre within 10 km, the municipality naming a province, a district centre within
30 km). Checked against the data's own region (iso_region "TR-34": ISO 3166-2:TR, whose numbers are the plates):
where the two differ and a district centre of the region's province is within REGION_KM, the airfield is at a border
the simplified outlines do not follow there and the region wins (Sabiha Gokcen in Pendik is inside the Kocaeli
outline, Cukurova near Tarsus inside Adana's); farther, the position stays and the run reports it. One byte in what
was air_field_t's tail padding.

Reproducible: both inputs are pinned by SHA-256 (research-data/ourairports/, git-ignored; --unpinned for test
fixtures only); the output depends only on them and on the map's committed province tables. Hard cap: MAX_BYTES of
table. Untrusted input: read as data only; run with python -I.

  python3 -I tools/airfields.py                 # writes integration/radiofoundry/air/airfield_data.{c,h}
  python3 -I tools/airfields.py --check         # fails if the committed files differ
  python3 -I tools/airfields.py --list          # the table for review
"""
import argparse
import csv
import hashlib
import io
import math
import re
import sys
from decimal import Decimal, InvalidOperation
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import turkey_map  # noqa: E402  (encode_name, c_string, ascii_fold)
sys.path.insert(0, str(ROOT))
from tools.rpt_list import Provinces  # noqa: E402  (the repeater list's province lookup: the map's outlines)

SOURCE_URL = 'https://davidmegginson.github.io/ourairports-data/'
SOURCE_DATE = '2026-11-19'
AIRPORTS_SHA256 = '73b5022692ba9739ba61b66e1f82ea1410597991c64a9274c5d40e3753b72664'
FREQS_SHA256 = '910718536bbe60ec3ab300bed8208367d4e4165dd5ab0f606ede52b9a86be623'
LICENCE = 'Unlicense (public domain dedication)'
ATTRIBUTION = 'Airfield data: OurAirports (public domain)'
DATA_DIR = ROOT / 'research-data' / 'ourairports'
OUT_DIR = ROOT / 'integration' / 'radiofoundry' / 'air'

CODE_MAX = 7            # airfield.h AIR_CODE_MAX
NAME_MAX = 22           # airfield.h AIR_NAME_MAX
FREQ_MAX = 64           # frequencies kept for one airfield (air_field_t.count is a uint8_t)
FIELD_BYTES = 44        # sizeof(air_field_t)
FREQ_BYTES = 4          # sizeof(air_freq_t)
REGION_KM = 30          # the data's region wins over the outline within this of a district centre of its province
MAX_BYTES = 12 * 1024   # a hard cap on the table (docs/plus.md: the job's flash budget)
LO_KHZ, HI_KHZ = 118000, 137000     # the VHF aeronautical mobile band (hi exclusive)
CH_BASE_KHZ = 108000    # air_freq_t.ch = (name - CH_BASE_KHZ) / 5

KINDS = {'large_airport': 0, 'medium_airport': 1, 'small_airport': 2}

# Entries of the pinned data that are not in the aerodrome's official publication: (code, channel name, reason).
# Removed only; the list matches the pinned data exactly (main() fails on one that matches nothing)
NOT_PUBLISHED = "not in the aerodrome's official publication"
UNOFFICIAL = 'not in an official publication: unofficial (no published aerodrome)'
EXCLUDE = (
    ('LTAB', '118.675', NOT_PUBLISHED),
    ('LTAD', '119.100', NOT_PUBLISHED + ": another unit's (Esenboga approach)"),
    ('LTAE', '119.100', NOT_PUBLISHED + ": another unit's (Esenboga approach)"),
    ('LTAF', '126.500', NOT_PUBLISHED),
    ('LTAG', '121.950', NOT_PUBLISHED),
    ('LTAG', '123.025', NOT_PUBLISHED),
    ('LTAG', '126.500', NOT_PUBLISHED),
    ('LTAG', '128.000', NOT_PUBLISHED),
    ('LTAG', '131.900', NOT_PUBLISHED),
    ('LTAI', '122.050', NOT_PUBLISHED),
    ('LTAI', '123.175', NOT_PUBLISHED),
    ('LTAI', '131.450', NOT_PUBLISHED),
    ('LTAJ', '120.100', NOT_PUBLISHED),
    ('LTAN', '131.425', NOT_PUBLISHED),
    ('LTAP', '121.950', NOT_PUBLISHED),
    ('LTAR', '118.100', NOT_PUBLISHED),
    ('LTAT', '121.950', NOT_PUBLISHED),
    ('LTAT', '128.350', NOT_PUBLISHED),
    ('LTAV', '121.950', NOT_PUBLISHED),
    ('LTAY', '121.600', NOT_PUBLISHED),
    ('LTAZ', '119.000', NOT_PUBLISHED),
    ('LTAZ', '119.100', NOT_PUBLISHED),
    ('LTBA', '121.700', NOT_PUBLISHED),
    ('LTBA', '121.800', NOT_PUBLISHED),
    ('LTBA', '121.900', NOT_PUBLISHED),
    ('LTBH', '122.100', NOT_PUBLISHED),
    ('LTBI', '121.950', NOT_PUBLISHED),
    ('LTBJ', '119.500', NOT_PUBLISHED),
    ('LTBK', '120.100', NOT_PUBLISHED),
    ('LTBL', '120.100', NOT_PUBLISHED),
    ('LTBL', '121.950', NOT_PUBLISHED),
    ('LTBL', '122.425', NOT_PUBLISHED),
    ('LTBO', '121.425', NOT_PUBLISHED),
    ('LTBO', '122.350', NOT_PUBLISHED),
    ('LTBP', '121.950', NOT_PUBLISHED),
    ('LTBQ', '120.500', NOT_PUBLISHED),
    ('LTBQ', '121.950', NOT_PUBLISHED),
    ('LTBR', '120.325', NOT_PUBLISHED),
    ('LTBR', '121.950', NOT_PUBLISHED),
    ('LTBR', '122.100', NOT_PUBLISHED),
    ('LTBU', '120.500', NOT_PUBLISHED),
    ('LTBV', '126.000', NOT_PUBLISHED),
    ('LTCA', '118.100', NOT_PUBLISHED),
    ('LTCC', '122.425', NOT_PUBLISHED),
    ('LTCC', '123.200', NOT_PUBLISHED),
    ('LTCE', '118.100', NOT_PUBLISHED),
    ('LTCE', '121.950', NOT_PUBLISHED),
    ('LTCG', '120.100', NOT_PUBLISHED),
    ('LTCI', '118.100', NOT_PUBLISHED),
    ('LTCJ', '121.950', NOT_PUBLISHED),
    ('LTCK', '121.950', NOT_PUBLISHED),
    ('LTCT', '126.000', NOT_PUBLISHED),
    ('LTFA', '120.100', NOT_PUBLISHED),
    ('LTFA', '121.950', NOT_PUBLISHED),
    ('LTFE', '119.775', NOT_PUBLISHED),
    ('LTFE', '121.950', NOT_PUBLISHED),
    ('LTFJ', '128.200', NOT_PUBLISHED),
    ('LTFJ', '130.250', NOT_PUBLISHED),
    ('TR-0018', '122.100', UNOFFICIAL),
    ('TR-0018', '123.300', UNOFFICIAL),
    ('TR-0040', '128.025', "not in an official publication for this field: another unit's (Antalya approach)"),
)
# airfield.h air_type_t, in display order within an airfield
TYPES = ['ATIS', 'DEL', 'GND', 'RMP', 'TWR', 'APP', 'DEP', 'AFIS', 'INFO', 'CTAF', 'PAR', 'SAR', 'EMR', 'OPS', 'OTH']
TYPE_WORDS = {
    'ATIS': 'ATIS', 'D-ATIS': 'ATIS', 'D-ATIS-ARR': 'ATIS', 'D-ATIS-DEP': 'ATIS',
    'DEL': 'DEL', 'DLV': 'DEL', 'CLD': 'DEL', 'CLR': 'DEL', 'CLNC': 'DEL', 'CLEARANCE': 'DEL', 'DELIVERY': 'DEL',
    'GND': 'GND', 'GROUND': 'GND', 'RMP': 'RMP', 'RAMP': 'RMP', 'APRON': 'RMP',
    'TWR': 'TWR', 'TOWER': 'TWR',
    'APP': 'APP', 'APPROACH': 'APP', 'APP/DEP': 'APP', 'ACP': 'APP', 'ARR': 'APP', 'DEP': 'DEP', 'DEPARTURE': 'DEP',
    'AFIS': 'AFIS', 'INFO': 'INFO', 'FIS': 'INFO', 'CTAF': 'CTAF', 'UNIC': 'CTAF', 'UNICOM': 'CTAF',
    'PAR': 'PAR', 'SAR': 'SAR', 'EMR': 'EMR', 'EMERG': 'EMR',
    'HAVAS': 'OPS', 'OPS': 'OPS', 'POST': 'OPS', 'PMSV': 'OPS', 'PTD': 'OPS', 'SFA': 'OPS', 'TAFR': 'OPS',
}
# The radio's ISO-8859-9 fonts (CONFIG_LANG_TR) have no a, i, u with a circumflex
ALLOWED = re.compile(r"^[0-9A-Za-zÇĞİÖŞÜçğıöşü' ./()-]+$")
SUFFIXES = [(' International Airport', ''), (' Army Air Base', ' AB'), (' Air Base', ' AB'), (' Airbase', ' AB'),
            (' Firefighting Airport', ''), (' Sport Airfield', ''), (' Airfield', ''), (' Airport', '')]


class AirError(Exception):
    pass


def short_name(name):
    s = name.split(' / ')[0].strip()
    s = s.replace('–', '-').replace('—', '-')
    s = s.translate(str.maketrans('âÂîÎûÛ', 'aAiIuU'))
    for suffix, repl in SUFFIXES:
        if s.endswith(suffix):
            s = s[:-len(suffix)] + repl
            break
    s = ' '.join(s.split())
    if len(s) > NAME_MAX:     # whole words while they fit
        words, out = s.split(' '), ''
        for w in words:
            if len(out) + len(w) + (1 if out else 0) > NAME_MAX:
                break
            out = (out + ' ' + w) if out else w
        s = out or s[:NAME_MAX]
    if not ALLOWED.match(s):
        raise AirError('name %r has a letter the radio cannot show' % s)
    return s


def norm_type(t):
    words = (t or '').strip().upper().split()
    if not words:
        return 'OTH'
    for w in (words[-1], words[0], ' '.join(words)):
        if w in TYPE_WORDS:
            return TYPE_WORDS[w]
    return 'OTH'


def channel_khz(text):
    """A frequency field ("121.58", "132", "118.675") -> the channel name in kHz, or a refusal reason"""
    try:
        mhz = Decimal(text.strip())
    except (InvalidOperation, AttributeError):
        return None, 'not a number'
    khz = mhz * 1000
    if khz != khz.to_integral_value():
        return None, 'grid'
    khz = int(khz)
    if not LO_KHZ <= khz < HI_KHZ:
        return None, 'band'
    if khz % 5 != 0 or khz % 25 == 20:
        return None, 'grid'
    return khz, None


def tune_hz(khz):
    """The frequency a channel name tunes: its 25 kHz value, or its 8.33 kHz grid point (radio_am_step's rounding)"""
    r = khz % 25
    if r == 0:
        return khz * 1000
    n = (khz - r) * 3 // 25 + (r // 5 - 1)          # grid point number: n * 25000 / 3 Hz
    return (n * 25000 + 1) // 3


def read_csv(text):
    return list(csv.DictReader(io.StringIO(text)))


def field_code(r):
    return (r.get('icao_code') or r.get('gps_code') or r.get('ident') or '').strip().upper()


def exclusions(exclude):
    """EXCLUDE -> {(code, khz): reason}; each a channel name of the band, once, with a reason"""
    out = {}
    for code, name, reason in exclude:
        khz, why = channel_khz(name)
        if why or (code, khz) in out or not reason:
            raise AirError('exclusion %s %s: %s' % (code, name, why or 'twice or without a reason'))
        out[(code, khz)] = reason
    return out


def region_plate(text):
    """OurAirports' iso_region "TR-06" -> 6 (ISO 3166-2:TR numbers the provinces by plate), else 0"""
    m = re.match(r'^TR-(\d\d)$', (text or '').strip())
    return int(m.group(1)) if m and 1 <= int(m.group(1)) <= 81 else 0


def near_province(prov_of, lat_e5, lon_e5, plate, max_km):
    """A district centre of province plate within max_km (rpt_list.Provinces.district's distance)"""
    x, y = (lon_e5 - 2500000) / 100, (lat_e5 - 3550000) / 100
    c = math.cos(math.radians(lat_e5 / 1e5))
    lim = max_km / 0.11132     # 0.001 degree of latitude is 111.32 m
    return any(p == plate and math.hypot((dx - x) * c, dy - y) < lim for dx, dy, p in prov_of.districts)


def province(prov_of, lat_e5, lon_e5, r, code, stats):
    """(plate, how) of an airport row: the repeater list's lookup, the data's region at a border (module doc)"""
    plate, how = prov_of.of(lat_e5, lon_e5, r.get('municipality') or '')
    region = region_plate(r.get('iso_region'))
    if region and region != plate:
        if near_province(prov_of, lat_e5, lon_e5, region, REGION_KM):
            return region, 'region'
        stats['differ'].append((code, plate, region))
    return plate, how


def build(airports_text, freqs_text, exclude=EXCLUDE, provinces=None):
    """-> (fields, stats); fields sorted by code: dict(code, name, lat, lon, kind, prov, freqs=[(type, khz)]).
    stats['unused_exclusions']: the exclusions that matched nothing (main() refuses them for the pinned data);
    stats['provinces'] {plate: airfields} (0: not known), stats['how'] {how it was placed: airfields},
    stats['differ'] [(code, position's plate, region's plate)] far from the region's province (the position kept).
    provinces: a tools/rpt_list.py Provinces (None: the map's)"""
    stats = {'airfields': 0, 'frequencies': 0, 'refused': {}, 'dropped_over_max': 0, 'without_frequency': 0,
             'excluded': 0, 'provinces': {}, 'how': {}, 'differ': []}
    prov_of = provinces or Provinces()
    ex = exclusions(exclude)
    used = set()
    aps = {}
    for r in read_csv(airports_text):
        if r.get('iso_country') != 'TR' or r.get('type') not in KINDS:
            continue
        aps[r['id']] = r
    by_ap = {}
    for r in read_csv(freqs_text):
        if r.get('airport_ref') not in aps:
            continue
        khz, why = channel_khz(r.get('frequency_mhz', ''))
        if why:
            stats['refused'][why] = stats['refused'].get(why, 0) + 1
            continue
        key = (field_code(aps[r['airport_ref']]), khz)
        if key in ex:
            stats['excluded'] += 1
            used.add(key)
            continue
        t = norm_type(r.get('type'))
        lst = by_ap.setdefault(r['airport_ref'], [])
        if (t, khz) in lst:
            stats['refused']['duplicate'] = stats['refused'].get('duplicate', 0) + 1
            continue
        lst.append((t, khz))
    fields = []
    for ref, r in aps.items():
        fr = by_ap.get(ref, [])
        if r['type'] == 'small_airport' and not fr:
            continue
        code = field_code(r)
        if not code or len(code) > CODE_MAX or not re.match(r'^[A-Z0-9-]+$', code):
            raise AirError('airport %s: code %r' % (ref, code))
        try:
            lat, lon = float(r['latitude_deg']), float(r['longitude_deg'])
        except ValueError:
            raise AirError('%s: position %r %r' % (code, r['latitude_deg'], r['longitude_deg']))
        if not (35.0 < lat < 43.0 and 25.0 < lon < 46.0):
            raise AirError('%s: position %f %f outside Turkey' % (code, lat, lon))
        fr.sort(key=lambda tk: (TYPES.index(tk[0]), tk[1]))
        if len(fr) > FREQ_MAX:
            stats['dropped_over_max'] += len(fr) - FREQ_MAX
            fr = fr[:FREQ_MAX]
        if not fr:
            stats['without_frequency'] += 1
        plate, how = province(prov_of, round(lat * 1e5), round(lon * 1e5), r, code, stats)
        stats['provinces'][plate] = stats['provinces'].get(plate, 0) + 1
        stats['how'][how] = stats['how'].get(how, 0) + 1
        fields.append(dict(code=code, name=short_name(r['name']), lat=lat, lon=lon, kind=KINDS[r['type']], prov=plate,
                           freqs=fr))
    fields.sort(key=lambda f: f['code'])
    codes = [f['code'] for f in fields]
    if len(set(codes)) != len(codes):
        raise AirError('a code twice')
    stats['unused_exclusions'] = sorted('%s %d.%03d' % (c, k // 1000, k % 1000) for c, k in ex if (c, k) not in used)
    stats['airfields'] = len(fields)
    stats['frequencies'] = sum(len(f['freqs']) for f in fields)
    stats['bytes'] = len(fields) * FIELD_BYTES + stats['frequencies'] * FREQ_BYTES
    if stats['bytes'] > MAX_BYTES:
        raise AirError('table %d bytes > the %d byte cap' % (stats['bytes'], MAX_BYTES))
    if not fields or not stats['frequencies']:
        raise AirError('no airfield with a frequency: wrong input?')
    if len(fields) > 255 or stats['frequencies'] > 0xFFFF or FREQ_MAX > 255:
        raise AirError('too many airfields or frequencies for the table')
    return fields, stats


def generate(fields, stats, source):
    head = ('/* SPDX-License-Identifier: GPL-3.0-or-later */\n'
            '/* GENERATED by tools/airfields.py - do not edit. DM32 Plus Tools > Airfields (docs/plus.md).\n'
            ' * %s. Source: %s\n'
            ' * Not for navigation. Names iso-8859-9 */\n' % (ATTRIBUTION, source))
    h = [head, '#ifndef AIRFIELD_DATA_H\n#define AIRFIELD_DATA_H\n\n#include "ui/airfield.h"\n\n',
         '#define AIR_FIELDS\t%uu\n' % len(fields),
         '#define AIR_FREQS\t%uu\n' % stats['frequencies'],
         '#define AIR_ATTRIBUTION\t"%s"\n' % ATTRIBUTION,
         '#define AIR_SOURCE_DATE\t"%s"\n\n' % SOURCE_DATE,
         '/* Sorted by code; airfield i\'s frequencies are air_freqs[first .. first + count - 1] */\n',
         'extern const air_field_t air_fields[AIR_FIELDS];\n',
         'extern const air_freq_t air_freqs[AIR_FREQS];\n\n#endif\n']
    c = [head, '#include "config.h"\n#if CONFIG_AIRFIELDS\n#include "ui/airfield_data.h"\n\n',
         '/* { lat_e5, lon_e5, first, count, kind, code, name, province } */\n',
         'const air_field_t air_fields[AIR_FIELDS] = {\n']
    first = 0
    for f in fields:
        name = turkey_map.c_string(turkey_map.encode_name(f['name'], 'iso-8859-9'))
        c.append('\t{ %d, %d, %u, %u, %u, "%s", %s, %u },\t/* %s */\n'
                 % (round(f['lat'] * 1e5), round(f['lon'] * 1e5), first, len(f['freqs']), f['kind'], f['code'], name,
                    f['prov'], turkey_map.ascii_fold(f['name'])))
        first += len(f['freqs'])
    c.append('};\n/* %s */\n\nconst uint16_t air_field_count = AIR_FIELDS;\n\n'
             'const air_freq_t air_freqs[AIR_FREQS] = {\n' % prov_line(stats))
    for f in fields:
        if not f['freqs']:
            continue
        items = ['{ %u, AIR_%s }' % ((khz - CH_BASE_KHZ) // 5, t) for t, khz in f['freqs']]
        c.append('\t%s,\t/* %s */\n' % (', '.join(items), f['code']))
    c.append('};\n#endif\n')
    return ''.join(h), ''.join(c)


def prov_line(stats):
    """The table's comment line: '35 provinces (01 2, 06 4, ...), unknown 0; by district 4, outline 70'"""
    dist = stats['provinces']
    return '%d provinces (%s), unknown %d; by %s' % (
        sum(1 for p in dist if p), ', '.join('%02d %d' % kv for kv in sorted(dist.items()) if kv[0]), dist.get(0, 0),
        ', '.join('%s %d' % kv for kv in sorted(stats['how'].items())))


def load(path, sha, unpinned):
    raw = Path(path).read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    if not unpinned and digest != sha:
        raise AirError('%s has SHA-256 %s, expected %s (OurAirports of %s)' % (path, digest, sha, SOURCE_DATE))
    return raw.decode('utf-8')


def describe(stats):
    ref = ', '.join('%s %d' % kv for kv in sorted(stats['refused'].items())) or 'none'
    return ('Airfields: %d (%d without a frequency), %d frequencies, %d bytes; refused: %s; excluded: %d; over %d an '
            'airfield: %d; %d provinces, unknown %d, by region %d; position and region far apart: %s' % (
                stats['airfields'], stats['without_frequency'], stats['frequencies'], stats['bytes'], ref,
                stats['excluded'], FREQ_MAX, stats['dropped_over_max'], sum(1 for p in stats['provinces'] if p),
                stats['provinces'].get(0, 0), stats['how'].get('region', 0),
                ', '.join('%s %02d not %02d' % d for d in stats['differ']) or 'none'))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--airports', default=str(DATA_DIR / 'airports.csv'))
    ap.add_argument('--freqs', default=str(DATA_DIR / 'airport-frequencies.csv'))
    ap.add_argument('--out-dir', type=Path, default=OUT_DIR)
    ap.add_argument('--unpinned', action='store_true', help='skip the SHA-256 pins (test fixtures only)')
    ap.add_argument('--check', action='store_true', help='fail if the files on disk differ from the output')
    ap.add_argument('--list', action='store_true', help='print the table for review')
    args = ap.parse_args()
    try:
        fields, stats = build(load(args.airports, AIRPORTS_SHA256, args.unpinned),
                              load(args.freqs, FREQS_SHA256, args.unpinned))
    except (AirError, OSError) as e:
        sys.exit('airfields: %s' % e)
    if stats['unused_exclusions'] and not args.unpinned:
        sys.exit('airfields: exclusions that match nothing in the data: ' + ', '.join(stats['unused_exclusions']))
    source = ('OurAirports of %s, %s (airports.csv SHA-256 %s, airport-frequencies.csv SHA-256 %s)'
              % (SOURCE_DATE, SOURCE_URL, AIRPORTS_SHA256, FREQS_SHA256)) if not args.unpinned else 'test fixture'
    if args.list:
        for f in fields:
            print('%-7s %-22s %8.4f %8.4f %d %02d  %s' % (f['code'], f['name'], f['lat'], f['lon'], f['kind'], f['prov'],
                  ' '.join('%s %d.%03d' % (t, k // 1000, k % 1000) for t, k in f['freqs'])))
    h, c = generate(fields, stats, source)
    files = {args.out_dir / 'airfield_data.h': h, args.out_dir / 'airfield_data.c': c}
    if args.check:
        stale = [str(p) for p, t in files.items() if not p.exists() or p.read_bytes() != t.encode('utf-8')]
        if stale:
            sys.exit('airfields: out of date: ' + ', '.join(stale))
    else:
        args.out_dir.mkdir(parents=True, exist_ok=True)
        for p, t in files.items():
            p.write_bytes(t.encode('utf-8'))
    print(describe(stats), file=sys.stderr)


if __name__ == '__main__':
    main()
