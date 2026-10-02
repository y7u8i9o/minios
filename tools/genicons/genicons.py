#!/usr/bin/env python3
"""Write PNG files without external libraries.

  genicons.py icons <dir>   16x16 icons drawn from the pixel art below
  genicons.py tests <dir>   images for libgui/tests/test_images.c

The writer applies a filter per row (cycling through all five) so the
decoder's filters are exercised by the test images.
"""
import os
import struct
import sys
import zlib

PALETTE = {
    ".": (0, 0, 0, 0),
    "k": (32, 32, 32, 255),
    "w": (255, 255, 255, 255),
    "g": (160, 160, 160, 255),
    "b": (48, 96, 176, 255),
    "y": (232, 192, 64, 255),
    "r": (200, 64, 64, 255),
    "n": (64, 160, 80, 255),
    "o": (224, 144, 48, 255),
    "h": (128, 128, 128, 128),
}

ICONS = {
    "new": [
        "................", "...kkkkkkkk.....", "...kwwwwwwkk....", "...kwwwwwwkwk...",
        "...kwwwwwwkkkk..", "...kwwwwwwwwwk..", "...kwwwwwwwwwk..", "...kwwwwwwwwwk..",
        "...kwwwwwwwwwk..", "...kwwwwwwwwwk..", "...kwwwwwwwwwk..", "...kwwwwwwwwwk..",
        "...kwwwwwwwwwk..", "...kwwwwwwwwwk..", "...kkkkkkkkkkk..", "................",
    ],
    "open": [
        "................", "................", "..kkkkk.........", ".kyyyyykkkkkkk..",
        ".kyyyyyyyyyyyk..", ".kyyyyyyyyyyyk..", ".kyyykkkkkkkkkkk", ".kyykooooooooook",
        ".kyykooooooooook", ".kykooooooooook.", ".kykooooooooook.", ".kkoooooooooook.",
        ".kkoooooooooook.", ".kkkkkkkkkkkkk..", "................", "................",
    ],
    "save": [
        "................", ".kkkkkkkkkkkkkk.", ".kbbkwwwwwwkbbk.", ".kbbkwwwwwwkbbk.",
        ".kbbkwwwwwwkbbk.", ".kbbkkkkkkkkbbk.", ".kbbbbbbbbbbbbk.", ".kbbbbbbbbbbbbk.",
        ".kbbbbbbbbbbbbk.", ".kbbkkkkkkkkbbk.", ".kbbkggggggkbbk.", ".kbbkggggggkbbk.",
        ".kbbkggggggkbbk.", ".kbbkggggggkbbk.", ".kkkkkkkkkkkkkk.", "................",
    ],
    "cut": [
        "................", "...k.......k....", "...k.......k....", "....k.....k.....",
        "....k.....k.....", ".....k...k......", ".....k...k......", "......k.k.......",
        ".......k........", "......k.k.......", ".....k...k......", "...kkk...kkk....",
        "..k..k...k..k...", "..k..k...k..k...", "...kk.....kk....", "................",
    ],
    "copy": [
        "................", "..kkkkkkkk......", "..kwwwwwwk......", "..kwwwwwwk......",
        "..kwwkkkkkkkkk..", "..kwwkwwwwwwwk..", "..kwwkwwwwwwwk..", "..kwwkwwwwwwwk..",
        "..kwwkwwwwwwwk..", "..kkkkwwwwwwwk..", ".....kwwwwwwwk..", ".....kwwwwwwwk..",
        ".....kwwwwwwwk..", ".....kwwwwwwwk..", ".....kkkkkkkkk..", "................",
    ],
    "paste": [
        "................", "......kkkk......", "..kkkkkggkkkkk..", "..kwwwkkkkwwwk..",
        "..kwwwwwwwwwwk..", "..kwwkkkkkkkkkk.", "..kwwkwwwwwwwwk.", "..kwwkwwwwwwwwk.",
        "..kwwkwwwwwwwwk.", "..kwwkwwwwwwwwk.", "..kkkkwwwwwwwwk.", ".....kwwwwwwwwk.",
        ".....kwwwwwwwwk.", ".....kwwwwwwwwk.", ".....kkkkkkkkkk.", "................",
    ],
    "quit": [
        "................", "....kkkkkkkk....", "...krrrrrrrrk...", "..krrrrrrrrrrk..",
        ".krrrkrrrrkrrrk.", ".krrrrkrrkrrrrk.", ".krrrrrkkrrrrrk.", ".krrrrrkkrrrrrk.",
        ".krrrrkrrkrrrrk.", ".krrrkrrrrkrrrk.", "..krrrrrrrrrrk..", "...krrrrrrrrk...",
        "....kkkkkkkk....", "................", "................", "................",
    ],
    "folder": [
        "................", "................", "..kkkkk.........", ".kyyyyykkkkkkkk.",
        ".kyyyyyyyyyyyyk.", ".kyyyyyyyyyyyyk.", ".kyyyyyyyyyyyyk.", ".kyyyyyyyyyyyyk.",
        ".kyyyyyyyyyyyyk.", ".kyyyyyyyyyyyyk.", ".kyyyyyyyyyyyyk.", ".kyyyyyyyyyyyyk.",
        ".kkkkkkkkkkkkkk.", "................", "................", "................",
    ],
    "file": [
        "................", "...kkkkkkkk.....", "...kwwwwwwkk....", "...kwwwwwwkwk...",
        "...kwwwwwwkkkk..", "...kwggggwwwwk..", "...kwwwwwwwwwk..", "...kwggggggwwk..",
        "...kwwwwwwwwwk..", "...kwggggggwwk..", "...kwwwwwwwwwk..", "...kwggggwwwwk..",
        "...kwwwwwwwwwk..", "...kwwwwwwwwwk..", "...kkkkkkkkkkk..", "................",
    ],
    "terminal": [
        "................", ".kkkkkkkkkkkkkk.", ".kkkkkkkkkkkkkk.", ".kwkkkkkkkkkkkk.",
        ".kkwkkkkkkkkkkk.", ".kkkwkkkkkkkkkk.", ".kkwkkkkkkkkkkk.", ".kwkkkkkkkkkkkk.",
        ".kkkkkkwwwwkkkk.", ".kkkkkkkkkkkkkk.", ".kkkkkkkkkkkkkk.", ".kkkkkkkkkkkkkk.",
        ".kkkkkkkkkkkkkk.", ".kkkkkkkkkkkkkk.", "................", "................",
    ],
    "clock": [
        "................", ".....kkkkkk.....", "...kkwwwwwwkk...", "..kwwwwkwwwwwk..",
        ".kwwwwwkwwwwwwk.", ".kwwwwwkwwwwwwk.", "kwwwwwwkwwwwwwwk", "kwwwwwwkkkkwwwwk",
        "kwwwwwwwwwwwwwwk", "kwwwwwwwwwwwwwwk", ".kwwwwwwwwwwwwk.", ".kwwwwwwwwwwwwk.",
        "..kwwwwwwwwwwk..", "...kkwwwwwwkk...", ".....kkkkkk.....", "................",
    ],
    "paint": [
        "................", "..........kkk...", ".........krrrk..", "........krrrrk..",
        ".......krrrrk...", "......krrrrk....", ".....krrrrk.....", "....krrrrk......",
        "...kyyrrk.......", "..kyyyyk........", ".kyyyyk.........", ".kyyyk..........",
        ".kkkk...........", "................", "................", "................",
    ],
    "pong": [
        "................", ".kk.............", ".kk.............", ".kk......ww.....",
        ".kk.....wwww....", ".kk......ww.....", ".kk.............", "................",
        "................", "...........kk...", "...........kk...", "...........kk...",
        "...........kk...", "...........kk...", "...........kk...", "................",
    ],
    "search": [
        "................", ".....kkkkk......", "...kkwwwwwkk....", "..kwwwwwwwwwk...",
        "..kwwwwwwwwwk...", ".kwwwwwwwwwwwk..", ".kwwwwwwwwwwwk..", ".kwwwwwwwwwwwk..",
        "..kwwwwwwwwwk...", "..kwwwwwwwwwk...", "...kkwwwwwkkk...", ".....kkkkk.kkk..",
        "............kkk.", ".............kkk", "..............kk", "................",
    ],
    "up": [
        "................", ".......kk.......", "......kbbk......", ".....kbbbbk.....",
        "....kbbbbbbk....", "...kbbbbbbbbk...", "..kbbbbbbbbbbk..", ".kkkkkbbbbkkkkk.",
        ".....kbbbbk.....", ".....kbbbbk.....", ".....kbbbbk.....", ".....kbbbbk.....",
        ".....kbbbbk.....", ".....kkkkkk.....", "................", "................",
    ],
    "back": [
        "................", "................", ".......kk.......", "......kbbk......",
        ".....kbbbk......", "....kbbbbkkkkkk.", "...kbbbbbbbbbbk.", "..kbbbbbbbbbbbk.",
        "..kbbbbbbbbbbbk.", "...kbbbbbbbbbbk.", "....kbbbbkkkkkk.", ".....kbbbk......",
        "......kbbk......", ".......kk.......", "................", "................",
    ],
    "forward": [
        "................", "................", ".......kk.......", "......kbbk......",
        "......kbbbk.....", ".kkkkkkbbbbk....", ".kbbbbbbbbbbk...", ".kbbbbbbbbbbbk..",
        ".kbbbbbbbbbbbk..", ".kbbbbbbbbbbk...", ".kkkkkkbbbbk....", "......kbbbk.....",
        "......kbbk......", ".......kk.......", "................", "................",
    ],
    "home": [
        "................", ".......kk.......", "......kbbk......", ".....kbbbbk.....",
        "....kbbbbbbk....", "...kbbbbbbbbk...", "..kbbbbbbbbbbk..", ".kkkbbbbbbbbkkk.",
        "...kbbbbbbbbk...", "...kbbbkkbbbk...", "...kbbbkwkbbk...", "...kbbbkwkbbk...",
        "...kbbbkwkbbk...", "...kkkkkkkkkk...", "................", "................",
    ],
    "refresh": [
        "................", ".....kkkkkk.....", "...kkbbbbbbkk...", "..kbbkkkkkkbbk..",
        ".kbbk......kbbk.", ".kbk.....kkkbbbk", ".kbk.....kbbbbbk", ".........kkkkkkk",
        "kkkkkkk.........", "kbbbbbk.....kbk.", "kbbbkkk.....kbk.", ".kbbk......kbbk.",
        "..kbbkkkkkkbbk..", "...kkbbbbbbkk...", ".....kkkkkk.....", "................",
    ],
    "edit": [
        "................", "...........kkk..", "..........kbbbk.", ".........kbbbbk.",
        "........kbbbbk..", ".......kbbbbk...", "......kbbbbk....", ".....kbbbbk.....",
        "....kbbbbk......", "...kbbbbk.......", "..kbbbbk........", ".kwwbbk.........",
        ".kwwwk..........", ".kkkk...........", "................", "................",
    ],
}


