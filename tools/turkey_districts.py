#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""DM32 Plus offline Turkey map, districts (docs/map.md): converts the GeoNames country dump for Turkey (TR.zip,
CC BY 4.0) into the compact C table integration/radiofoundry/map/turkey_district_data.c/.h: the 973 districts (ilce)
as a point (the district centre GeoNames gives) and a name, grouped by province (plate order).

Selection: feature class A, code ADM2. GeoNames' admin1 codes are not the plate numbers (Ankara is 68, Adana 81);
they are matched to the 81 provinces by name. Names: the " İlçesi" / " District" / " Merkez" suffixes go, the central
district of a province without its own name is called after the province ("Kilis Merkez" -> "Kilis"), a few
ASCII-only or historic spellings are corrected to the official name (OVERRIDES), and a, i, u with circumflex are folded
(the radio's fonts have no glyphs for them). Every province must have exactly its official number of districts
(DISTRICTS_PER_PROVINCE, 973 in total), so a changed dump fails loudly instead of drifting.

Reproducible: the input archive is pinned by SHA-256; the output depends only on the input and the options.
Untrusted input: the archive is only read as data; run with python -I.

    python -I tools/turkey_districts.py --zip research-data/geonames/TR.zip [--charset iso-8859-9] [--check]
"""
import argparse
import hashlib
import io
import re
import sys
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import turkey_map  # noqa: E402  (the province names, the units and the name encoders)

SOURCE_URL = 'https://download.geonames.org/export/dump/TR.zip'
SOURCE_DATE = '2026-11-19'		# downloaded; GeoNames rebuilds the dump daily, so the pin is that day's file
SOURCE_SHA256 = '2df01e05aa64b678be9ae0733efe35cca36a407b249dd4432f0915085a442848'
LICENCE = 'CC BY 4.0'
ATTRIBUTION = 'District data: GeoNames (CC BY 4.0)'

UNIT, LON0, LAT0 = turkey_map.UNIT, turkey_map.LON0, turkey_map.LAT0
MAX_BYTES = 20 * 1024			# the district table alone
MAP_BUDGET = 60 * 1024			# provinces (turkey_map_data) + districts together: a hard cap

ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / 'integration/radiofoundry/map'

# Official number of districts by plate (Ministry of Interior, 973 since 2018: Derecik, Kemalpaşa (Artvin) and
# Sultanhanı were the last ones made)
DISTRICTS_PER_PROVINCE = [
    15, 9, 18, 8, 7, 25, 19, 9, 17, 20, 8, 8, 7, 9, 11, 17, 12, 12, 14, 19, 17, 9, 11, 9, 20, 14, 9, 16, 6, 5,
    15, 13, 13, 39, 30, 8, 20, 16, 8, 7, 12, 31, 13, 13, 17, 11, 10, 13, 6, 8, 6, 19, 12, 16, 17, 7, 9, 17, 11, 12,
    18, 8, 13, 6, 13, 14, 8, 8, 3, 6, 9, 6, 7, 4, 6, 4, 6, 6, 4, 7, 8,
]
assert len(DISTRICTS_PER_PROVINCE) == 81 and sum(DISTRICTS_PER_PROVINCE) == 973

# ADM2 records that are not districts
EXCLUDE = {
    8694443: 'Doski Aşireti Bölgesi (Hakkari): a tribal area, not a district',
}

# geonameid -> the official name, where GeoNames has an ASCII-only, historic or alternative spelling
OVERRIDES = {
    7932392: 'Cihanbeyli',		# "Cihanbeyli District"
    7733228: 'Ağın',
    7910465: 'Ulukışla',		# "Ulukisla"
    7910468: 'Yeşilhisar',		# "Yesilhisar"
    7910469: 'İncesu',			# "Incesu"
    7910611: 'Beypazarı',		# "Beypazari"
    7932395: 'Yahyalı',			# "Yahyali"
    7732933: 'Selçuklu',		# "Selcuklu"
    7911042: 'Muradiye',		# "Muradiye / Berkri"
    8630849: 'Şemdinli',		# "Şemdinni"
}
# Whole-name corrections (whatever the id): official spellings
RENAME = {
    'Şebin Karahisar': 'Şebinkarahisar',
    'Ondokuzmayıs': '19 Mayıs',
}

# The letters a name may use after folding (the radio's ISO-8859-9 fonts with CONFIG_LANG_TR)
ALLOWED = re.compile(r"^[0-9A-Za-zÇĞİÖŞÜçğıöşü' .-]+$")


def fold_name(s):
    return s.translate(str.maketrans('âÂîÎûÛ', 'aAiIuU'))


def ascii_key(s):
    return turkey_map.ascii_fold(fold_name(s)).lower().replace('ı', 'i')


def province_key(s):
    s = ascii_key(s)
    return s[:-len(' province')] if s.endswith(' province') else s


def load(zip_path):
    raw = Path(zip_path).read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    if digest != SOURCE_SHA256:
        sys.exit('turkey_districts: %s has SHA-256 %s, expected %s (GeoNames TR.zip of %s)'
                 % (zip_path, digest, SOURCE_SHA256, SOURCE_DATE))
    text = zipfile.ZipFile(io.BytesIO(raw)).read('TR.txt').decode('utf-8')
    rows = [ln.split('\t') for ln in text.split('\n') if ln]
    if any(len(r) != 19 for r in rows):
        sys.exit('turkey_districts: TR.txt is not the 19 column GeoNames format')

    # admin1 code -> plate, by the province's name
    plate_by_name = {province_key(n): i + 1 for i, n in enumerate(turkey_map.NAMES)}
    plate_of = {}
    for r in rows:
        if r[6] == 'A' and r[7] == 'ADM1':
            k = province_key(r[2] or r[1])
            if k not in plate_by_name:
                k = province_key(r[1])
            if k not in plate_by_name or r[10] in plate_of:
                sys.exit('turkey_districts: ADM1 %s %r does not name one of the 81 provinces' % (r[10], r[1]))
            plate_of[r[10]] = plate_by_name[k]
    if sorted(plate_of.values()) != list(range(1, 82)):
        sys.exit('turkey_districts: expected 81 provinces, got %d' % len(plate_of))

    out = {p: [] for p in range(1, 82)}
    for r in rows:
        if r[6] != 'A' or r[7] != 'ADM2':
            continue
        gid = int(r[0])
        if gid in EXCLUDE:
            continue
        if r[10] not in plate_of:
            sys.exit('turkey_districts: %d %r has admin1 code %r' % (gid, r[1], r[10]))
        plate = plate_of[r[10]]
        name = OVERRIDES.get(gid, r[1])
        for suffix in (' İlçesi', ' District', ' Merkez'):
            if name.endswith(suffix):
                name = name[:-len(suffix)]
        name = RENAME.get(name, name)
        # The central district named after its province: the province's official spelling
        if ascii_key(name) == ascii_key(turkey_map.NAMES[plate - 1]):
            name = turkey_map.NAMES[plate - 1]
        name = fold_name(name)
        if not ALLOWED.match(name) or len(name) > 20:
            sys.exit('turkey_districts: %d: name %r' % (gid, name))
        lat, lon = float(r[4]), float(r[5])
        if not (35.5 < lat < 42.5 and 25.5 < lon < 45.0):
            sys.exit('turkey_districts: %d %s: position %f %f outside Turkey' % (gid, name, lat, lon))
        out[plate].append((name, lat, lon, gid))
    for plate in range(1, 82):
        got, want = len(out[plate]), DISTRICTS_PER_PROVINCE[plate - 1]
        names = [d[0] for d in out[plate]]
        if got != want:
            sys.exit('turkey_districts: TR-%02d %s has %d districts, officially %d: %s'
                     % (plate, turkey_map.NAMES[plate - 1], got, want, ', '.join(sorted(names))))
        if len(set(names)) != len(names):
            sys.exit('turkey_districts: TR-%02d has a name twice' % plate)
        out[plate].sort(key=lambda d: (ascii_key(d[0]), d[0]))
    return out


def map_table_bytes(header):
    """The size of the province table (turkey_map_data.c), from its header, for the shared budget"""
    text = header.read_text(encoding='utf-8')

    def num(pattern):
        m = re.search(pattern, text)
        if not m:
            sys.exit('turkey_districts: %s: %s not found' % (header, pattern))
        return int(m.group(1))
    return (num(r'#define TURKEY_MAP_PROVINCES\t(\d+)u') * 18 + num(r'#define TURKEY_MAP_RINGS\t(\d+)u') * 8 +
            num(r'#define TURKEY_MAP_STEPS\t(\d+)u') * 2 + num(r'turkey_map_names\[(\d+)\]'))


def generate(districts, args):
    pos, first, name_off, names = [], [], [], bytearray()
    for plate in range(1, 82):
        first.append(len(pos))
        name_off.append(len(names))
        for name, lat, lon, _ in districts[plate]:
            pos.append((int(round((lon - LON0) / UNIT)), int(round((lat - LAT0) / UNIT)), name))
            names += turkey_map.encode_name(name, args.charset) + b'\0'
    first.append(len(pos))
    if len(names) > 0xFFFF:
        sys.exit('turkey_districts: names too long for the offsets')
    size = len(pos) * 4 + len(first) * 2 + len(name_off) * 2 + len(names)
    prov_h = args.out_dir / 'turkey_map_data.h'
    provinces = map_table_bytes(prov_h if prov_h.exists() else OUT_DIR / 'turkey_map_data.h')
    sizes = dict(districts=len(pos), names=len(names), bytes=size, map_total=size + provinces)
    if size > MAX_BYTES:
        sys.exit('turkey_districts: %d bytes > %d' % (size, MAX_BYTES))
    if size + provinces > MAP_BUDGET:
        sys.exit('turkey_districts: map data %d + %d bytes > the %d byte budget' % (provinces, size, MAP_BUDGET))

    head = ('/* SPDX-License-Identifier: GPL-3.0-or-later */\n'
            '/* GENERATED by tools/turkey_districts.py - do not edit. DM32 Plus offline Turkey map (docs/map.md).\n'
            ' * %s. Source: GeoNames TR.zip of %s, %s (feature class A, ADM2)\n'
            ' * SHA-256 %s; names %s */\n' % (ATTRIBUTION, SOURCE_DATE, SOURCE_URL, SOURCE_SHA256, args.charset))
    h = [head,
         '#ifndef TURKEY_DISTRICT_DATA_H\n#define TURKEY_DISTRICT_DATA_H\n\n#include <stdint.h>\n'
         '#include "turkey_map_data.h"\n\n',
         '#define TURKEY_DISTRICTS\t%uu\n' % len(pos),
         '#define TURKEY_DISTRICT_CHARSET\t"%s"\t/* of the names */\n' % args.charset,
         '#define TURKEY_DISTRICT_ATTRIBUTION\t"%s"\n\n' % ATTRIBUTION,
         '/* District d: its centre (units as turkey_map_data.h); the districts of province p (plate - 1) are\n'
         ' * first[p] .. first[p + 1] - 1, sorted by name, their names NUL separated from names + name_first[p] */\n',
         'extern const turkey_map_pt_t turkey_district_pos[TURKEY_DISTRICTS];\n',
         'extern const uint16_t turkey_district_first[TURKEY_MAP_PROVINCES + 1u];\n',
         'extern const uint16_t turkey_district_name_first[TURKEY_MAP_PROVINCES];\n',
         'extern const char turkey_district_names[%u];\n\n#endif\n' % len(names)]
    c = [head, '#include "config.h"\n#if CONFIG_MAP\n#include "turkey_district_data.h"\n\n',
         'const turkey_map_pt_t turkey_district_pos[TURKEY_DISTRICTS] = {\n']
    for plate in range(1, 82):
        c.append('\t/* %02d %s */\n' % (plate, turkey_map.ascii_fold(turkey_map.NAMES[plate - 1])))
        row = pos[first[plate - 1]:first[plate]]
        for i in range(0, len(row), 6):
            c.append('\t' + ' '.join('{ %d, %d },' % (x, y) for x, y, _ in row[i:i + 6]) + '\n')
    c.append('};\n\nconst uint16_t turkey_district_first[TURKEY_MAP_PROVINCES + 1u] = {\n')
    for i in range(0, len(first), 12):
        c.append('\t' + ' '.join('%u,' % v for v in first[i:i + 12]) + '\n')
    c.append('};\n\nconst uint16_t turkey_district_name_first[TURKEY_MAP_PROVINCES] = {\n')
    for i in range(0, len(name_off), 12):
        c.append('\t' + ' '.join('%u,' % v for v in name_off[i:i + 12]) + '\n')
    c.append('};\n\n/* NUL separated; the last name ends with the literal\'s own NUL */\n'
             'const char turkey_district_names[%u] =\n' % len(names))
    parts = names.split(b'\0')[:-1]
    for i in range(0, len(parts), 6):
        chunk = parts[i:i + 6]
        last = i + 6 >= len(parts)
        text = ' "\\0" '.join(turkey_map.c_string(p) for p in chunk)
        c.append('\t%s%s\n' % (text, ';' if last else ' "\\0"'))
    c.append('#endif\n')
    return ''.join(h), ''.join(c), sizes


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--zip', required=True, help='GeoNames TR.zip of %s' % SOURCE_DATE)
    ap.add_argument('--charset', choices=('utf-8', 'iso-8859-9', 'ascii'), default='iso-8859-9',
                    help='names: UTF-8, the radio\'s ISO-8859-9 font slots (CONFIG_LANG_TR) or folded ASCII')
    ap.add_argument('--out-dir', type=Path, default=OUT_DIR)
    ap.add_argument('--check', action='store_true', help='fail if the files on disk differ from the output')
    ap.add_argument('--list', action='store_true', help='print plate, name, lat, lon, geonameid for review')
    args = ap.parse_args()
    districts = load(args.zip)
    if args.list:
        for plate in range(1, 82):
            for name, lat, lon, gid in districts[plate]:
                print('%02d\t%s\t%.5f\t%.5f\t%d' % (plate, name, lat, lon, gid))
    h, c, sizes = generate(districts, args)
    files = {args.out_dir / 'turkey_district_data.h': h, args.out_dir / 'turkey_district_data.c': c}
    if args.check:
        stale = [str(p) for p, t in files.items() if not p.exists() or p.read_bytes() != t.encode('utf-8')]
        if stale:
            sys.exit('turkey_districts: out of date: ' + ', '.join(stale))
    else:
        args.out_dir.mkdir(parents=True, exist_ok=True)
        for p, t in files.items():
            p.write_bytes(t.encode('utf-8'))
    print('turkey_districts: ' + ', '.join('%s %d' % kv for kv in sizes.items()), file=sys.stderr)


if __name__ == '__main__':
    main()
