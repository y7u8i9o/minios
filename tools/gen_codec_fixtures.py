#!/usr/bin/env python3
"""Create the audio fixtures of the codec tests (docs/plan/codecs.md).

    tools/gen_codec_fixtures.py

The script synthesises deterministic signals, encodes them with the
reference tools of the host and writes the results to user/etc/tests,
from where the build installs them in /etc/tests. It needs flac, ffmpeg,
a C compiler, pkg-config and libvorbis. Every FLAC fixture is checked
with `flac -t`, whose MD5 test verifies the file against the samples it
encodes, and the boot test codec_flac verifies the decoder of minios
against the same sums.

The Ogg FLAC fixtures come from flac --ogg and from the Ogg muxer of
ffmpeg, and are checked with `flac -t` as well.

The Vorbis fixtures are encoded by libvorbis through tools/codecref/
vorbisref.c, and the same program decodes them with libvorbisfile into
16 bit samples, stored as FLAC files named *.ref.flac. The boot test
codec_vorbis compares the decoder of minios with those samples.

The FLAC fixtures cover the paths that the encoder of minios does not
write: LPC up to order 32 from the reference encoder, every sample rate
code, 8, 12, 20, 24 and 32 bit samples, eight channels, wasted bits,
variable block sizes and an ID3v2 tag before the stream."""

import math
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile

TOP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(TOP, "user", "etc", "tests")


def need(tool):
    if not shutil.which(tool):
        sys.exit("gen_codec_fixtures: %s is needed" % tool)


def run(*args):
    subprocess.run(args, check=True, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)


# ---- signals ----

def signal(frames, channels, bits, rate, seed, wasted=0):
    """Interleaved integer samples: a chord that differs per channel, a
    chirp, noise, a stretch of digital silence and a full scale square,
    which together exercise every subframe type."""
    rnd = random.Random(seed)
    top = (1 << (bits - 1)) - 1
    out = []
    for i in range(frames):
        t = i / rate
        part = i * 5 // max(frames, 1)
        for c in range(channels):
            if part == 0:
                v = 0.45 * math.sin(2 * math.pi * (220 + 55 * c) * t) + 0.3 * math.sin(2 * math.pi * 1375 * t)
            elif part == 1:
                v = 0.6 * math.sin(2 * math.pi * (100 + 4000 * t) * t * (1 + c * 0.1))
            elif part == 2:
                v = rnd.uniform(-0.5, 0.5)
            elif part == 3:
                v = 0.0
            else:
                v = 1.0 if (i // 40) % 2 else -1.0
            s = max(-top - 1, min(top, int(round(v * top))))
            if wasted:
                s = (s >> wasted) << wasted
            out.append(s)
    return out


def write_wav(path, samples, channels, bits, rate):
    width = (bits + 7) // 8
    data = bytearray()
    for s in samples:
        if width == 1:
            data.append((s + 128) & 0xff)
        else:
            data += (s & ((1 << (8 * width)) - 1)).to_bytes(width, "little")
    fmt = struct.pack("<HHIIHH", 1, channels, rate, rate * channels * width, channels * width, 8 * width)
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE")
        f.write(b"fmt " + struct.pack("<I", 16) + fmt)
        f.write(b"data" + struct.pack("<I", len(data)) + data)


def write_wav_valid_bits(path, samples, channels, bits, container, rate):
    """A WAVE_FORMAT_EXTENSIBLE file whose samples have bits valid bits in
    containers of container bits, the input from which flac writes 12 and
    20 bit streams."""
    data = bytearray()
    for s in samples:
        data += ((s << (container - bits)) & ((1 << container) - 1)).to_bytes(container // 8, "little")
    pcm = struct.pack("<IHH", 1, 0, 0x10) + bytes([0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71])
    fmt = struct.pack("<HHIIHHHHI", 0xfffe, channels, rate, rate * channels * container // 8,
                      channels * container // 8, container, 22, bits, 0) + pcm
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 4 + 8 + len(fmt) + 8 + len(data)) + b"WAVE")
        f.write(b"fmt " + struct.pack("<I", len(fmt)) + fmt)
        f.write(b"data" + struct.pack("<I", len(data)) + data)


def write_raw(path, samples, bits):
    width = (bits + 7) // 8
    with open(path, "wb") as f:
        for s in samples:
            f.write((s & ((1 << (8 * width)) - 1)).to_bytes(width, "little"))


# ---- FLAC frames, for the variable block size stream ----

def crc8(data):
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xff if crc & 0x80 else (crc << 1) & 0xff
    return crc


def crc16(data):
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x8005) & 0xffff if crc & 0x8000 else (crc << 1) & 0xffff
    return crc


