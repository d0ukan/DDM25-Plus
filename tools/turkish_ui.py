#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Turkish UI support for the RadioFoundry derivative (applied by tools/build_radiofoundry.py).

The radio draws one byte a character with ASCII + Latin-1 glyphs. Turkish needs six letters outside Latin-1
(Ğ ğ İ ı Ş ş). Like ISO-8859-9 (Latin-5), they take the slots of six Icelandic letters no shipped language
uses: Ð->Ğ, Ý->İ, Þ->Ş, ð->ğ, ý->ı, þ->ş. The glyphs are composed from glyphs the pinned font files already
contain (base letter + an accent taken from a Latin-1 letter), so no new font source is needed.

    python tools/turkish_ui.py --preview [FONT]   print the composed glyphs as text art
"""
import argparse
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
UP = ROOT / "research/upstream/radiofoundry"

# Turkish letter -> the Latin-1 byte it is drawn from (ISO-8859-9 layout)
TURKISH_BYTES = {"Ğ": 0xD0, "İ": 0xDD, "Ş": 0xDE, "ğ": 0xF0, "ı": 0xFD, "ş": 0xFE}
FONTS = ("spleen_5x8", "spleen_6x12", "spleen_8x16", "spleen_12x24", "spleen_16x32",
         "jbmono_6x12", "jbmono_8x16", "jbmono_9x18", "jbmono_12x24", "jbmono_16x32")
EXTRAS = 4          # core/ui/font.h GLYPH_EXTRA_COUNT: glyphs stored between ASCII and Latin-1


class Font:
    def __init__(self, name, text):
        self.name, self.text = name, text
        up = name.upper()
        self.w = int(re.search(r"#define %s_W (\d+)" % up, text).group(1))
        self.h = int(re.search(r"#define %s_H (\d+)" % up, text).group(1))
        m = re.search(r"#define %s_BPP (\d+)" % up, text)
        self.bpp = int(m.group(1)) if m else 1
        self.row_bytes = (self.w * self.bpp + 7) // 8
        body = re.search(r"static const uint8_t %s_bits\[\] = \{\n(.*?)\n\};" % name, text, re.S)
        self.bits_span = body.span(1)
        self.lines = body.group(1).split("\n")
        self.glyph_lines = [i for i, l in enumerate(self.lines) if l.startswith("\t0x")]
        idx = re.search(r"static const uint16_t %s_index\[\] = \{\n(.*?)\n\};" % name, text, re.S)
        self.index_span = idx.span(1)
        self.index_lines = idx.group(1).split("\n")
        self.glyphs = [self.parse(self.lines[i]) for i in self.glyph_lines]

    def slot(self, code):
        """Glyph list position of a byte: 0x20..0x7E, the extras, then Latin-1 0xA0..0xFF"""
        if 0x20 <= code < 0x7F:
            return code - 0x20
        if code >= 0xA0:
            return 0x5F + EXTRAS + code - 0xA0
        raise ValueError(code)

    def parse(self, line):
        data = [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", line.split("//")[0])]
        first, rows = data[0], data[1]
        grid = [[0] * self.w for _ in range(self.h)]
        per = (1 << self.bpp) - 1
        for r in range(rows):
            raw = data[2 + r * self.row_bytes: 2 + (r + 1) * self.row_bytes]
            value = int.from_bytes(bytes(raw), "big")
            total = self.row_bytes * 8
            for x in range(self.w):
                grid[first + r][x] = (value >> (total - (x + 1) * self.bpp)) & per
        return grid

    def get(self, ch):
        return [row[:] for row in self.glyphs[self.slot(ord(ch))]]

    def encode(self, grid):
        inked = [y for y in range(self.h) if any(grid[y])]
        if not inked:
            return [0, 0]
        first, last = inked[0], inked[-1]
        out = [first, last - first + 1]
        for y in range(first, last + 1):
            value = 0
            for x in range(self.w):
                value = (value << self.bpp) | grid[y][x]
            value <<= self.row_bytes * 8 - self.w * self.bpp
            out += list(value.to_bytes(self.row_bytes, "big"))
        return out

    def render(self):
        lines = list(self.lines)
        sizes = []
        for n, i in enumerate(self.glyph_lines):
            data = self.encode(self.glyphs[n])
            if self.encode(self.parse(lines[i])) != data:
                comment = lines[i].split("//", 1)[1] if "//" in lines[i] else ""
                lines[i] = "\t" + ",".join("0x%02X" % b for b in data) + ",\t//" + comment
            sizes.append(len(data))
        offsets, pos = [], 0
        for size in sizes:
            offsets.append(pos)
            pos += size
        index, k = [], 0
        for l in self.index_lines:
            if l.startswith("#"):
                index.append(l)
                continue
            count = len(re.findall(r"\d+", l))
            index.append("\t" + ",".join(str(v) for v in offsets[k:k + count]) + ",")
            k += count
        if k != len(offsets):
            raise SystemExit("%s: index has %d entries, glyphs need %d" % (self.name, k, len(offsets)))
        a, b = self.bits_span
        c, d = self.index_span
        return self.text[:a] + "\n".join(lines) + self.text[b:c] + "\n".join(index) + self.text[d:]


def diff(accented, base):
    """The accent's pixels: inked in the accented letter, not in its base"""
    return [[a if a and not b else 0 for a, b in zip(ra, rb)] for ra, rb in zip(accented, base)]


