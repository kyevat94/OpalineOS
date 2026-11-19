# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 The OpalineOS authors
"""Generates the kernel console font (assets/fonts/console.fnt) from a TrueType font.

Run this only when the font changes; the generated file is committed.
Needs Pillow and fontTools (pip install pillow fonttools).

    python tools/gen_font.py PATH/TO/DejaVuSansMono.ttf [--size 15]

Output format ("OPFN", little endian):
    0   char[4]  magic "OPFN"
    4   u16      version (1)
    6   u8       cell width  (pixels)
    7   u8       cell height (pixels)
    8   u8       baseline    (pixels from the top of the cell)
    9   u8       flags (0)
    10  u16      reserved
    12  u32      glyph count N
    16  u32      offset of the code point table (N x u32, sorted ascending)
    20  u32      offset of the bitmaps (N x width*height bytes, 8-bit alpha, row major)
    24  u8[8]    reserved
The glyph for U+FFFD (replacement character) is always present.
"""
import argparse
import os
import struct

from fontTools.ttLib import TTFont
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "assets", "fonts", "console.fnt")

RANGES = [
    (0x0020, 0x007E),   # ASCII
    (0x00A0, 0x00FF),   # Latin-1 Supplement
    (0x0100, 0x017F),   # Latin Extended-A
    (0x0180, 0x024F),   # Latin Extended-B
    (0x0370, 0x03FF),   # Greek
    (0x0400, 0x04FF),   # Cyrillic
    (0x1E00, 0x1EFF),   # Latin Extended Additional (Vietnamese)
    (0x2010, 0x2027),   # dashes, quotes, bullet, ellipsis
    (0x2030, 0x203A),
    (0x20AC, 0x20AC),   # euro sign
    (0x2122, 0x2122),   # trade mark
    (0x2190, 0x2199),   # arrows
    (0x2500, 0x259F),   # box drawing, block elements
    (0x25A0, 0x25FF),   # geometric shapes
    (0xFFFD, 0xFFFD),   # replacement character
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ttf")
    ap.add_argument("--size", type=int, default=15)
    args = ap.parse_args()

    cmap = TTFont(args.ttf).getBestCmap()
    font = ImageFont.truetype(args.ttf, args.size)
    ascent, descent = font.getmetrics()
    cell_w = round(font.getlength("M"))
    cell_h = ascent + descent
    baseline = ascent

    codepoints = [cp for lo, hi in RANGES for cp in range(lo, hi + 1) if cp in cmap]
    if 0xFFFD not in codepoints:
        raise SystemExit("the font has no U+FFFD glyph")

    bitmaps = bytearray()
    for cp in codepoints:
        img = Image.new("L", (cell_w, cell_h), 0)
        ImageDraw.Draw(img).text((0, baseline), chr(cp), font=font, fill=255, anchor="ls")
        bitmaps += img.tobytes()

    n = len(codepoints)
    header = struct.pack("<4sHBBBBHIII8s", b"OPFN", 1, cell_w, cell_h, baseline, 0, 0,
                         n, 32, 32 + 4 * n, b"\0" * 8)
    assert len(header) == 32
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "wb") as f:
        f.write(header + struct.pack(f"<{n}I", *codepoints) + bitmaps)
    print(f"OK: {OUT}: {n} glyphs, cell {cell_w}x{cell_h}, baseline {baseline}, "
          f"{os.path.getsize(OUT) // 1024} KiB")


if __name__ == "__main__":
    main()
