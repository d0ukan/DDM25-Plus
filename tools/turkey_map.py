#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""DM32 Plus offline Turkey map (docs/map.md): converts the Natural Earth 1:10m admin-1 states/provinces shapefile
(public domain) into the compact C table integration/radiofoundry/map/turkey_map_data.c/.h.

The 81 provinces (il) are selected by ISO 3166-2 code TR-01 .. TR-81 (the licence plate number). Every border shared
by two provinces is split at its junctions and simplified once (Douglas-Peucker), so neighbours keep identical
borders without gaps. Coordinates are quantised to 0.001 degree units from a fixed origin; each ring is a start
point plus int8 (dx, dy) steps (long edges are split into several steps), closed back onto its start.

Reproducible: the input archive is pinned by SHA-256; the output depends only on the input and the options.
Untrusted input: the archive is only read as data (zip, dBASE III, ESRI shapefile parsed here); run with python -I.

    python -I tools/turkey_map.py --zip ne_10m_admin_1_states_provinces.zip [--tolerance 0.002] [--check]
"""
import argparse
import hashlib
import io
import math
import struct
import sys
import zipfile
from pathlib import Path

SOURCE_URL = 'https://naciscdn.org/naturalearth/10m/cultural/ne_10m_admin_1_states_provinces.zip'
SOURCE_VERSION = '5.1.1'
SOURCE_SHA256 = 'efc59726337323058f9446210adc96673179cd344e053666ee3d28cb58ba2b05'
BASE = 'ne_10m_admin_1_states_provinces'

UNIT = 0.001			# degrees per coordinate unit
LON0, LAT0 = 25.0, 35.5		# the origin of the units (south-west of Turkey)
MAX_BYTES = 40 * 1024		# flash budget for the whole table

ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / 'integration/radiofoundry/map'

# Official province names by plate code (the generator checks Natural Earth's name_tr against them)
NAMES = [
    'Adana', 'Adıyaman', 'Afyonkarahisar', 'Ağrı', 'Amasya', 'Ankara', 'Antalya', 'Artvin', 'Aydın', 'Balıkesir',
    'Bilecik', 'Bingöl', 'Bitlis', 'Bolu', 'Burdur', 'Bursa', 'Çanakkale', 'Çankırı', 'Çorum', 'Denizli',
    'Diyarbakır', 'Edirne', 'Elazığ', 'Erzincan', 'Erzurum', 'Eskişehir', 'Gaziantep', 'Giresun', 'Gümüşhane',
    'Hakkari', 'Hatay', 'Isparta', 'Mersin', 'İstanbul', 'İzmir', 'Kars', 'Kastamonu', 'Kayseri', 'Kırklareli',
    'Kırşehir', 'Kocaeli', 'Konya', 'Kütahya', 'Malatya', 'Manisa', 'Kahramanmaraş', 'Mardin', 'Muğla', 'Muş',
    'Nevşehir', 'Niğde', 'Ordu', 'Rize', 'Sakarya', 'Samsun', 'Siirt', 'Sinop', 'Sivas', 'Tekirdağ', 'Tokat',
    'Trabzon', 'Tunceli', 'Şanlıurfa', 'Uşak', 'Van', 'Yozgat', 'Zonguldak', 'Aksaray', 'Bayburt', 'Karaman',
    'Kırıkkale', 'Batman', 'Şırnak', 'Bartın', 'Ardahan', 'Iğdır', 'Yalova', 'Karabük', 'Kilis', 'Osmaniye', 'Düzce',
]
assert len(NAMES) == 81


# ---------------------------------------------------------------------------------------------------------------
# Input: zip -> dBASE records and shapefile polygons

def read_dbf(data):
    count, hlen, rlen = struct.unpack('<IHH', data[4:12])
    fields, off = [], 32
    while data[off] != 0x0D:
        name = data[off:off + 11].split(b'\0')[0].decode('ascii')
        fields.append((name, data[off + 16]))
        off += 32
    rows = []
    for r in range(count):
        rec = data[hlen + r * rlen:hlen + (r + 1) * rlen]
        p, row = 1, {}
        for name, ln in fields:
            row[name] = rec[p:p + ln].split(b'\0')[0].decode('utf-8', 'replace').strip()
            p += ln
        rows.append(row)
    return rows


def read_polygon(shp, shx, index):
    off = struct.unpack('>I', shx[100 + 8 * index:104 + 8 * index])[0] * 2
    content = off + 8
    shape = struct.unpack('<i', shp[content:content + 4])[0]
    if shape not in (5, 15, 25):
        raise ValueError('record %d: shape type %d is not a polygon' % (index, shape))
    nparts, npoints = struct.unpack('<ii', shp[content + 36:content + 44])
    parts = list(struct.unpack('<%di' % nparts, shp[content + 44:content + 44 + 4 * nparts]))
    pts_off = content + 44 + 4 * nparts
    pts = [struct.unpack('<dd', shp[pts_off + 16 * i:pts_off + 16 * i + 16]) for i in range(npoints)]
    parts.append(npoints)
    return [pts[parts[i]:parts[i + 1]] for i in range(nparts)]


def load(zip_path):
    raw = Path(zip_path).read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    if digest != SOURCE_SHA256:
        sys.exit('turkey_map: %s has SHA-256 %s, expected %s (Natural Earth %s)' % (zip_path, digest, SOURCE_SHA256,
                                                                                  SOURCE_VERSION))
    z = zipfile.ZipFile(io.BytesIO(raw))
    version = z.read(BASE + '.VERSION.txt').decode('ascii').strip()
    if version != SOURCE_VERSION:
        sys.exit('turkey_map: VERSION.txt says %r' % version)
    rows = read_dbf(z.read(BASE + '.dbf'))
    shp, shx = z.read(BASE + '.shp'), z.read(BASE + '.shx')
    provinces = {}
    for i, row in enumerate(rows):
        if row.get('adm0_a3') != 'TUR':
            continue
        code = row['iso_3166_2']
        if not code.startswith('TR-'):
            sys.exit('turkey_map: unexpected code %r' % code)
        plate = int(code[3:])
        if plate in provinces or not 1 <= plate <= 81:
            sys.exit('turkey_map: bad or duplicate plate %d' % plate)
        if row.get('name_tr') and row['name_tr'] != NAMES[plate - 1]:
            print('turkey_map: note: TR-%02d name_tr %a, using %a' % (plate, row['name_tr'], NAMES[plate - 1]),
                  file=sys.stderr)
        provinces[plate] = read_polygon(shp, shx, i)
    if sorted(provinces) != list(range(1, 82)):
        sys.exit('turkey_map: expected TR-01..TR-81, got %d provinces' % len(provinces))
    return provinces


# ---------------------------------------------------------------------------------------------------------------
# Geometry

def quantise(ring):
    out = []
    for lon, lat in ring:
        p = (int(round((lon - LON0) / UNIT)), int(round((lat - LAT0) / UNIT)))
        if not out or out[-1] != p:
            out.append(p)
    if len(out) > 1 and out[0] == out[-1]:
        out.pop()
    return out		# open ring


def area2(ring):
    s = 0
    for i in range(len(ring)):
        x0, y0 = ring[i - 1]
        x1, y1 = ring[i]
        s += x0 * y1 - x1 * y0
    return s		# twice the signed area, units^2


def seg_dist2(p, a, b):
    ax, ay = a
    dx, dy = b[0] - ax, b[1] - ay
    px, py = p[0] - ax, p[1] - ay
    ll = dx * dx + dy * dy
    if ll == 0:
        return px * px + py * py
    t = max(0.0, min(1.0, (px * dx + py * dy) / ll))
    ex, ey = px - t * dx, py - t * dy
    return ex * ex + ey * ey


def douglas_peucker(pts, tol):
    keep = [False] * len(pts)
    keep[0] = keep[-1] = True
    stack = [(0, len(pts) - 1)]
    tol2 = tol * tol
    while stack:
        a, b = stack.pop()
        best, bi = -1.0, -1
        for i in range(a + 1, b):
            d = seg_dist2(pts[i], pts[a], pts[b])
            if d > best:
                best, bi = d, i
        if bi >= 0 and best > tol2:
            keep[bi] = True
            stack.append((a, bi))
            stack.append((bi, b))
    return [p for p, k in zip(pts, keep) if k]


def simplify(provinces, tol_units, min_island_units2):
    # Rings kept per province: the largest part always, the others when big enough
    rings = {}
    for plate, parts in provinces.items():
        q = [r for r in (quantise(p) for p in parts) if len(r) >= 3]
        q.sort(key=lambda r: -abs(area2(r)))
        rings[plate] = [q[0]] + [r for r in q[1:] if abs(area2(r)) / 2 >= min_island_units2]

    # Owners of each vertex, then junctions: where the set of owners changes along a ring
    owners = {}
    for plate, rs in rings.items():
        for r in rs:
            for p in r:
                owners.setdefault(p, set()).add(plate)
    anchors = set()
    for rs in rings.values():
        for r in rs:
            n = len(r)
            for i in range(n):
                o = owners[r[i]]
                if o != owners[r[i - 1]] or o != owners[r[(i + 1) % n]]:
                    anchors.add(r[i])
    shared = sum(1 for o in owners.values() if len(o) > 1)

    cache = {}

    def chain(seq):
        rev = seq[::-1]
        if rev < seq:
            key = tuple(rev)
            if key not in cache:
                cache[key] = douglas_peucker(rev, tol_units)
            return cache[key][::-1]
        key = tuple(seq)
        if key not in cache:
            cache[key] = douglas_peucker(seq, tol_units)
        return cache[key]

    out = {}
    for plate in sorted(rings):
        res = []
        for r in rings[plate]:
            n = len(r)
            idx = [i for i in range(n) if r[i] in anchors]
            if len(idx) < 2:		# an island or a ring with one junction: fix two far apart vertices
                a = min(range(n), key=lambda i: r[i])
                b = max(range(n), key=lambda i: ((r[i][0] - r[a][0]) ** 2 + (r[i][1] - r[a][1]) ** 2, -i))
                idx = sorted(set(idx) | {a, b})
            simp = []
            for k in range(len(idx)):
                i, j = idx[k], idx[(k + 1) % len(idx)]
                seq = r[i:j + 1] if j > i else r[i:] + r[:j + 1]
                simp.extend(chain(seq)[:-1])
            if len(simp) < 3:
                continue
            if area2(simp) < 0:		# store counter-clockwise (x east, y north)
                simp = simp[::-1]
            res.append(simp)
        if not res:
            sys.exit('turkey_map: TR-%02d vanished; lower the tolerance' % plate)
        out[plate] = res
    return out, shared, len(owners)


def label_point(rings):
    """A point inside the province: the centroid if inside, else the middle of the widest inside run on the
    centroid's row (scanline at a half unit so vertices are never hit)."""
    big = max(rings, key=lambda r: abs(area2(r)))
    a = area2(big)
    cx = sum((big[i - 1][0] + big[i][0]) * (big[i - 1][0] * big[i][1] - big[i][0] * big[i - 1][1])
             for i in range(len(big))) / (3.0 * a)
    cy = sum((big[i - 1][1] + big[i][1]) * (big[i - 1][0] * big[i][1] - big[i][0] * big[i - 1][1])
             for i in range(len(big))) / (3.0 * a)
    y = math.floor(cy) + 0.5
    xs = []
    for r in rings:
        for i in range(len(r)):
            (x0, y0), (x1, y1) = r[i - 1], r[i]
            if (y0 > y) != (y1 > y):
                xs.append(x0 + (y - y0) * (x1 - x0) / (y1 - y0))
    xs.sort()
    runs = [(xs[i], xs[i + 1]) for i in range(0, len(xs) - 1, 2)]
    for lo, hi in runs:
        if lo <= cx <= hi:
            return int(round(cx)), int(math.floor(cy))
    lo, hi = max(runs, key=lambda t: (t[1] - t[0], -t[0]))
    return int(round((lo + hi) / 2)), int(math.floor(cy))


