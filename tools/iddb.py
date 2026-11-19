#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""DM32 Plus DMR ID database: RadioID.net's user list to a compact C table in program flash (CONFIG_ID_DB,
docs/plus.md "DMR ID database").

Input: RadioID.net's DMR user database as it serves it (https://radioid.net/static/user.csv, columns
RADIO_ID,CALLSIGN,FIRST_NAME,LAST_NAME,CITY,STATE,COUNTRY). It is not committed: download it into
research-data/user.csv (git-ignored) before a release build, like the satellites' elements. Without it the build
uses integration/radiofoundry/iddb/sample.csv (a few fictional IDs) and says so.

  python3 tools/iddb.py CSV                         Turkey's IDs as C on stdout
  python3 tools/iddb.py CSV -o FILE --country Turkey --country Cyprus --prefix TA
  python3 tools/iddb.py CSV --stats                 counts and bytes only

Selection: rows whose COUNTRY matches a --country (case and accents ignored; "Turkey" also matches RadioID's
"Turkiye") or whose CALLSIGN starts with a --prefix; neither given = Turkey.
Per ID: the callsign (up to 6 letters / digits, base-37 in 32 bits; longer ones are skipped and counted) and the
first word of the first name (else of the last name), folded to ASCII (Turkish letters too: some of the radio's
font sizes have no Latin-1 glyphs) and cut to 10 characters. The first row of an ID wins; IDs above 24 bits are
skipped.

