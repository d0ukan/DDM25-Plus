#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""DM32 Plus satellites: two-line elements and FM transponders to a C table (CONFIG_SATELLITES, docs/plus.md).

Sources (integration/radiofoundry/sat/):
  tle.txt       standard three-line element sets (a name line, line 1, line 2), as CelesTrak serves them (e.g. its
                amateur.txt as is). Lines starting with '#' are comments; the first one is shown on the radio as the
                elements' source. Checksums are verified.
  sat_freq.txt  the satellites the radio knows, one a line:
                  catalog-number  name  downlink-MHz  uplink-MHz  CTCSS-Hz  arm-tone-Hz
                (0 for no tone). Only these go in; element sets of other satellites are skipped.

  python3 tools/sat_tle.py             print the C table (sat_data.c)
  python3 tools/sat_tle.py -o FILE     write it
The firmware build (tools/plus_features.py, prepare_satellites) runs this every build, so refreshing the elements
is editing tle.txt and building again. Output is deterministic. Python stdlib only.
"""
import argparse
import math
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SAT_MAX = 8
NAME_LEN = 7        # the pass list's first column (sat.h holds up to SAT_NAME_LEN)


class TleError(Exception):
    pass


def checksum(line):
    return sum(int(c) if c.isdigit() else 1 if c == '-' else 0 for c in line[:68]) % 10


def implied(field):
    """A TLE number with an implied decimal point and exponent: ' 28098-4' = 0.28098e-4"""
    f = field.strip()
    if not f or f in ('00000-0', '00000+0', '-00000-0', '+00000-0'):
        return 0.0
    sign = -1.0 if f[0] == '-' else 1.0
    f = f.lstrip('+-')
    mant, exp = f[:-2], f[-2:]
    return sign * float('0.' + mant) * 10.0 ** int(exp)


def days2000(year):
    """Days from 2000-01-01 to 1 January of year (proleptic Gregorian, either side of 2000)"""
    days = 0
    leap = lambda y: (y % 4 == 0 and y % 100 != 0) or y % 400 == 0
    if year >= 2000:
        for y in range(2000, year):
            days += 366 if leap(y) else 365
    else:
        for y in range(year, 2000):
            days -= 366 if leap(y) else 365
    return days


def parse_tle(name, l1, l2):
    for n, line in ((1, l1), (2, l2)):
        if len(line) < 69 or line[0] != str(n) or line[1] != ' ':
            raise TleError(f'{name}: line {n} is not a TLE line')
        if int(line[68]) != checksum(line):
            raise TleError(f'{name}: line {n} checksum {line[68]}, should be {checksum(line)}')
    cat1, cat2 = l1[2:7].strip(), l2[2:7].strip()
    if cat1 != cat2:
        raise TleError(f'{name}: catalog numbers differ ({cat1}, {cat2})')
    yy = int(l1[18:20])
    year = 2000 + yy if yy < 57 else 1900 + yy
    doy = float(l1[20:32])
    n = float(l2[52:63])
    el = {
        'catnum': int(cat1),
        'epoch': days2000(year) * 86400.0 + (doy - 1.0) * 86400.0,
        'no': n * 2.0 * math.pi / 1440.0,
        'ecc': float('0.' + l2[26:33].strip()),
        'inc': math.radians(float(l2[8:16])),
        'node': math.radians(float(l2[17:25])),
        'argp': math.radians(float(l2[34:42])),
        'mo': math.radians(float(l2[43:51])),
        'bstar': implied(l1[53:61]),
        'period_min': 1440.0 / n,
    }
    if el['period_min'] >= 225.0:
        raise TleError(f'{name}: deep-space orbit (period {el["period_min"]:.0f} min): SGP4 near-earth only')
    return el


def read_tles(path):
    note, sets = '', {}
    lines = [l.rstrip('\r\n') for l in open(path, encoding='ascii')]
    i = 0
    while i < len(lines):
        line = lines[i]
        if not line.strip():
            i += 1
            continue
        if line.startswith('#'):
            if not note:
                note = line[1:].strip()
            i += 1
            continue
        if line.startswith('1 ') and i + 1 < len(lines):
            name, l1, l2 = '', line, lines[i + 1]
            i += 2
        elif i + 2 < len(lines):
            name, l1, l2 = line.strip(), lines[i + 1], lines[i + 2]
            if name.startswith('0 '):	# Space-Track's "0 NAME"
                name = name[2:].strip()
            i += 3
        else:
            raise TleError(f'{path}:{i + 1}: incomplete element set')
        el = parse_tle(name or l1[2:7], l1, l2)
        sets[el['catnum']] = el
    return note, sets


def read_freqs(path):
    rows = []
    for no, line in enumerate(open(path, encoding='ascii'), 1):
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        f = line.split()
        if len(f) != 6:
            raise TleError(f'{path}:{no}: want catalog name down up ctcss arm')
        cat, name, down, up, ctcss, arm = int(f[0]), f[1], float(f[2]), float(f[3]), float(f[4]), float(f[5])
        if len(name) > NAME_LEN or '"' in name or '\\' in name:
            raise TleError(f'{path}:{no}: name "{name}" (at most {NAME_LEN} plain characters)')
        rows.append((cat, name, round(down * 1e6), round(up * 1e6), round(ctcss * 10), round(arm * 10)))
    if len(rows) > SAT_MAX:
        raise TleError(f'{path}: {len(rows)} satellites, the radio holds {SAT_MAX}')
    return rows


def generate(tle_path, freq_path):
    note, sets = read_tles(tle_path)
    rows = read_freqs(freq_path)
    out = ['/* SPDX-License-Identifier: GPL-3.0-or-later */',
           '/* Generated by tools/sat_tle.py from integration/radiofoundry/sat/tle.txt and sat_freq.txt: do not edit. */',
           '#include "config.h"', '#if CONFIG_SATELLITES', '#include "radio/sat.h"', '',
           'const char sat_tle_note[] = "%s";' % note.replace('\\', '/').replace('"', "'"),
           'const sat_info_t sat_table[] = {']
    for cat, name, down, up, ctcss, arm in rows:
        if cat not in sets:
            raise TleError(f'{freq_path}: no element set for {name} ({cat}) in {tle_path}')
        e = sets[cat]
        nums = ', '.join('%.17g' % e[k] for k in ('epoch', 'no', 'ecc', 'inc', 'node', 'argp', 'mo', 'bstar'))
        out.append('\t{ "%s", %du, %du, %du, %d, %d,' % (name, cat, down, up, ctcss, arm))
        out.append('\t  { %s } },' % nums)
    out += ['};', 'const uint8_t sat_count = (uint8_t)(sizeof sat_table / sizeof sat_table[0]);',
            '_Static_assert(sizeof sat_table / sizeof sat_table[0] <= SAT_MAX, "SAT_MAX satellites at most");',
            '#endif', '']
    return '\n'.join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--tle', default=os.path.join(ROOT, 'integration', 'radiofoundry', 'sat', 'tle.txt'))
    ap.add_argument('--freq', default=os.path.join(ROOT, 'integration', 'radiofoundry', 'sat', 'sat_freq.txt'))
    ap.add_argument('-o', '--out')
    args = ap.parse_args()
    try:
        text = generate(args.tle, args.freq)
    except (TleError, ValueError) as e:
        sys.exit(f'sat_tle: {e}')
    if args.out:
        with open(args.out, 'w', encoding='ascii', newline='\n') as f:
            f.write(text)
    else:
        sys.stdout.write(text)


if __name__ == '__main__':
    main()