def encode_ring(ring):
    """Start point plus int8 steps back onto the start (closed)."""
    steps = []
    pts = ring + [ring[0]]
    for i in range(1, len(pts)):
        dx, dy = pts[i][0] - pts[i - 1][0], pts[i][1] - pts[i - 1][1]
        n = max(1, -(-max(abs(dx), abs(dy)) // 127))
        px = py = 0
        for k in range(1, n + 1):
            nx, ny = dx * k // n if dx >= 0 else -((-dx) * k // n), dy * k // n if dy >= 0 else -((-dy) * k // n)
            steps.append((nx - px, ny - py))
            px, py = nx, ny
    return steps


# ---------------------------------------------------------------------------------------------------------------
# Output

def ascii_fold(s):
    """Turkish letters to their ASCII base (for C comments and fallback fonts)."""
    return s.translate(str.maketrans('çğıİöşüÇĞÖŞÜâÂ', 'cgiIosuCGOSUaA'))


def encode_name(s, charset):
    """utf-8 (default), iso-8859-9 (the radio's one byte a character fonts with CONFIG_LANG_TR, see
    tools/turkish_ui.py) or ascii (Turkish letters folded, for builds without the Turkish glyphs)."""
    if charset == 'ascii':
        return ascii_fold(s).encode('ascii')
    return s.encode(charset)


def c_string(raw):
    out = []
    for b in raw:
        out.append(chr(b) if 0x20 <= b < 0x7F and chr(b) not in '"\\?' else '\\%03o' % b)
    return '"' + ''.join(out) + '"'


def generate(provinces, args):
    tol = args.tolerance / UNIT
    island = args.min_island / (UNIT * UNIT)
    simp, shared, total = simplify(provinces, tol, island)

    ring_rows, deltas, prov_rows, names = [], [], [], bytearray()
    xmin = ymin = 1 << 30
    xmax = ymax = -(1 << 30)
    for plate in range(1, 82):
        rings = simp[plate]
        bx0 = min(p[0] for r in rings for p in r)
        by0 = min(p[1] for r in rings for p in r)
        bx1 = max(p[0] for r in rings for p in r)
        by1 = max(p[1] for r in rings for p in r)
        xmin, ymin, xmax, ymax = min(xmin, bx0), min(ymin, by0), max(xmax, bx1), max(ymax, by1)
        first = len(ring_rows)
        for r in rings:
            st = encode_ring(r)
            ring_rows.append((r[0][0], r[0][1], len(deltas), len(st)))
            deltas.extend(st)
        lx, ly = label_point(rings)
        prov_rows.append((first, len(rings), plate, len(names), lx, ly, bx0, by0, bx1, by1))
        names += encode_name(NAMES[plate - 1], args.charset) + b'\0'
    if len(ring_rows) > 0xFFFF or len(deltas) > 0xFFFF or max(r[1] for r in prov_rows) > 255:
        sys.exit('turkey_map: table too large for the index types')

    size = len(prov_rows) * 18 + len(ring_rows) * 8 + len(deltas) * 2 + len(names)
    sizes = dict(provinces=81, rings=len(ring_rows), points=len(deltas), names=len(names), bytes=size,
                 shared_vertices=shared, source_vertices=total)
    if size > MAX_BYTES:
        sys.exit('turkey_map: %d bytes > %d; raise --tolerance' % (size, MAX_BYTES))

    opts = 'tolerance %.4f deg, min island %.4f deg^2, names %s' % (args.tolerance, args.min_island, args.charset)
    head = ('/* SPDX-License-Identifier: GPL-3.0-or-later */\n'
            '/* GENERATED by tools/turkey_map.py - do not edit. DM32 Plus offline Turkey map (docs/map.md).\n'
            ' * Source: Natural Earth %s 1:10m admin-1 states/provinces (public domain), %s\n'
            ' * SHA-256 %s; %s */\n' % (SOURCE_VERSION, SOURCE_URL, SOURCE_SHA256, opts))
    h = [head,
         '#ifndef TURKEY_MAP_DATA_H\n#define TURKEY_MAP_DATA_H\n\n#include <stdint.h>\n\n',
         '#define TURKEY_MAP_PROVINCES\t81u\n',
         '#define TURKEY_MAP_RINGS\t%uu\n' % len(ring_rows),
         '#define TURKEY_MAP_STEPS\t%uu\n' % len(deltas),
         '#define TURKEY_MAP_CHARSET\t"%s"\t/* of the names */\n' % args.charset,
         '#define TURKEY_MAP_UNIT_E7\t%d\t/* one coordinate unit: 0.001 degree */\n' % int(round(UNIT * 1e7)),
         '#define TURKEY_MAP_LON0_E7\t%d\t/* unit (0, 0) */\n' % int(round(LON0 * 1e7)),
         '#define TURKEY_MAP_LAT0_E7\t%d\n' % int(round(LAT0 * 1e7)),
         '#define TURKEY_MAP_XMIN\t\t%d\t/* bounds of all rings, units */\n' % xmin,
         '#define TURKEY_MAP_YMIN\t\t%d\n' % ymin,
         '#define TURKEY_MAP_XMAX\t\t%d\n' % xmax,
         '#define TURKEY_MAP_YMAX\t\t%d\n\n' % ymax,
         'typedef struct {\n\tint16_t x, y;\t/* units east / north of the origin */\n} turkey_map_pt_t;\n\n',
         '/* One closed ring: the start point, then steps[first .. first + count) as (dx, dy) int8 pairs; the last\n'
         ' * step returns to the start. Counter-clockwise. */\n',
         'typedef struct {\n\tturkey_map_pt_t start;\n\tuint16_t first, count;\n} turkey_map_ring_t;\n\n',
         'typedef struct {\n\tuint16_t first_ring;\n\tuint8_t ring_count;\n\tuint8_t plate;\t\t/* 1..81 */\n'
         '\tuint16_t name;\t\t/* offset in turkey_map_names: NUL terminated (TURKEY_MAP_CHARSET) */\n'
         '\tturkey_map_pt_t label;\t/* inside the province */\n\tturkey_map_pt_t min, max;\t/* bounding box */\n'
         '} turkey_map_province_t;\n\n',
         'extern const turkey_map_province_t turkey_map_provinces[TURKEY_MAP_PROVINCES];\t/* by plate - 1 */\n',
         'extern const turkey_map_ring_t turkey_map_rings[TURKEY_MAP_RINGS];\n',
         'extern const int8_t turkey_map_steps[TURKEY_MAP_STEPS][2];\n',
         'extern const char turkey_map_names[%u];\n\n#endif\n' % len(names)]

    c = [head, '#include "config.h"\n#if CONFIG_MAP\n#include "turkey_map_data.h"\n\n',
         'const turkey_map_province_t turkey_map_provinces[TURKEY_MAP_PROVINCES] = {\n']
    for (first, cnt, plate, noff, lx, ly, bx0, by0, bx1, by1) in prov_rows:
        c.append('\t{ %u, %u, %u, %u, { %d, %d }, { %d, %d }, { %d, %d } },\t/* %s */\n'
                 % (first, cnt, plate, noff, lx, ly, bx0, by0, bx1, by1,
                    ascii_fold(NAMES[plate - 1])))
    c.append('};\n\nconst turkey_map_ring_t turkey_map_rings[TURKEY_MAP_RINGS] = {\n')
    for (x, y, first, cnt) in ring_rows:
        c.append('\t{ { %d, %d }, %u, %u },\n' % (x, y, first, cnt))
    c.append('};\n\nconst int8_t turkey_map_steps[TURKEY_MAP_STEPS][2] = {\n')
    for i in range(0, len(deltas), 8):
        c.append('\t' + ' '.join('{ %d, %d },' % d for d in deltas[i:i + 8]) + '\n')
    c.append('};\n\n/* NUL separated; the last name ends with the literal\'s own NUL */\n'
             'const char turkey_map_names[%u] =\n' % len(names))
    parts = names.split(b'\0')[:-1]
    for i, p in enumerate(parts):
        last = i == len(parts) - 1
        c.append('\t%s%s\n' % (c_string(p), ';' if last else ' "\\0"'))
    c.append('#endif\n')
    return ''.join(h), ''.join(c), sizes


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--zip', required=True, help='ne_10m_admin_1_states_provinces.zip (Natural Earth %s)'
                    % SOURCE_VERSION)
    ap.add_argument('--tolerance', type=float, default=0.002, help='Douglas-Peucker tolerance, degrees')
    ap.add_argument('--min-island', type=float, default=0.01, help='drop islands smaller than this, degrees^2')
    ap.add_argument('--charset', choices=('utf-8', 'iso-8859-9', 'ascii'), default='utf-8',
                    help='province names: UTF-8, the radio\'s ISO-8859-9 font slots (CONFIG_LANG_TR) or folded ASCII')
    ap.add_argument('--out-dir', type=Path, default=OUT_DIR)
    ap.add_argument('--check', action='store_true', help='fail if the files on disk differ from the output')
    args = ap.parse_args()
    h, c, sizes = generate(load(args.zip), args)
    files = {args.out_dir / 'turkey_map_data.h': h, args.out_dir / 'turkey_map_data.c': c}
    if args.check:
        stale = [str(p) for p, t in files.items() if not p.exists() or p.read_bytes() != t.encode('utf-8')]
        if stale:
            sys.exit('turkey_map: out of date: ' + ', '.join(stale))
    else:
        args.out_dir.mkdir(parents=True, exist_ok=True)
        for p, t in files.items():
            p.write_bytes(t.encode('utf-8'))
    print('turkey_map: ' + ', '.join('%s %d' % kv for kv in sizes.items()))


if __name__ == '__main__':
    main()