Layout (iddb.h): records sorted by ID in blocks of IDDB_BLOCK; an index (first ID, byte offset) per block is
binary searched, then at most IDDB_BLOCK records are walked. A record: the ID's distance from the previous one
(LEB128, 1 byte for dense IDs; 0 for a block's first), the packed callsign (4 bytes LE), the name's number (2 bytes
LE) in a table of distinct names (16-bit offsets into one NUL-separated string). Output is deterministic.
Python stdlib only.
"""
import argparse
import csv
import datetime
import io
import os
import sys
import unicodedata

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SAMPLE = os.path.join(ROOT, 'integration', 'radiofoundry', 'iddb', 'sample.csv')
BLOCK = 64              # iddb.h IDDB_BLOCK
NAME_MAX = 10           # iddb.h IDDB_NAME_MAX
CALL_MAX = 6
ID_MAX = 0xFFFFFF
MAX_BYTES = 120 * 1024  # the flash this feature may take (docs/plus.md)
ALPHABET = ' ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789'   # base-37 digits, ' ' pads on the right
COUNTRY_ALIASES = {'turkey': 'turkiye'}
SPECIAL = {'ı': 'i', 'İ': 'I', 'ß': 'ss', 'ø': 'o', 'Ø': 'O', 'æ': 'ae', 'Æ': 'AE', 'đ': 'd', 'Đ': 'D',
           'ł': 'l', 'Ł': 'L', 'œ': 'oe', 'Œ': 'OE', 'þ': 'th', 'Þ': 'Th', 'ð': 'd', 'Ð': 'D'}


class IddbError(Exception):
    pass


def fold(text):
    """ASCII only: Turkish dotless/dotted i first (Python's own lower() makes 'i̇' of 'İ'), then accents off"""
    text = ''.join(SPECIAL.get(c, c) for c in text)
    text = unicodedata.normalize('NFKD', text)
    return ''.join(c for c in text if ord(c) < 128 and not unicodedata.combining(c))


def country_key(name):
    k = fold(name).strip().lower()
    return COUNTRY_ALIASES.get(k, k)


def short_name(first, last):
    """The first word of the first name (else of the last name), ASCII, letters / digits / ' - . only, at most
    NAME_MAX characters; an all-upper or all-lower name in title case ("MEHMET" -> "Mehmet")"""
    for field in (first, last):
        words = ''.join(c if c.isalnum() or c in "'-." else ' ' for c in fold(field)).split()
        if words:
            w = words[0][:NAME_MAX]
            if w.isupper() or w.islower():
                w = w[:1].upper() + w[1:].lower()
            return w
    return ''


def pack_call(call):
    """Base-37 of up to 6 characters (A-Z, 0-9), padded with ' ' on the right; None if it does not fit"""
    call = call.strip().upper()
    if not call or len(call) > CALL_MAX or any(c not in ALPHABET[1:] for c in call):
        return None
    v = 0
    for c in call.ljust(CALL_MAX):
        v = v * 37 + ALPHABET.index(c)
    return v


def unpack_call(v):
    out = []
    for _ in range(CALL_MAX):
        out.append(ALPHABET[v % 37])
        v //= 37
    return ''.join(reversed(out)).rstrip()


def leb128(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def read(path, countries=(), prefixes=()):
    """[(id, callsign, name)] sorted by ID, and what was skipped"""
    want = {country_key(c) for c in countries}
    prefixes = tuple(p.strip().upper() for p in prefixes)
    if not want and not prefixes:
        want = {country_key('Turkey')}
    with open(path, 'rb') as f:
        raw = f.read()
    text = raw.decode('utf-8-sig', errors='replace')
    rows = {}
    skipped = {'callsign': 0, 'id': 0, 'duplicate': 0}
    reader = csv.DictReader(io.StringIO(text))
    need = {'RADIO_ID', 'CALLSIGN', 'FIRST_NAME', 'COUNTRY'}
    if reader.fieldnames is None or not need <= set(reader.fieldnames):
        raise IddbError(f'{path}: not a RadioID.net user.csv (needs columns {", ".join(sorted(need))})')
    for r in reader:
        call = (r.get('CALLSIGN') or '').strip().upper()
        if not (country_key(r.get('COUNTRY') or '') in want or (prefixes and call.startswith(prefixes))):
            continue
        try:
            rid = int((r.get('RADIO_ID') or '').strip())
        except ValueError:
            skipped['id'] += 1
            continue
        if rid <= 0 or rid > ID_MAX:
            skipped['id'] += 1
            continue
        packed = pack_call(call)
        if packed is None:
            skipped['callsign'] += 1
            continue
        if rid in rows:
            skipped['duplicate'] += 1
            continue
        rows[rid] = (call, packed, short_name(r.get('FIRST_NAME') or '', r.get('LAST_NAME') or ''))
    return [(rid, *rows[rid]) for rid in sorted(rows)], skipped


def encode(entries):
    """The tables: index [(first id, offset)], record bytes, names (sorted), name offsets"""
    names = sorted({e[3] for e in entries})
    if len(names) > 0xFFFF:
        raise IddbError(f'{len(names)} distinct names: more than 65535')
    number = {n: i for i, n in enumerate(names)}
    blob = bytearray()
    offsets = []
    for n in names:
        offsets.append(len(blob))
        blob += n.encode('ascii') + b'\0'
    if offsets and offsets[-1] > 0xFFFF:
        raise IddbError(f'names take {len(blob)} bytes: more than 16-bit offsets reach; select fewer IDs')
    index = []
    rec = bytearray()
    prev = 0
    for i, (rid, _call, packed, name) in enumerate(entries):
        if i % BLOCK == 0:
            index.append((rid, len(rec)))
            prev = rid
        rec += leb128(rid - prev)
        rec += packed.to_bytes(4, 'little')
        rec += number[name].to_bytes(2, 'little')
        prev = rid
    return index, bytes(rec), names, offsets, bytes(blob)


def size_of(index, rec, offsets, blob):
    return len(index) * 8 + len(rec) + len(offsets) * 2 + len(blob)


def c_bytes(data, indent='\t', per=16):
    return [indent + ''.join('0x%02x,' % b for b in data[i:i + per]) for i in range(0, len(data), per)]


def generate(path, countries=(), prefixes=(), date=None, sample=False, source=None, max_bytes=MAX_BYTES):
    entries, skipped = read(path, countries, prefixes)
    index, rec, names, offsets, blob = encode(entries)
    size = size_of(index, rec, offsets, blob)
    if size > max_bytes:
        raise IddbError(f'{len(entries)} IDs take {size} bytes, more than {max_bytes}: select fewer countries')
    if date is None:
        date = datetime.datetime.fromtimestamp(os.path.getmtime(path), datetime.timezone.utc).strftime('%Y-%m-%d')
    if source is None:
        source = 'sample' if sample else 'RadioID.net'
    sel = ', '.join([*countries, *('prefix ' + p for p in prefixes)]) or 'Turkey'
    out = ['/* SPDX-License-Identifier: GPL-3.0-or-later */',
           '/* Generated by tools/iddb.py from %s (%s, %s): do not edit.' % (
               'integration/radiofoundry/iddb/sample.csv' if sample else os.path.basename(path), sel, date),
           ' * DMR IDs, callsigns and first names: RadioID.net user database (radioid.net), unless "sample". */',
           '#include "config.h"', '#if CONFIG_ID_DB', '#include "radio/iddb.h"', '',
           '/* %d IDs, %d skipped (callsign %d, ID %d, duplicate %d), %d names, %d bytes */' % (
               len(entries), sum(skipped.values()), skipped['callsign'], skipped['id'], skipped['duplicate'],
               len(names), size),
           'const iddb_info_t iddb_info = { %du, %du, %du, %d, "%s", "%s" };' % (
               len(entries), len(index), len(rec), 1 if sample else 0, date, source.replace('"', "'")),
           'const iddb_block_t iddb_index[] = {']
    out += ['\t{ %du, %du },' % b for b in index] or ['\t{ 0u, 0u },']
    out += ['};', 'const uint8_t iddb_records[] = {']
    out += c_bytes(rec) or ['\t0']
    out += ['};', 'const uint16_t iddb_name_off[] = {']
    out += ['\t' + ' '.join('%du,' % o for o in offsets[i:i + 12]) for i in range(0, len(offsets), 12)] or ['\t0u']
    out += ['};', 'const char iddb_names[] =']
    # "\000": a full octal escape, so a name starting with a digit does not join it
    out += ['\t"%s"' % ''.join(n + '\\000' for n in names[i:i + 8]) for i in range(0, len(names), 8)] or ['\t""']
    out[-1] += ';'
    out += ['#endif', '']
    stats = {'ids': len(entries), 'skipped': skipped, 'names': len(names), 'blocks': len(index),
             'record_bytes': len(rec), 'name_bytes': len(blob) + 2 * len(offsets), 'index_bytes': 8 * len(index),
             'bytes': size, 'date': date}
    return '\n'.join(out), stats


def describe(stats, sample):
    return ('ID database: %s%d IDs, %d names, %d bytes (records %d, names %d, index %d), %s' % (
        'SAMPLE (no research-data/user.csv: fictional IDs) ' if sample else '', stats['ids'], stats['names'],
        stats['bytes'], stats['record_bytes'], stats['name_bytes'], stats['index_bytes'], stats['date']))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('csv', nargs='?', help='RadioID.net user.csv (default: the committed sample)')
    ap.add_argument('--country', action='append', default=[], help='a COUNTRY to take (repeatable; default Turkey)')
    ap.add_argument('--prefix', action='append', default=[], help='a callsign prefix to take (repeatable)')
    ap.add_argument('--date', help='the date shown on the radio (default: the CSV file\'s date, UTC)')
    ap.add_argument('--max-bytes', type=int, default=MAX_BYTES)
    ap.add_argument('--stats', action='store_true', help='print counts and bytes only')
    ap.add_argument('-o', '--out')
    args = ap.parse_args()
    sample = args.csv is None
    try:
        text, stats = generate(args.csv or SAMPLE, args.country, args.prefix, args.date or ('sample' if sample else None),
                               sample, max_bytes=args.max_bytes)
    except (IddbError, OSError) as e:
        sys.exit(f'iddb: {e}')
    print(describe(stats, sample), file=sys.stderr)
    if args.stats:
        return
    if args.out:
        with open(args.out, 'w', encoding='ascii', newline='\n') as f:
            f.write(text)
    else:
        sys.stdout.write(text)


if __name__ == '__main__':
    main()
