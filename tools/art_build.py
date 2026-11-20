#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""DM32 Plus art: text pixel sources (art/) to C tables and PNG previews (docs/art.md).

Sources (plain text, one character a pixel):
  art/icons.txt     16x16 icons, 1 bit a pixel ('#' ink, '.' none), each after a line "icon <name>"
  art/mark.txt      the splash mark, 2 bits a pixel ('.' none, 'a' 'b' 'c' its three colors)
  art/wordmark.txt  "DDM32 Plus" in a pixel face, 1 bit a pixel; columns from "split" on take the accent color
Lines starting with ';' are comments.

  python3 tools/art_build.py                 write integration/radiofoundry/art/art_data.[ch]
  python3 tools/art_build.py --check         fail if those files are not what the sources give
  python3 tools/art_build.py --preview DIR   also write art/preview.png, mock-splash.png, mock-tools.png using
                                             the firmware fonts (jbmono_8x16/9x18/12x24.inc) found in DIR

Output is deterministic (no dates, sorted nothing: source order is the enum order). Python stdlib only.
"""
import argparse
import os
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ART = os.path.join(ROOT, 'art')
OUT = os.path.join(ROOT, 'integration', 'radiofoundry', 'art')
ICON = 16


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def rgb565_to_888(c):
    r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
    return (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)


# --- sources ---

def _lines(path):
    with open(path, encoding='utf-8') as f:
        for n, raw in enumerate(f, 1):
            line = raw.rstrip('\r\n').rstrip()
            if line == '' or line.startswith(';'):
                continue
            yield n, line


def load_icons(path):
    icons, cur = [], None
    for n, line in _lines(path):
        if line.startswith('icon '):
            name = line[5:].strip()
            if not name.isidentifier() or name.lower() != name:
                raise SystemExit('%s:%d: icon name must be a lower-case identifier' % (path, n))
            if any(i[0] == name for i in icons):
                raise SystemExit('%s:%d: icon %s twice' % (path, n, name))
            cur = (name, [])
            icons.append(cur)
            continue
        if cur is None:
            raise SystemExit('%s:%d: pixels before the first "icon" line' % (path, n))
        if len(line) != ICON or set(line) - set('.#'):
            raise SystemExit('%s:%d: an icon row is %d of "." and "#"' % (path, n, ICON))
        cur[1].append([1 if ch == '#' else 0 for ch in line])
    for name, rows in icons:
        if len(rows) != ICON:
            raise SystemExit('%s: icon %s has %d rows, not %d' % (path, name, len(rows), ICON))
        if not any(any(r) for r in rows):
            raise SystemExit('%s: icon %s is empty' % (path, name))
    return icons


def _image(path, chars):
    """Header lines ("size W H", "color k R G B ink|paper|fg", "split X") then rows of the given characters.
    A color's role: ink or paper on a 1 bpp panel; fg: the theme's foreground on color panels (R G B is then only
    the preview's dark-theme stand-in) and ink on a 1 bpp panel."""
    meta, rows = {'color': {}}, []
    for n, line in _lines(path):
        word = line.split()
        if word[0] == 'size' and len(word) == 3:
            meta['size'] = (int(word[1]), int(word[2]))
        elif word[0] == 'split' and len(word) == 2:
            meta['split'] = int(word[1])
        elif word[0] == 'color' and len(word) == 6 and word[1] in chars[1:] and word[5] in ('ink', 'paper', 'fg'):
            meta['color'][word[1]] = (tuple(int(v) for v in word[2:5]), word[5] != 'paper', word[5] == 'fg')
        else:
            if set(line) - set(chars):
                raise SystemExit('%s:%d: unknown character in pixel row' % (path, n))
            rows.append([chars.index(ch) for ch in line])
    if 'size' not in meta:
        raise SystemExit('%s: no "size W H" line' % path)
    w, h = meta['size']
    if len(rows) != h or any(len(r) != w for r in rows):
        raise SystemExit('%s: pixel rows are not %d x %d' % (path, w, h))
    return meta, rows


def load_mark(path):
    meta, rows = _image(path, '.abc')
    if sorted(meta['color']) != ['a', 'b', 'c']:
        raise SystemExit('%s: needs "color a|b|c R G B ink|paper|fg" for each of a, b, c' % path)
    if meta['color']['a'][2]:
        raise SystemExit('%s: color a is the accent (Plus, the rule, the callsign): a fixed color, not fg' % path)
    return meta, rows


def load_wordmark(path):
    meta, rows = _image(path, '.#')
    if not 0 < meta.get('split', 0) < meta['size'][0]:
        raise SystemExit('%s: needs "split X" inside the width' % path)
    return meta, rows


# --- C output ---

def _hex_rows(vals, per_line, fmt):
    out = []
    for i in range(0, len(vals), per_line):
        out.append('\t' + ', '.join(fmt % v for v in vals[i:i + per_line]) + ',')
    return '\n'.join(out)


def pack_bits(rows, bpp):
    """Rows packed MSB first, each row padded to a whole byte."""
    out = []
    for r in rows:
        acc, nbits = 0, 0
        for v in r:
            acc = (acc << bpp) | v
            nbits += bpp
            if nbits == 8:
                out.append(acc)
                acc, nbits = 0, 0
        if nbits:
            out.append(acc << (8 - nbits))
    return out


def gen_c(icons, mark, word):
    (mmeta, mrows), (wmeta, wrows) = mark, word
    mw, mh = mmeta['size']
    ww, wh = wmeta['size']
    mbits = pack_bits(mrows, 2)
    wbits = pack_bits(wrows, 1)
    icon_bytes = len(icons) * ICON * 2
    total = icon_bytes + len(mbits) + 6 + 3 + len(wbits)
    h = []
    h.append('// SPDX-License-Identifier: GPL-3.0-or-later')
    h.append('// DM32 Plus art tables. Generated by tools/art_build.py from art/*.txt. Do not edit.')
    h.append('#ifndef PLUS_ART_DATA_H')
    h.append('#define PLUS_ART_DATA_H')
    h.append('')
    h.append('#include <stdint.h>')
    h.append('')
    h.append('#define ART_ICON_W\t%d' % ICON)
    h.append('#define ART_ICON_H\t%d' % ICON)
    h.append('')
    h.append('enum {')
    for name, _ in icons:
        h.append('\tART_ICON_%s,' % name.upper())
    h.append('\tART_ICON_COUNT')
    h.append('};')
    h.append('')
    h.append('// One row a uint16_t, bit 15 the leftmost pixel; set = ink')
    h.append('extern const uint16_t art_icon_bits[ART_ICON_COUNT][ART_ICON_H];')
    h.append('')
    h.append('// The splash mark: 2 bits a pixel, MSB first, rows of ART_MARK_STRIDE bytes; 0 none, 1..3 its colors')
    h.append('#define ART_MARK_W\t%d' % mw)
    h.append('#define ART_MARK_H\t%d' % mh)
    h.append('#define ART_MARK_STRIDE\t%d' % ((mw * 2 + 7) // 8))
    h.append('extern const uint8_t art_mark_bits[ART_MARK_H * ART_MARK_STRIDE];')
    h.append('extern const uint16_t art_mark_rgb[3];\t// RGB565 of colors 1..3')
    h.append('extern const uint8_t art_mark_ink[3];\t// on a 1 bpp panel: 1 ink, 0 paper')
    h.append('extern const uint8_t art_mark_fg[3];\t// 1: the theme\'s foreground on color panels (RGB unused)')
    h.append('')
    h.append('// The wordmark: 1 bit a pixel, MSB first, rows of ART_WORD_STRIDE bytes; columns from ART_WORD_SPLIT on')
    h.append('// take the accent color')
    h.append('#define ART_WORD_W\t%d' % ww)
    h.append('#define ART_WORD_H\t%d' % wh)
    h.append('#define ART_WORD_STRIDE\t%d' % ((ww + 7) // 8))
    h.append('#define ART_WORD_SPLIT\t%d' % wmeta['split'])
    h.append('extern const uint8_t art_word_bits[ART_WORD_H * ART_WORD_STRIDE];')
    h.append('')
    h.append('#define ART_DATA_BYTES\t%d\t// flash taken by the tables above' % total)
    h.append('')
    h.append('#endif')
    h.append('')

    c = []
    c.append('// SPDX-License-Identifier: GPL-3.0-or-later')
    c.append('// DM32 Plus art tables. Generated by tools/art_build.py from art/*.txt. Do not edit.')
    c.append('#include "art.h"	// ART_ENABLED: CONFIG_PLUS_ART')
    c.append('')
    c.append('#if ART_ENABLED')
    c.append('')
    c.append('const uint16_t art_icon_bits[ART_ICON_COUNT][ART_ICON_H] = {')
    for name, rows in icons:
        vals = [sum(v << (15 - i) for i, v in enumerate(r)) for r in rows]
        c.append('\t[ART_ICON_%s] = {' % name.upper())
        c.append(_hex_rows(vals, 8, '0x%04X').replace('\t', '\t\t', 1).replace('\n\t', '\n\t\t'))
        c.append('\t},')
    c.append('};')
    c.append('')
    c.append('const uint8_t art_mark_bits[ART_MARK_H * ART_MARK_STRIDE] = {')
    c.append(_hex_rows(mbits, (mw * 2 + 7) // 8, '0x%02X'))
    c.append('};')
    c.append('')
    cols = [mmeta['color'][k] for k in 'abc']
    c.append('const uint16_t art_mark_rgb[3] = { %s };' % ', '.join('0x%04X' % rgb565(*col[0]) for col in cols))
    c.append('const uint8_t art_mark_ink[3] = { %s };' % ', '.join('1' if col[1] else '0' for col in cols))
    c.append('const uint8_t art_mark_fg[3] = { %s };' % ', '.join('1' if col[2] else '0' for col in cols))
    c.append('')
    c.append('const uint8_t art_word_bits[ART_WORD_H * ART_WORD_STRIDE] = {')
    c.append(_hex_rows(wbits, (ww + 7) // 8, '0x%02X'))
    c.append('};')
    c.append('')
    c.append('#endif')
    c.append('')
    return '\n'.join(h), '\n'.join(c), total


# --- PNG previews ---

def write_png(path, w, h, px):
    """px: list of h rows of w (r, g, b) tuples."""
    raw = b''.join(b'\x00' + bytes(v for p in row for v in p) for row in px)

    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)
    data = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    data += chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b'')
    with open(path, 'wb') as f:
        f.write(data)


class Canvas:
    def __init__(self, w, h, bg):
        self.w, self.h = w, h
        self.px = [[bg] * w for _ in range(h)]

    def fill(self, x, y, w, h, c):
        for yy in range(max(0, y), min(self.h, y + h)):
            row = self.px[yy]
            for xx in range(max(0, x), min(self.w, x + w)):
                row[xx] = c

    def blend(self, x, y, c, a):
        if 0 <= x < self.w and 0 <= y < self.h:
            o = self.px[y][x]
            self.px[y][x] = tuple((c[i] * a + o[i] * (3 - a)) // 3 for i in range(3))

    def scaled(self, k):
        out = Canvas(self.w * k, self.h * k, (0, 0, 0))
        for y in range(self.h):
            row = [p for p in self.px[y] for _ in range(k)]
            for i in range(k):
                out.px[y * k + i] = list(row)
        return out

    def save(self, path):
        write_png(path, self.w, self.h, self.px)


class Font:
    """A firmware font from its .inc (tools/fontconv.py output): the ASCII glyphs of the first table."""

    def __init__(self, path):
        import re
        txt = open(path, encoding='utf-8').read()
        name = os.path.basename(path)[:-4].upper()
        num = lambda k: int(re.search(r'#define %s_%s (\w+)' % (name, k), txt).group(1).rstrip('u'), 0)
        self.w, self.h, self.bpp = num('W'), num('H'), num('BPP')
        start = txt.index('_bits[] = {')
        body = txt[start:txt.index('};', start)].split('\n')[1:]
        self.glyphs = []
        for line in body:
            if line.startswith('#') or '0x' not in line:
                continue
            vals = [int(v, 16) for v in re.findall(r'0x([0-9A-Fa-f]{2})', line.split('//')[0])]
            self.glyphs.append(vals)
            if len(self.glyphs) == 95:
                break

    def text(self, cv, x, y, s, color):
        rb = (self.w * self.bpp + 7) // 8
        mx = (1 << self.bpp) - 1
        for ch in s:
            o = ord(ch)
            if 0x20 <= o < 0x7F:
                g = self.glyphs[o - 0x20]
                top, rows = g[0], g[1]
                for r in range(rows):
                    bits = int.from_bytes(bytes(g[2 + r * rb:2 + (r + 1) * rb]), 'big')
                    for col in range(self.w):
                        v = (bits >> (rb * 8 - (col + 1) * self.bpp)) & mx
                        if v:
                            cv.blend(x + col, y + top + r, color, v * 3 // mx)
            x += self.w
        return x

    def width(self, s):
        return self.w * len(s)


def draw_icon(cv, rows, x, y, color, k=1):
    for r, row in enumerate(rows):
        for c, v in enumerate(row):
            if v:
                cv.fill(x + c * k, y + r * k, k, k, color)


def draw_mark(cv, mark, x, y, k, fg=None):
    meta, rows = mark
    cols = [fg if fg is not None and meta['color'][ch][2] else rgb565_to_888(rgb565(*meta['color'][ch][0]))
            for ch in 'abc']
    for r, row in enumerate(rows):
        for c, v in enumerate(row):
            if v:
                cv.fill(x + c * k, y + r * k, k, k, cols[v - 1])


def draw_word(cv, word, x, y, k, c1, c2):
    meta, rows = word
    for r, row in enumerate(rows):
        for c, v in enumerate(row):
            if v:
                cv.fill(x + c * k, y + r * k, k, k, c1 if c < meta['split'] else c2)


THEMES = {
    # ui_screens.c ui_theme_set: bg, fg, dim, line, focus, focus_fg; the splash accent is the mark's color a
    'dark': dict(bg=(0, 0, 0), fg=(255, 255, 255), dim=(168, 168, 168), line=(72, 72, 72), focus=(32, 96, 200),
                 hint_bg=(32, 32, 32), warn=(240, 168, 0)),
    'light': dict(bg=(255, 255, 255), fg=(0, 0, 0), dim=(88, 88, 88), line=(176, 176, 176), focus=(64, 128, 232),
                  hint_bg=(224, 224, 224), warn=(176, 96, 0)),
}


def q(c):
    return rgb565_to_888(rgb565(*c))


def splash_layout(mark, word, n_lines, line_h, foot_h, w=240, h=320):
    """Where splash_plus.c ui_draw_splash_plus puts things on a w x h panel (keep the two in step):
    scale, mark x/y, wordmark x/y, rule y, first line y, footer y, callsign y (the bottom line)."""
    mw, mh = mark[0]['size']
    ww, wh = word[0]['size']
    sw = lambda k: max(mw, ww) * k
    k = 2 if w >= sw(2) + 16 and h >= 200 else 1
    gap = 6 * k
    art_h = (mh + 6 + wh) * k
    call_y = h - foot_h
    foot_y = call_y - foot_h
    total = art_h + gap + 1
    while n_lines > 0 and total + gap + n_lines * line_h > foot_y - 2:
        n_lines -= 1
    if n_lines:
        total += gap + n_lines * line_h
    y = max(0, (foot_y - total) // 2)
    rule_y = y + art_h + gap
    return (k, w // 2 - mw * k // 2, y, w // 2 - ww * k // 2, y + (mh + 6) * k, rule_y, rule_y + 1 + gap,
            foot_y, call_y)


def mock_splash(mark, word, fonts, theme):
    # DM32 (240 x 320, CONFIG_FONT_BIG): tiny 8x16, medium 12x24 for the owner's lines
    t = {k: q(v) for k, v in THEMES[theme].items()}
    cv = Canvas(240, 320, t['bg'])
    lines = ['TA0XXX', 'P25 RX']
    lf, tiny = fonts.get('12x24', fonts['9x18']), fonts.get('8x16', fonts['9x18'])
    k, mx, my, wx, wy, rule_y, ly, foot_y, call_y = splash_layout(mark, word, len(lines), lf.h, tiny.h)
    draw_mark(cv, mark, mx, my, k, t['fg'])
    accent = q(mark[0]['color']['a'][0])
    draw_word(cv, word, wx, wy, k, t['fg'], accent)
    cv.fill(120 - 40, rule_y, 80, 1, accent)
    for line in lines:
        lf.text(cv, (240 - lf.width(line)) // 2, ly, line, t['fg'])
        ly += lf.h
    foot = 'DM-32UV  v1.0.0'	# board and release, as ui_draw_splash gets them
    tiny.text(cv, (240 - tiny.width(foot)) // 2, foot_y, foot, t['dim'])
    call = 'TA2DOU'	# art.h ART_CALLSIGN
    tiny.text(cv, (240 - tiny.width(call)) // 2, call_y, call, accent)
    return cv


TOOLS = [('scan', 'Scan', 'Off'), ('position', 'Position', ''), ('hotspot', 'Hotspot', 'Off'),
         ('usb_net', 'USB network', 'Serial'), ('sfr', 'Single-freq repeater', ''), ('roam', 'Roaming', 'Off'),
         ('diag', 'Diagnostics', ''), ('aprs', 'APRS', ''), ('spectrum', 'Spectrum', ''), ('fox', 'Fox hunt', ''),
         ('beacon', 'CW beacon', ''), ('lone', 'Lone worker', ''), ('almanac', 'Sun & Moon', ''),
         ('goto', 'Go-To', ''), ('p25search', 'P25 search', ''), ('airband', 'Air band', ''),
         ('battery', 'Battery', '87%')]


def mock_tools(icons, fonts, theme, first=0, focus=1):
    t = {k: q(v) for k, v in THEMES[theme].items()}
    cv = Canvas(240, 320, t['bg'])
    f = fonts['9x18']
    byname = dict(icons)
    # Status strip (ui_draw_top): time left, battery right, a line under
    f.text(cv, 2, 0, '12:34', t['fg'])
    f.text(cv, 240 - f.width('87%') - 22, 0, '87%', t['fg'])
    draw_icon(cv, byname['battery'], 240 - 18, 1, t['fg'])
    cv.fill(0, 19, 240, 1, t['line'])
    f.text(cv, 1, 21, 'Tools', t['dim'])
    y0, row_h = 19 + 18 + 2, 24
    hint_y = 320 - 18
    rows = (hint_y - y0) // row_h
    for r in range(rows):
        i = first + r
        if i >= len(TOOLS):
            break
        name, label, value = TOOLS[i]
        ry = y0 + r * row_h
        fg, vfg, nfg = t['fg'], t['dim'], t['dim']
        if name in ('usb_net', 'sfr', 'roam'):
            fg = vfg = t['line']	# UI_ITEM_DIM: not in this build
        if i == focus:
            cv.fill(0, ry, 240, row_h, t['focus'])
            fg = vfg = nfg = q((255, 255, 255))
        pad = (row_h - f.h) // 2
        x = f.text(cv, 1, ry + pad, str((i + 1) % 10) + ' ', nfg)	# item_num: the tool's number mod 10
        draw_icon(cv, byname[name], x, ry + (row_h - ICON) // 2, fg)
        x += ICON + 5
        f.text(cv, x, ry + pad, label, fg)
        if value:
            f.text(cv, 240 - f.width(value) - 4, ry + pad, value, vfg)
    cv.fill(0, hint_y, 240, 18, t['hint_bg'])
    f.text(cv, 2, hint_y, '1-9 open  Back', t['dim'])
    return cv


def preview_sheet(icons, mark, word, fonts):
    cols, cell_w, cell_h = 4, 150, 64
    nrows = (len(icons) + cols - 1) // cols
    mw, mh = mark[0]['size']
    ww, wh = word[0]['size']
    head = max(mh * 2, 1) + wh * 2 + 40
    W, H = cols * cell_w + 16, head + nrows * cell_h + 16
    cv = Canvas(W, H, (24, 24, 28))
    draw_mark(cv, mark, 16, 16, 2)
    draw_mark(cv, mark, 16 + mw * 2 + 24, 16, 1)
    accent = q(mark[0]['color']['a'][0])
    draw_word(cv, word, 16, 16 + mh * 2 + 12, 2, (255, 255, 255), accent)
    draw_word(cv, word, 16 + ww * 2 + 24, 16 + mh * 2 + 12, 1, (255, 255, 255), accent)
    f = fonts.get('8x16')
    for n, (name, rows) in enumerate(icons):
        x = 8 + (n % cols) * cell_w
        y = head + (n // cols) * cell_h
        # 3x on a light tile, 1x dark and light, the name under
        cv.fill(x, y, 3 * ICON + 4, 3 * ICON + 4, (232, 232, 232))
        draw_icon(cv, rows, x + 2, y + 2, (16, 16, 16), 3)
        cv.fill(x + 56, y + 2, ICON + 4, ICON + 4, (0, 0, 0))
        draw_icon(cv, rows, x + 58, y + 4, (255, 255, 255))
        cv.fill(x + 80, y + 2, ICON + 4, ICON + 4, (255, 255, 255))
        draw_icon(cv, rows, x + 82, y + 4, (0, 0, 0))
        cv.fill(x + 104, y + 2, ICON + 4, ICON + 4, (32, 96, 200))
        draw_icon(cv, rows, x + 106, y + 4, (255, 255, 255))
        if f:
            f.text(cv, x + 56, y + 28, name[:11], (200, 200, 200))
    return cv


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--check', action='store_true', help='fail if the generated C differs from the sources')
    ap.add_argument('--preview', metavar='FONT_DIR', help='write the PNG previews; fonts from FONT_DIR')
    a = ap.parse_args(argv)

    icons = load_icons(os.path.join(ART, 'icons.txt'))
    mark = load_mark(os.path.join(ART, 'mark.txt'))
    word = load_wordmark(os.path.join(ART, 'wordmark.txt'))
    h, c, total = gen_c(icons, mark, word)
    files = {os.path.join(OUT, 'art_data.h'): h, os.path.join(OUT, 'art_data.c'): c}
    if a.check:
        bad = [p for p, s in files.items() if not os.path.exists(p) or open(p, encoding='utf-8').read() != s]
        if bad:
            print('art_build: out of date: ' + ', '.join(os.path.relpath(p, ROOT) for p in bad))
            return 1
    else:
        os.makedirs(OUT, exist_ok=True)
        for p, s in files.items():
            with open(p, 'w', encoding='utf-8', newline='\n') as f:
                f.write(s)
    print('art_build: %d icons, mark %dx%d, wordmark %dx%d, %d bytes of tables' %
          (len(icons), mark[0]['size'][0], mark[0]['size'][1], word[0]['size'][0], word[0]['size'][1], total))

    if a.preview:
        fonts = {}
        for size in ('8x16', '9x18', '12x24'):
            p = os.path.join(a.preview, 'jbmono_%s.inc' % size)
            if os.path.exists(p):
                fonts[size] = Font(p)
        if '9x18' not in fonts:
            raise SystemExit('art_build: --preview needs jbmono_9x18.inc in ' + a.preview)
        preview_sheet(icons, mark, word, fonts).save(os.path.join(ART, 'preview.png'))
        sp = [mock_splash(mark, word, fonts, th) for th in ('dark', 'light')]
        both = Canvas(240 * 2 + 8, 320, (128, 128, 128))
        for i, s in enumerate(sp):
            for y in range(320):
                both.px[y][i * 248:i * 248 + 240] = s.px[y]
        both.scaled(2).save(os.path.join(ART, 'mock-splash.png'))
        tl = [mock_tools(icons, fonts, 'dark', 0, 1), mock_tools(icons, fonts, 'light', 8, 10)]
        both = Canvas(240 * 2 + 8, 320, (128, 128, 128))
        for i, s in enumerate(tl):
            for y in range(320):
                both.px[y][i * 248:i * 248 + 240] = s.px[y]
        both.scaled(2).save(os.path.join(ART, 'mock-tools.png'))
        print('art_build: previews in ' + os.path.relpath(ART, ROOT))
    return 0


if __name__ == '__main__':
    sys.exit(main())