def png_chunk(tag, body):
    return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xffffffff)


def filter_row(ftype, row, prev, bpp):
    out = bytearray()
    for i, x in enumerate(row):
        a = row[i - bpp] if i >= bpp else 0
        b = prev[i] if prev else 0
        c = prev[i - bpp] if prev and i >= bpp else 0
        if ftype == 0:
            v = x
        elif ftype == 1:
            v = x - a
        elif ftype == 2:
            v = x - b
        elif ftype == 3:
            v = x - (a + b) // 2
        else:
            p = a + b - c
            pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
            pred = a if pa <= pb and pa <= pc else b if pb <= pc else c
            v = x - pred
        out.append(v & 0xff)
    return bytes(out)


def write_png(path, w, h, ctype, rows, palette=None, trns=None, level=9):
    bpp = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    raw = bytearray()
    prev = None
    for y, row in enumerate(rows):
        f = y % 5
        raw.append(f)
        raw += filter_row(f, row, prev, bpp)
        prev = row
    data = b"\x89PNG\r\n\x1a\n"
    data += png_chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, ctype, 0, 0, 0))
    if palette is not None:
        data += png_chunk(b"PLTE", b"".join(bytes(c[:3]) for c in palette))
    if trns is not None:
        data += png_chunk(b"tRNS", trns)
    data += png_chunk(b"IDAT", zlib.compress(bytes(raw), level))
    data += png_chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(data)


