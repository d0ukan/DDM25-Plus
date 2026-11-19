#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Draws the DM32 Plus splash mark into art/mark.txt (the source tools/art_build.py reads).

The mark (owner's choice 2026-11-18, "antenna and waves"): a dark navy round badge (color a), two pairs of teal arcs
radiating from the antenna's top (b), and a yellow mast with a ball top (c). Run this after changing the shapes below,
then tools/art_build.py; or edit mark.txt by hand and leave this script alone. Deterministic: 8 x 8 samples a pixel,
a pixel takes a shape at half coverage or more.
"""
import math
import os

W = H = 56
SS = 8
COLORS = {  # name: (R, G, B, on a 1 bpp panel)
    'a': ((18, 32, 47), 'ink'),        # badge: dark navy
    'b': ((0, 200, 180), 'paper'),     # waves: teal
    'c': ((255, 196, 0), 'paper'),     # antenna: warm yellow
}


def circle(cx, cy, r):
    return lambda x, y: (x - cx) ** 2 + (y - cy) ** 2 <= r * r


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


def rrect(x0, y0, x1, y1, r):
    def f(x, y):
        if not (x0 <= x <= x1 and y0 <= y <= y1):
            return False
        cx = min(max(x, x0 + r), x1 - r)
        cy = min(max(y, y0 + r), y1 - r)
        return (x - cx) ** 2 + (y - cy) ** 2 <= r * r
    return f


def rect(x0, y0, x1, y1):
    return lambda x, y: x0 <= x <= x1 and y0 <= y <= y1


def union(*fs):
    return lambda x, y: any(f(x, y) for f in fs)


# A four-level trace: (x from, x to, level 0..3), joined by verticals, stroke t
def trace(steps, top, pitch, t):
    parts = []
    for i, (xa, xb, lv) in enumerate(steps):
        y = top + (3 - lv) * pitch
        parts.append(rect(xa - t / 2, y - t / 2, xb + t / 2, y + t / 2))
        if i + 1 < len(steps):
            y2 = top + (3 - steps[i + 1][2]) * pitch
            parts.append(rect(xb - t / 2, min(y, y2) - t / 2, xb + t / 2, max(y, y2) + t / 2))
    return union(*parts)


C = 27.5	# the badge's centre
TOP = 22	# the antenna's ball, where the waves start
SHAPES = [
    ('a', circle(C, C, 28)),
    ('b', union(ring(C, TOP, 9, 12.5, 125, 235), ring(C, TOP, 9, 12.5, -55, 55),
                ring(C, TOP, 16, 19.5, 130, 230), ring(C, TOP, 16, 19.5, -50, 50))),
    ('c', union(circle(C, TOP, 4.5), poly([(C - 2.2, 25), (C + 2.2, 25), (C + 7, 48), (C - 7, 48)]))),
]


def main():
    px = [['.'] * W for _ in range(H)]
    for y in range(H):
        for x in range(W):
            for name, f in SHAPES:
                n = sum(f(x + (sx + .5) / SS, y + (sy + .5) / SS) for sy in range(SS) for sx in range(SS))
                if n * 2 >= SS * SS:
                    px[y][x] = name
    out = ['; DM32 Plus splash mark. Written by art/mark_gen.py (or edit by hand).',
           '; "." none; a, b, c the colors below (RGB, then what each is on a 1 bpp panel).',
           'size %d %d' % (W, H)]
    for k in 'abc':
        rgb, mono = COLORS[k]
        out.append('color %s %d %d %d %s' % (k, rgb[0], rgb[1], rgb[2], mono))
    out += [''.join(r) for r in px]
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'mark.txt')
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(out) + '\n')


if __name__ == '__main__':
    main()
