#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Draws the DDM32 Plus splash mark into art/mark.txt (the source tools/art_build.py reads).

The mark (owner's own design, 2026-11-20): a handheld radio drawn as a "D". Its antenna is the D's stem, rising above
the body with two signal arcs at its tip; the body is a D outline with chamfered right corners, a smaller D outline
inside it, and an orange key on the stem's left side. The outlines, stem and arcs (b) take the theme's text color
(white on dark, black on light: role fg), the key is the accent orange (a), which also colors "Plus", the rule and
the callsign. Run this after changing the shapes below, then tools/art_build.py. Deterministic: 8 x 8 samples a
pixel, a pixel takes a shape at half coverage or more.
"""
import math
import os

W = H = 56
SS = 8
COLORS = {  # name: (R, G, B, role: ink/paper on a 1 bpp panel, or fg: the theme's text color)
    'a': ((240, 120, 0), 'ink'),       # the key; the accent: orange, reads on black and on white
    'b': ((255, 255, 255), 'fg'),      # outlines, antenna, arcs: the theme's text color (white is the preview's)
    'c': ((240, 120, 0), 'ink'),       # unused (the format has three colors)
}


def ring(cx, cy, r0, r1, a0, a1):
    """An annulus sector; degrees, 0 = right, counter-clockwise on screen (y down)"""
    def f(x, y):
        if not r0 <= math.hypot(x - cx, y - cy) <= r1:
            return False
        a = math.degrees(math.atan2(cy - y, x - cx)) % 360
        lo, hi = a0 % 360, a1 % 360
        return lo <= a <= hi if lo <= hi else (a >= lo or a <= hi)
    return f


def poly(pts):
    def f(x, y):
        inside = False
        for i in range(len(pts)):
            (x1, y1), (x2, y2) = pts[i], pts[(i + 1) % len(pts)]
            if (y1 > y) != (y2 > y) and x < (x2 - x1) * (y - y1) / (y2 - y1) + x1:
                inside = not inside
        return inside
    return f


def rect(x0, y0, x1, y1):
    return lambda x, y: x0 <= x <= x1 and y0 <= y <= y1


def union(*fs):
    return lambda x, y: any(f(x, y) for f in fs)


def minus(f, g):
    return lambda x, y: f(x, y) and not g(x, y)


def d_shape(x0, y0, x1, y1, c):
    """A D: a rectangle whose two right corners are cut by c"""
    return poly([(x0, y0), (x1 - c, y0), (x1, y0 + c), (x1, y1 - c), (x1 - c, y1), (x0, y1)])


def d_outline(x0, y0, x1, y1, c, t):
    """The D's outline, t thick (the inner cut keeps the stroke even along the chamfers)"""
    k = t * (math.sqrt(2) - 1)
    return minus(d_shape(x0, y0, x1, y1, c), d_shape(x0 + t, y0 + t, x1 - t, y1 - t, c - k))


STEM_X0, STEM_X1 = 17.0, 21.0	# the antenna and the body's left side
TIP = 12.0			# the antenna's top, where the arcs start
SHAPES = [
    ('b', union(
        rect(STEM_X0, TIP, STEM_X1, 54.0),				# antenna and stem
        d_outline(STEM_X0, 25.0, 50.0, 54.0, 9.0, 4.0),		# the body
        d_outline(25.5, 32.0, 42.0, 47.0, 5.0, 3.0),		# the D inside
        ring(STEM_X0 + 2.0, TIP + 1.0, 5.0, 8.0, 95, 170),		# the signal arcs, up and left of the tip
        ring(STEM_X0 + 2.0, TIP + 1.0, 10.5, 13.5, 95, 170))),
    ('a', rect(11.0, 30.0, 16.0, 35.5)),				# the key, left of the stem
]


def main():
    px = [['.'] * W for _ in range(H)]
    for y in range(H):
        for x in range(W):
            for name, f in SHAPES:
                n = sum(f(x + (sx + .5) / SS, y + (sy + .5) / SS) for sy in range(SS) for sx in range(SS))
                if n * 2 >= SS * SS:
                    px[y][x] = name
    out = ['; DDM32 Plus splash mark. Written by art/mark_gen.py (or edit by hand).',
           '; "." none; a, b, c the colors below (RGB, then its role: ink/paper on a 1 bpp panel, fg the theme\'s text).',
           'size %d %d' % (W, H)]
    for k in 'abc':
        rgb, role = COLORS[k]
        out.append('color %s %d %d %d %s' % (k, rgb[0], rgb[1], rgb[2], role))
    out += [''.join(r) for r in px]
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'mark.txt')
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(out) + '\n')


if __name__ == '__main__':
    main()
