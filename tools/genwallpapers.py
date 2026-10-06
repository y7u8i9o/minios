#!/usr/bin/env python3
"""Generate the wallpapers of user/share/wallpapers (docs/design/desktop.md).

Usage: tools/genwallpapers.py [DIR]

The two designs of the system are drawn at 2560x1600 pixels, the largest
mode of the virtio-gpu driver, as RGB PNG files in DIR (by default
user/share/wallpapers):

    default.png  a vertical gradient from (24, 56, 112) to (40, 104, 176)
                 with a band along the diagonal from the top left to the
                 bottom right corner, 1/8 of the width wide, that adds 32
                 to each channel
    dusk.png     a vertical gradient from (96, 48, 96) to (192, 88, 80)

The gradients are dithered with a 4x4 ordered pattern, so that the steps
of 8 bit colour do not appear as bands. The edges of the band are
smoothed by the covered fraction of each pixel along the row. The designs
are those of the former files of 320x240 pixels.
"""
import os
import struct
import sys
import zlib

W, H = 2560, 1600
BAYER = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]


def write_png(path, rows):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    raw = b"".join(b"\x00" + bytes(row) for row in rows)
    data = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(data)


def dither(value, x, y):
    """The 8 bit value of a channel value in 0..255 with ordered dithering."""
    v = int(value + (BAYER[y % 4][x % 4] + 0.5) / 16)
    return 0 if v < 0 else 255 if v > 255 else v


def gradient(top, bottom, y):
    t = y / (H - 1)
    return [top[k] + (bottom[k] - top[k]) * t for k in range(3)]


def default_rows():
    # The band covers |x - y * W / H| <= HALF along each row.
    half = W / 16
    for y in range(H):
        base = gradient((24, 56, 112), (40, 104, 176), y)
        centre = y * W / H
        row = bytearray(W * 3)
        for x in range(W):
            # The fraction of the pixel [x, x + 1) inside the band.
            lo, hi = max(x, centre - half), min(x + 1, centre + half)
            cover = hi - lo if hi > lo else 0.0
            for k in range(3):
                row[x * 3 + k] = dither(base[k] + 32 * cover, x, y)
        yield row


def dusk_rows():
    for y in range(H):
        base = gradient((96, 48, 96), (192, 88, 80), y)
        row = bytearray(W * 3)
        for x in range(W):
            for k in range(3):
                row[x * 3 + k] = dither(base[k], x, y)
        yield row


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "user", "share",
                                                               "wallpapers")
    write_png(os.path.join(out, "default.png"), list(default_rows()))
    write_png(os.path.join(out, "dusk.png"), list(dusk_rows()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