def overlay(grid, mark):
    return [[max(g, m) for g, m in zip(rg, rm)] for rg, rm in zip(grid, mark)]


def rows_of(grid):
    return [y for y, row in enumerate(grid) if any(row)]


def flip_rows(mark):
    """Mirror a mark top to bottom inside its own rows (a circumflex becomes a breve-like cup)"""
    ys = rows_of(mark)
    out = [[0] * len(mark[0]) for _ in mark]
    for y in ys:
        out[ys[0] + ys[-1] - y] = mark[y][:]
    return out


def place_above(font, base, mark):
    """Put a mark over a capital: at the accent rows the font's own accented capitals use, or, when this size
    draws its accented capitals without accents (no rows free above the cap height), squeezed into the rows
    above the letter with one blank row between"""
    out = [[0] * font.w for _ in range(font.h)]
    ys = rows_of(mark)
    if not ys:
        return out
    top = rows_of(font.get(base))[0]
    room = top - 1                      # rows 0..top-2, row top-1 stays blank
    keep = ys[-min(len(ys), room):] if room > 0 else []
    for k, y in enumerate(keep):
        out[top - 1 - len(keep) + k] = mark[y][:]
    return out


def compose(font):
    """The six Turkish letters, each built from glyphs this font already has"""
    def above(ch, base):
        top = rows_of(font.get(base))[0]
        return [r if y < top else [0] * font.w for y, r in enumerate(diff(font.get(ch), font.get(base)))]
    x_top = rows_of(font.get("n"))[0]
    i = font.get("i")
    blank = [0] * font.w
    dotless = [r if y >= x_top else blank[:] for y, r in enumerate(i)]       # i without its dot
    dot = [r if y < x_top else blank[:] for y, r in enumerate(i)]
    low_breve = flip_rows(above("â", "a"))                              # a circumflex mirrored: a cup
    cap_breve = flip_rows(above("Â", "A"))
    if not rows_of(cap_breve):
        cap_breve = place_above(font, "G", low_breve)
    cap_dot = above("Í", "I")
    if rows_of(cap_dot):    # the dot where this size draws the acute of Í, bottom-aligned
        acute, d = rows_of(cap_dot), rows_of(dot)
        cap_dot = [blank[:] for _ in range(font.h)]
        for y in d:
            cap_dot[y + acute[-1] - d[-1]] = dot[y][:]
    else:
        cap_dot = place_above(font, "I", dot)
    return {
        0xD0: overlay(font.get("G"), cap_breve),
        0xF0: overlay(font.get("g"), low_breve),
        0xDE: overlay(font.get("S"), diff(font.get("Ç"), font.get("C"))),   # the cedilla of Ç under S
        0xFE: overlay(font.get("s"), diff(font.get("ç"), font.get("c"))),
        0xDD: overlay(font.get("I"), cap_dot),
        0xFD: dotless,
    }


def has_latin1(font):
    return len(font.glyphs) > font.slot(0xFF)


def turkish_font(name, text=None):
    """The pinned font file (or `text`) with the six Turkish letters in their Latin-5 slots"""
    if text is None:
        text = (UP / "core/ui/fonts" / (name + ".inc")).read_text(encoding="utf-8")
    font = Font(name, text)
    if not has_latin1(font):
        return font.text            # this size has no Latin-1 glyphs at all (the UI draws its fallback)
    for code, grid in compose(font).items():
        font.glyphs[font.slot(code)] = grid
    return font.render()


def font_transform(name, text):
    """A (old, new) pair for build_radiofoundry.transform: the whole font file, its Turkish letters compiled in
    only with CONFIG_LANG_TR (other builds keep the pinned bytes; the font file sees config.h through font.h)"""
    turkish = turkish_font(name, text)
    if turkish == text:
        return None
    return (text, "#if CONFIG_LANG_TR\t// DM32 Plus: Turkish letters in the Latin-5 slots (tools/turkish_ui.py)\n"
            + turkish + "#else\n" + text + "#endif\n")


def preview(name):
    font = Font(name, (UP / "core/ui/fonts" / (name + ".inc")).read_text(encoding="utf-8"))
    if not has_latin1(font):
        print(name, "has no Latin-1 glyphs; left as is")
        return
    shades = " .+#" if font.bpp == 2 else " #"
    made = compose(font)
    show = [("G", font.get("G")), ("Ğ", made[0xD0]), ("g", font.get("g")), ("ğ", made[0xF0]),
            ("S", font.get("S")), ("Ş", made[0xDE]), ("s", font.get("s")), ("ş", made[0xFE]),
            ("I", font.get("I")), ("İ", made[0xDD]), ("i", font.get("i")), ("ı", made[0xFD])]
    print(name)
    print("  ".join(c.ljust(font.w) for c, _ in show))
    for y in range(font.h):
        print("  ".join("".join(shades[min(v, len(shades) - 1)] for v in g[y]) for _, g in show))


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--preview", nargs="*", metavar="FONT")
    args = ap.parse_args()
    for name in (args.preview or FONTS):
        if (UP / "core/ui/fonts" / (name + ".inc")).exists():
            preview(name)
            assert Font(name, turkish_font(name)).glyphs  # round trip parses
