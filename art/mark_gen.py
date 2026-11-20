#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Traces the DDM32 Plus splash mark from the owner's drawing (art/mark-src.png) into art/mark.txt, the source
tools/art_build.py reads.

The mark (owner's own design, 2026-11-20): a handheld radio drawn as a "D". A thick stem is the antenna, with two
signal arcs at its tip, and the shared left side of an outer D frame and an inner D; an orange key sits on the stem.
The white parts take the theme's text color (role fg: white on dark, black on light), the orange key is the accent
(color a), which also colors "Plus", the rule and the callsign.

Tracing: the inked part above the first blank band of the image (the text under it is left out) is scaled to H_OUT
rows with square pixels; each mark pixel samples its source box (6 x 6 points) and takes orange from ORANGE_MIN
coverage, else white from WHITE_MIN. Needs Pillow (only this script; tools/art_build.py stays stdlib only). Run it
after changing the drawing, then tools/art_build.py. Deterministic.
"""
import os
import sys

from PIL import Image

H_OUT = 56			# the mark's height in pixels (2x on 240 px panels: 112)
WHITE_MIN = 0.5
ORANGE_MIN = 0.4
S = 6				# samples a side per mark pixel
COLORS = {  # name: (R, G, B, role: ink/paper on a 1 bpp panel, or fg: the theme's text color)
    'a': ((240, 120, 0), 'ink'),       # the key; the accent: orange, reads on black and on white
    'b': ((255, 255, 255), 'fg'),      # frame, stem, arcs: the theme's text color (white is the preview's)
    'c': ((240, 120, 0), 'ink'),       # unused (the format has three colors)
}
HERE = os.path.dirname(os.path.abspath(__file__))


def kind(c):
    r, g, b = c[:3]
    if r > 140 and g > 140 and b > 140:
        return 1		# white
    if r > 150 and 60 < g < 200 and b < 120 and r - b > 80:
        return 2		# orange
    return 0


def trace(path):
    im = Image.open(path).convert('RGB')
    w, h = im.size
    px = im.load()
    rows = [any(kind(px[x, y]) for x in range(w)) for y in range(h)]
    top = rows.index(True)
    bot = top
    while bot + 1 < h and rows[bot + 1]:
        bot += 1
    xs = [x for y in range(top, bot + 1) for x in range(w) if kind(px[x, y])]
    x0, x1 = min(xs), max(xs)
    step = (bot - top + 1) / H_OUT
    w_out = int(round((x1 - x0 + 1) / step))
    w_out += w_out % 2			# even: art.c centres it with cx - W * k / 2
    left = (x0 + x1 + 1) / 2 - w_out * step / 2
    out = []
    for j in range(H_OUT):
        line = []
        for i in range(w_out):
            white = orange = 0
            for a in range(S):
                for b in range(S):
                    x = int(left + i * step + (b + .5) * step / S)
                    y = int(top + j * step + (a + .5) * step / S)
                    k = kind(px[x, y]) if 0 <= x < w and 0 <= y < h else 0
                    white += k == 1
                    orange += k == 2
            line.append('a' if orange >= ORANGE_MIN * S * S else 'b' if white >= WHITE_MIN * S * S else '.')
        out.append(''.join(line))
    return w_out, out


def main():
    w_out, rows = trace(os.path.join(HERE, 'mark-src.png'))
    out = ['; DDM32 Plus splash mark, traced from art/mark-src.png by art/mark_gen.py (or edit by hand).',
           '; "." none; a, b, c the colors below (RGB, then its role: ink/paper on a 1 bpp panel, fg the theme\'s text).',
           'size %d %d' % (w_out, H_OUT)]
    for k in 'abc':
        rgb, role = COLORS[k]
        out.append('color %s %d %d %d %s' % (k, rgb[0], rgb[1], rgb[2], role))
    out += rows
    with open(os.path.join(HERE, 'mark.txt'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(out) + '\n')
    print('mark %dx%d traced' % (w_out, H_OUT), file=sys.stderr)


if __name__ == '__main__':
    main()