ADAM7 = [(0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)]


def pack_row(samples, depth):
    """A row of samples (ints below 2**depth) as PNG bytes."""
    if depth == 8:
        return bytes(samples)
    if depth == 16:
        return b"".join(struct.pack(">H", v) for v in samples)
    out = bytearray()
    acc, n = 0, 0
    for v in samples:
        acc = acc << depth | v
        n += depth
        if n == 8:
            out.append(acc)
            acc, n = 0, 0
    if n:
        out.append(acc << (8 - n))
    return bytes(out)


def write_png_depth(path, w, h, ctype, depth, pixels, interlace=False, palette=None):
    """pixels[y][x] is a tuple of samples; rows are filtered in turn
    with the five filters, separately in each Adam7 pass."""
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    bpp = max(1, channels * depth // 8)
    passes = ADAM7 if interlace else [(0, 0, 1, 1)]
    raw = bytearray()
    n = 0
    for x0, y0, dx, dy in passes:
        prev = None
        for y in range(y0, h, dy):
            row = pack_row([v for x in range(x0, w, dx) for v in pixels[y][x]], depth)
            if not row:
                continue
            f = n % 5
            n += 1
            raw.append(f)
            raw += filter_row(f, row, prev, bpp)
            prev = row
    data = b"\x89PNG\r\n\x1a\n"
    data += png_chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, depth, ctype, 0, 0, 1 if interlace else 0))
    if palette is not None:
        data += png_chunk(b"PLTE", b"".join(bytes(c[:3]) for c in palette))
    data += png_chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    data += png_chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(data)