def coded_number(v):
    if v < 0x80:
        return bytes([v])
    extra = 1
    while extra < 6 and v >= 1 << (5 * extra + 6):
        extra += 1
    first = ((0xff00 >> (extra + 1)) & 0xff) | (v >> (6 * extra))
    return bytes([first] + [0x80 | ((v >> (6 * i)) & 0x3f) for i in range(extra - 1, -1, -1)])


def header_length(data, at):
    """The length of the frame header at data[at], including its CRC-8."""
    p = at + 4
    first = data[p]
    n = 1
    if first >= 0xc0:
        while first & (0x80 >> n):
            n += 1
    p += n
    bs, rate = data[at + 2] >> 4, data[at + 2] & 15
    p += 1 if bs == 6 else 2 if bs == 7 else 0
    p += 1 if rate == 12 else 2 if rate in (13, 14) else 0
    return p + 1 - at


def frames_of(path):
    """The frames of a FLAC file: the bytes after the metadata, split at
    the headers whose CRC-8 matches and whose frame CRC-16 matches."""
    data = open(path, "rb").read()
    at = 4
    while True:
        last, n = data[at] >> 7, int.from_bytes(data[at + 1:at + 4], "big")
        at += 4 + n
        if last:
            break
    starts = []
    i = at
    while i + 4 < len(data):
        if data[i] == 0xff and data[i + 1] & 0xfe == 0xf8:
            try:
                hl = header_length(data, i)
                if crc8(data[i:i + hl - 1]) == data[i + hl - 1]:
                    starts.append(i)
            except IndexError:
                pass
        i += 1
    starts.append(len(data))
    frames = []
    a = 0
    while a < len(starts) - 1:
        for b in range(a + 1, len(starts)):
            frame = data[starts[a]:starts[b]]
            if crc16(frame[:-2]) == int.from_bytes(frame[-2:], "big"):
                frames.append(frame)
                a = b
                break
        else:
            sys.exit("gen_codec_fixtures: cannot split %s" % path)
    return data[:at], frames


def variable_stream(parts, path, total, md5):
    """Join the frames of several fixed block size files into one stream
    with the variable blocking strategy, in which each header carries the
    number of its first sample."""
    out = bytearray()
    sample = 0
    sizes = []
    for head, frames, block in parts:
        for i, frame in enumerate(frames):
            n = block if i < len(frames) - 1 else None
            hl = header_length(frame, 0)
            bs = frame[2] >> 4
            p = 4
            first = frame[p]
            k = 1
            if first >= 0xc0:
                while first & (0x80 >> k):
                    k += 1
            rest = frame[4 + k:hl - 1]
            if bs == 6:
                n = rest[0] + 1
            elif bs == 7:
                n = int.from_bytes(rest[0:2], "big") + 1
            else:
                n = [0, 192, 576, 1152, 2304, 4608, 0, 0, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768][bs]
            header = bytearray(frame[0:4])
            header[1] |= 1
            header += coded_number(sample) + rest
            header.append(crc8(header))
            body = bytearray(header) + frame[hl:-2]
            body += crc16(body).to_bytes(2, "big")
            out += body
            sizes.append(n)
            sample += n
    head = bytearray(parts[0][0])
    info = 8
    sizes_but_last = sizes[:-1] or sizes
    head[info:info + 2] = min(sizes_but_last).to_bytes(2, "big")
    head[info + 2:info + 4] = max(sizes).to_bytes(2, "big")
    head[info + 4:info + 10] = bytes(6)
    packed = int.from_bytes(head[info + 10:info + 18], "big")
    packed = (packed & ~((1 << 36) - 1)) | total
    head[info + 10:info + 18] = packed.to_bytes(8, "big")
    head[info + 18:info + 34] = md5
    with open(path, "wb") as f:
        f.write(head + out)


