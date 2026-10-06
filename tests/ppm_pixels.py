"""Pixels of a binary PPM image (P6, 8 bits per channel), for the post
scripts of the boot cases that check a QEMU screendump.

    import sys, os
    sys.path.insert(0, os.path.join(os.environ["TOP"], "tests"))
    import ppm_pixels
    image = ppm_pixels.read(path)
    image.pixel(x, y)       # 0xRRGGBB
"""


class Image:
    def __init__(self, width, height, data):
        self.width = width
        self.height = height
        self.data = data

    def pixel(self, x, y):
        i = (y * self.width + x) * 3
        return self.data[i] << 16 | self.data[i + 1] << 8 | self.data[i + 2]


def read(path):
    with open(path, "rb") as f:
        raw = f.read()
    fields = []
    i = 0
    # The header: P6, width, height and the maximum value, separated by
    # white space, with comments from # to the end of a line.
    while len(fields) < 4:
        while raw[i:i + 1].isspace():
            i += 1
        if raw[i:i + 1] == b"#":
            while raw[i:i + 1] not in (b"\n", b""):
                i += 1
            continue
        start = i
        while not raw[i:i + 1].isspace():
            i += 1
        fields.append(raw[start:i])
    if fields[0] != b"P6" or int(fields[3]) != 255:
        raise ValueError(f"{path}: not an 8 bit P6 image")
    width, height = int(fields[1]), int(fields[2])
    data = raw[i + 1:i + 1 + width * height * 3]
    if len(data) != width * height * 3:
        raise ValueError(f"{path}: truncated")
    return Image(width, height, data)
