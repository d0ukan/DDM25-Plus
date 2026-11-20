#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Makes the program icon (win/src/DDM32Plus/Assets/ddm32plus.ico, 16..256 px) from the owner's drawing
art/mark-src.png: only the mark (the radio "D" with its antenna arcs and the orange key), not the "DDM32+" text
under it, white and orange on a black rounded square.

Large sizes are the drawing scaled down (8x supersampled). From 32 px down the antenna arcs are left out and the
strokes are thickened a little, so the radio "D" with its orange key still reads at 16 px. Needs Pillow.
Deterministic; run it again after changing the drawing:

    python win/tools/make_icon.py [path/to/mark-src.png]
"""
from pathlib import Path
import sys

from PIL import Image, ImageDraw, ImageFilter

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
OUT = REPO / 'win' / 'src' / 'DDM32Plus' / 'Assets'
SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)
SMALL = 32          # this size and below: no arcs, thicker strokes


def kind(c):
    r, g, b = c[:3]
    if r > 140 and g > 140 and b > 140:
        return 1        # white
    if r > 150 and 60 < g < 200 and b < 120 and r - b > 80:
        return 2        # orange
    return 0


def mark_box(im):
    """The inked part above the first blank band (the text below is left out), as art/mark_gen.py traces it"""
    px = im.load()
    w, h = im.size
    rows = [any(kind(px[x, y]) for x in range(w)) for y in range(h)]
    top = rows.index(True)
    bot = top
    while bot + 1 < h and rows[bot + 1]:
        bot += 1
    xs = [x for y in range(top, bot + 1) for x in range(w) if kind(px[x, y])]
    return min(xs), top, max(xs) + 1, bot + 1


def clean(im, box, small):
    """The mark as RGBA (white / orange on transparent); small: without the arcs left of the stem"""
    x0, y0, x1, y1 = box
    src = im.crop(box)
    px = src.load()
    w, h = src.size
    out = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    po = out.load()
    keep = None
    if small:
        # the antenna arcs are white parts of their own: keep only the largest white part (stem and D) and the key
        seen, parts = set(), []
        for y in range(h):
            for x in range(w):
                if (x, y) in seen or kind(px[x, y]) != 1:
                    continue
                part, todo = [], [(x, y)]
                seen.add((x, y))
                while todo:
                    cx, cy = todo.pop()
                    part.append((cx, cy))
                    for nx, ny in ((cx + 1, cy), (cx - 1, cy), (cx, cy + 1), (cx, cy - 1)):
                        if 0 <= nx < w and 0 <= ny < h and (nx, ny) not in seen and kind(px[nx, ny]) == 1:
                            seen.add((nx, ny))
                            todo.append((nx, ny))
                parts.append(part)
        keep = set(max(parts, key=len))
    for y in range(h):
        for x in range(w):
            k = kind(px[x, y])
            if not k or (keep is not None and k == 1 and (x, y) not in keep):
                continue
            po[x, y] = (255, 255, 255, 255) if k == 1 else (240, 120, 0, 255)
    if small:
        out = out.crop(out.getbbox())
        # thicken: a max filter on the alpha keeps the shape, the colours follow the nearest inked pixel
        alpha = out.getchannel('A').filter(ImageFilter.MaxFilter(5))
        rgb = out.convert('RGB').filter(ImageFilter.MaxFilter(5))
        out = Image.merge('RGBA', (*rgb.split(), alpha))
    return out


def render(mark, size):
    s = 8
    big = size * s
    tile = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    d = ImageDraw.Draw(tile)
    d.rounded_rectangle((0, 0, big - 1, big - 1), radius=int(big * 0.18), fill=(16, 16, 16, 255))
    pad = 0.10 if size > SMALL else 0.06
    room = big * (1 - 2 * pad)
    w, h = mark.size
    k = min(room / w, room / h)
    m = mark.resize((max(1, round(w * k)), max(1, round(h * k))), Image.LANCZOS)
    tile.alpha_composite(m, ((big - m.width) // 2, (big - m.height) // 2))
    return tile.resize((size, size), Image.LANCZOS)


def main():
    src = Path(sys.argv[1]) if len(sys.argv) > 1 else REPO / 'art' / 'mark-src.png'
    im = Image.open(src).convert('RGB')
    box = mark_box(im)
    large, small = clean(im, box, False), clean(im, box, True)
    images = [render(small if n <= SMALL else large, n) for n in SIZES]
    OUT.mkdir(parents=True, exist_ok=True)
    ico = OUT / 'ddm32plus.ico'
    images[-1].save(ico, format='ICO', sizes=[(n, n) for n in SIZES], append_images=images[:-1])
    # a preview of every size side by side, 4x, for checking by eye (not shipped)
    prev = Image.new('RGBA', (sum(n * 4 + 8 for n in SIZES), 256 * 4 // 2 + 8), (200, 200, 200, 255))
    x = 4
    for n, img in zip(SIZES, images):
        if n <= 128:
            prev.alpha_composite(img.resize((n * 4, n * 4), Image.NEAREST), (x, 4))
            x += n * 4 + 8
    prev.crop((0, 0, x, 128 * 4 // 1 + 8)).save(HERE / 'icon-preview.png')
    print(f'{ico} ({ico.stat().st_size} bytes), sizes {SIZES}')


if __name__ == '__main__':
    main()