def md5_of(samples, bits):
    import hashlib
    width = (bits + 7) // 8
    return hashlib.md5(b"".join((s & ((1 << (8 * width)) - 1)).to_bytes(width, "little")
                               for s in samples)).digest()


# ---- the fixtures ----

def flac_fixtures(tmp):
    made = []

    def wav(name, frames, channels, bits, rate, seed, wasted=0):
        s = signal(frames, channels, bits, rate, seed, wasted)
        path = os.path.join(tmp, name + ".wav")
        write_wav(path, s, channels, bits, rate)
        return path, s

    def flac(src, name, *options):
        dst = os.path.join(OUT, name)
        run("flac", "-f", "-s", "--no-padding", *options, "-o", dst, src)
        made.append(dst)

    def ffmpeg(src, name, *options):
        dst = os.path.join(OUT, name)
        run("ffmpeg", "-y", "-loglevel", "error", "-i", src, "-c:a", "flac", *options, dst)
        made.append(dst)

    src, _ = wav("stereo16", 22050, 2, 16, 44100, 1)
    flac(src, "codec-stereo16.flac", "-8", "-b", "4608")
    src, _ = wav("mono24", 14400, 1, 24, 48000, 2)
    flac(src, "codec-mono24-lpc32.flac", "--lax", "-l", "32", "-b", "1152", "-r", "8", "-p")
    src, _ = wav("u8", 6615, 3, 8, 22050, 3)
    flac(src, "codec-u8-3ch.flac", "-5", "--channel-map=none")
    src, _ = wav("eight", 1600, 8, 16, 8000, 4)
    flac(src, "codec-8ch.flac", "-8", "--channel-map=none")
    src, _ = wav("wasted", 4000, 2, 16, 32000, 5, wasted=3)
    flac(src, "codec-wasted.flac", "-8")
    for rate, name in ((12000, "codec-rate12k.flac"), (11025, "codec-rate11025.flac"),
                       (37800, "codec-rate37800.flac"), (88200, "codec-rate88200.flac"),
                       (705600, "codec-rate705600.flac")):
        src, _ = wav("rate%d" % rate, 2000, 1, 16, rate, rate)
        flac(src, name, "--lax", "-6")
    s32 = signal(3000, 1, 32, 12000, 6)
    raw = os.path.join(tmp, "s32.raw")
    write_raw(raw, s32, 32)
    flac(raw, "codec-s32.flac", "--force-raw-format", "--endian=little", "--sign=signed", "--channels=1",
         "--bps=32", "--sample-rate=12000", "-8")
    src, _ = wav("ff24", 9600, 2, 24, 96000, 7)
    ffmpeg(src, "codec-ffmpeg24.flac", "-sample_fmt", "s32", "-compression_level", "12", "-lpc_type", "cholesky",
           "-prediction_order_method", "3", "-exact_rice_parameters", "1")
    for bits in (12, 20):
        src = os.path.join(tmp, "s%d.wav" % bits)
        write_wav_valid_bits(src, signal(3000, 2, bits, 16000, bits), 2, bits, 16 if bits == 12 else 24, 16000)
        flac(src, "codec-s%d.flac" % bits, "-8")

    # Variable block sizes: the first part at 1152, the second at 4096.
    frames = 11000
    s = signal(frames, 2, 16, 44100, 8)
    a, b = s[:2 * 5000], s[2 * 5000:]
    pa, pb = os.path.join(tmp, "va.wav"), os.path.join(tmp, "vb.wav")
    write_wav(pa, a, 2, 16, 44100)
    write_wav(pb, b, 2, 16, 44100)
    fa, fb = os.path.join(tmp, "va.flac"), os.path.join(tmp, "vb.flac")
    run("flac", "-f", "-s", "--no-padding", "-8", "-b", "1152", "-o", fa, pa)
    run("flac", "-f", "-s", "--no-padding", "-8", "-b", "4096", "-o", fb, pb)
    ha, fra = frames_of(fa)
    hb, frb = frames_of(fb)
    dst = os.path.join(OUT, "codec-variable.flac")
    variable_stream([(ha, fra, 1152), (hb, frb, 4096)], dst, frames, md5_of(s, 16))
    made.append(dst)

    # An ID3v2.4 tag with a text frame before the chime.
    chime = os.path.join(TOP, "user", "share", "sounds", "chime.flac")
    run("flac", "-f", "-s", "--no-padding", "-8", "-o", chime, os.path.join(TOP, "user", "share", "sounds", "chime.wav"))
    made.append(chime)
    text = b"\x03minios codec test"
    frame = b"TIT2" + bytes([0, 0, 0, len(text)]) + b"\x00\x00" + text
    size = len(frame)
    tag = b"ID3\x04\x00\x00" + bytes([(size >> 21) & 0x7f, (size >> 14) & 0x7f, (size >> 7) & 0x7f, size & 0x7f])
    dst = os.path.join(OUT, "codec-id3.flac")
    with open(dst, "wb") as f:
        f.write(tag + frame + open(chime, "rb").read())
    made.append(dst)

    for path in made:
        run("flac", "-t", "-s", path)
    return made