def gen_icons(out):
    os.makedirs(out, exist_ok=True)
    for name, art in ICONS.items():
        rows = [bytes(sum((PALETTE[ch] for ch in line), ())) for line in art]
        write_png(os.path.join(out, name + ".png"), 16, 16, 6, rows)


def gen_tests(out):
    os.makedirs(out, exist_ok=True)
    w, h = 37, 23
    # RGBA: r = x*7, g = y*11, b = (x+y)*3, a = 255 - x*2 (all mod 256).
    rows = [bytes(sum((((x * 7) & 255, (y * 11) & 255, ((x + y) * 3) & 255, (255 - x * 2) & 255) for x in range(w)), ()))
            for y in range(h)]
    write_png(os.path.join(out, "rgba.png"), w, h, 6, rows)
    rows = [bytes(sum((((x * 7) & 255, (y * 11) & 255, ((x + y) * 3) & 255) for x in range(w)), ())) for y in range(h)]
    write_png(os.path.join(out, "rgb.png"), w, h, 2, rows, trns=struct.pack(">HHH", 0, 0, 0))
    rows = [bytes(((x * 5 + y * 3) & 255) for x in range(w)) for y in range(h)]
    write_png(os.path.join(out, "grey.png"), w, h, 0, rows)
    rows = [bytes(sum((((x * 5 + y * 3) & 255, (x * 9) & 255) for x in range(w)), ())) for y in range(h)]
    write_png(os.path.join(out, "greya.png"), w, h, 4, rows)
    palette = [(i * 3 & 255, i * 5 & 255, i * 7 & 255) for i in range(16)]
    rows = [bytes(((x + y) % 16) for x in range(w)) for y in range(h)]
    write_png(os.path.join(out, "pal.png"), w, h, 3, rows, palette=palette, trns=bytes([0, 255, 128] + [255] * 13))
    # A stored (uncompressed) deflate stream.
    write_png(os.path.join(out, "stored.png"), w, h, 0, [bytes(((x * 5 + y * 3) & 255) for x in range(w)) for y in range(h)], level=0)
    # Other bit depths and Adam7 interlacing.
    write_png_depth(os.path.join(out, "grey4.png"), w, h, 0, 4, [[((x + y) % 16,) for x in range(w)] for y in range(h)])
    write_png_depth(os.path.join(out, "pal2.png"), w, h, 3, 2, [[((x * 3 + y) % 4,) for x in range(w)] for y in range(h)],
                    palette=[(10, 20, 30), (40, 50, 60), (70, 80, 90), (100, 110, 120)])
    write_png_depth(os.path.join(out, "rgba16.png"), w, h, 6, 16,
                    [[(x * 1000, y * 2000, (x + y) * 300, 65535 - x * 500) for x in range(w)] for y in range(h)])
    write_png_depth(os.path.join(out, "adam7.png"), w, h, 6, 8,
                    [[((x * 7) & 255, (y * 11) & 255, ((x + y) * 3) & 255, (255 - x * 2) & 255) for x in range(w)]
                     for y in range(h)], interlace=True)
    write_png_depth(os.path.join(out, "grey1i.png"), w, h, 0, 1, [[((x ^ y) & 1,) for x in range(w)] for y in range(h)],
                    interlace=True)


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] not in ("icons", "tests"):
        sys.exit(__doc__)
    (gen_icons if sys.argv[1] == "icons" else gen_tests)(sys.argv[2])