# ---- Ogg ----

def ogg_pages(path):
    """The pages of an Ogg file as (serial, flags, bytes)."""
    data = open(path, "rb").read()
    pages = []
    at = 0
    while at < len(data):
        if data[at:at + 4] != b"OggS":
            sys.exit("gen_codec_fixtures: %s is not a sequence of Ogg pages" % path)
        segments = data[at + 26]
        body = sum(data[at + 27:at + 27 + segments])
        size = 27 + segments + body
        pages.append((int.from_bytes(data[at + 14:at + 18], "little"), data[at + 5], data[at:at + size]))
        at += size
    return pages


def multiplex(paths, out):
    """Interleave the pages of several single stream files into one file:
    the first pages of all streams, then the other pages in turn."""
    streams = [ogg_pages(p) for p in paths]
    result = bytearray()
    for st in streams:
        result += st[0][2]
    rest = [st[1:] for st in streams]
    while any(rest):
        for r in rest:
            if r:
                result += r.pop(0)[2]
    open(out, "wb").write(result)


def reference_tool(tmp, name, packages):
    src = os.path.join(TOP, "tools", "codecref", name + ".c")
    exe = os.path.join(tmp, name)
    flags = subprocess.run(["pkg-config", "--cflags", "--libs"] + packages, check=True, capture_output=True,
                           text=True).stdout.split()
    subprocess.run([os.environ.get("CC", "cc"), "-O2", "-o", exe, src] + flags + ["-lm"], check=True)
    return exe


def raw_to_flac(raw, out, channels, rate):
    run("flac", "-f", "-s", "--no-padding", "-8", "--force-raw-format", "--endian=little", "--sign=signed",
        "--channels=%d" % channels, "--bps=16", "--sample-rate=%d" % rate, "--channel-map=none", "-o", out, raw)


def vorbis_fixtures(tmp):
    tool = reference_tool(tmp, "vorbisref", ["vorbisenc", "vorbisfile", "vorbis", "ogg"])
    made = []

    def encode(name, wav, quality, serial, out_dir=OUT):
        dst = os.path.join(out_dir, name)
        run(tool, "encode", wav, dst, str(quality), str(serial))
        return dst

    def reference(ogg, channels, rate, name=None):
        raw = os.path.join(tmp, os.path.basename(ogg) + ".raw")
        run(tool, "decode", ogg, raw)
        dst = os.path.join(OUT, (name or os.path.basename(ogg)[:-4]) + ".ref.flac")
        raw_to_flac(raw, dst, channels, rate)
        made.append(dst)

    def source(name, frames, channels, rate, seed):
        path = os.path.join(tmp, name + ".wav")
        write_wav(path, signal(frames, channels, 16, rate, seed), channels, 16, rate)
        return path

    chime_wav = os.path.join(TOP, "user", "share", "sounds", "chime.wav")
    chime = encode("chime.ogg", chime_wav, 0.3, 4001, os.path.join(TOP, "user", "share", "sounds"))
    made.append(chime)
    reference(chime, 1, 16000, "codec-vorbis-chime")
    for name, frames, channels, rate, quality, seed in (
            ("codec-vorbis-stereo.ogg", 66150, 2, 44100, 0.5, 11),
            ("codec-vorbis-51.ogg", 24000, 6, 48000, 0.2, 12),
            ("codec-vorbis-3ch.ogg", 8000, 3, 16000, 0.0, 13),
            ("codec-vorbis-low.ogg", 22050, 2, 22050, -0.1, 14),
            ("codec-vorbis-high.ogg", 19200, 2, 48000, 1.0, 15)):
        ogg = encode(name, source(name, frames, channels, rate, seed), quality, 4100 + seed)
        made.append(ogg)
        reference(ogg, channels, rate)

    # Two chained streams of one format.
    a = encode("chain-a.ogg", source("chain-a", 11025, 1, 22050, 21), 0.3, 4201, tmp)
    b = encode("chain-b.ogg", source("chain-b", 7000, 1, 22050, 22), 0.3, 4202, tmp)
    chained = os.path.join(OUT, "codec-vorbis-chained.ogg")
    open(chained, "wb").write(open(a, "rb").read() + open(b, "rb").read())
    made.append(chained)
    reference(chained, 1, 22050)

    # A Vorbis stream multiplexed with an Ogg FLAC stream.
    v = encode("mux-v.ogg", source("mux-v", 22050, 2, 22050, 31), 0.2, 4301, tmp)
    f = os.path.join(tmp, "mux-f.oga")
    run("flac", "-f", "-s", "--ogg", "--serial-number=4302", "-8", "-o", f, source("mux-f", 22050, 1, 22050, 32))
    mux = os.path.join(OUT, "codec-vorbis-mux.ogg")
    multiplex([f, v], mux)
    made.append(mux)
    reference(v, 2, 22050, "codec-vorbis-mux")

    for path in made:
        if path.endswith(".flac"):
            run("flac", "-t", "-s", path)
    return made


def oggflac_fixtures(tmp):
    """FLAC in Ogg from the reference flac and from ffmpeg's Ogg muxer, and
    two chained streams. The FLAC stream inside codec-vorbis-mux.ogg is
    a fourth case."""
    made = []

    def source(name, frames, channels, bits, rate, seed):
        path = os.path.join(tmp, name + ".wav")
        write_wav(path, signal(frames, channels, bits, rate, seed), channels, bits, rate)
        return path

    dst = os.path.join(OUT, "codec-oggflac-ref.oga")
    run("flac", "-f", "-s", "--ogg", "--serial-number=5001", "-8", "-o", dst, source("of1", 22050, 2, 16, 44100, 41))
    made.append(dst)
    dst = os.path.join(OUT, "codec-oggflac-ffmpeg.oga")
    run("ffmpeg", "-y", "-loglevel", "error", "-i", source("of2", 9600, 2, 24, 48000, 42), "-c:a", "flac",
        "-sample_fmt", "s32", "-f", "ogg", dst)
    made.append(dst)
    a, b = os.path.join(tmp, "ofa.oga"), os.path.join(tmp, "ofb.oga")
    run("flac", "-f", "-s", "--ogg", "--serial-number=5003", "-5", "-o", a, source("ofa", 8000, 1, 16, 16000, 43))
    run("flac", "-f", "-s", "--ogg", "--serial-number=5004", "-5", "-o", b, source("ofb", 5000, 1, 16, 16000, 44))
    dst = os.path.join(OUT, "codec-oggflac-chained.oga")
    open(dst, "wb").write(open(a, "rb").read() + open(b, "rb").read())
    made.append(dst)
    for path in made:
        run("flac", "-t", "-s", path)
    return made


def main():
    for tool in ("flac", "ffmpeg", "pkg-config"):
        need(tool)
    with tempfile.TemporaryDirectory() as tmp:
        for path in flac_fixtures(tmp) + vorbis_fixtures(tmp) + oggflac_fixtures(tmp):
            print("%7d %s" % (os.path.getsize(path), os.path.relpath(path, TOP)))


if __name__ == "__main__":
    main()
