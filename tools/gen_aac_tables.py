#!/usr/bin/env python3
"""Generate lib/libcodec/modules/aac/tables.c from FFmpeg's aactab.c.

    tools/gen_aac_tables.py PATH/TO/libavcodec/aactab.c

The AAC decoder of minios was written for minios, but it needs the
normative tables of ISO/IEC 14496-3. These are the Huffman codebooks for
scale factors and spectral data, the scale factor band offsets for each
sample rate, and the TNS band limits. The owner decided on 2026-10-06 to
take the tables from FFmpeg 7.1 (libavcodec/aactab.c), which transcribes
them from the standard. This script extracts exactly those arrays and
checks their sizes against the standard. It also checks that every
codebook is a complete prefix code. It then writes the tables in the form
that the decoder uses.

The script prints nothing on success. The generated file is committed, so
the build does not need FFmpeg."""

import os
import re
import sys

TOP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(TOP, "lib", "libcodec", "modules", "aac", "tables.c")

# The sample rate index of ISO/IEC 14496-3 selects one of the FFmpeg arrays
# of band offsets. The rates 96 and 88.2 kHz share a table, as do 48 and
# 44.1 kHz, 24 and 22.05 kHz, and 16, 12 and 11.025 kHz.
LONG = ["96", "96", "64", "48", "48", "32", "24", "24", "16", "16", "16", "8", "8"]
SHORT = ["96", "96", "96", "48", "48", "48", "24", "24", "16", "16", "16", "8", "8"]

# Codebook sizes (Annex 4.A of the standard). Codebooks 1 to 4 have four
# dimensions of 3 values each. Codebooks 5 and 6 have two dimensions of 9
# values, codebooks 7 and 8 have two dimensions of 8 values, codebooks 9
# and 10 have two dimensions of 13 values, and codebook 11 has two
# dimensions of 17 values.
SPECTRAL_SIZES = [81, 81, 81, 81, 81, 81, 64, 64, 169, 169, 289]


def array(text, name):
    """Return the numbers of the C array called name."""
    m = re.search(r"\b%s\s*\[[^\]]*\]\s*=\s*\{(.*?)\};" % re.escape(name), text, re.S)
    if not m:
        sys.exit("gen_aac_tables: array %s not found" % name)
    body = re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.S)
    return [int(v, 0) for v in re.findall(r"0x[0-9a-fA-F]+|\d+", body)]


def check_prefix_code(name, codes, bits):
    """Check that the codes form a complete prefix code. Every code must be
    distinct, and no code may be a prefix of another. The code lengths must
    fill the code space exactly (Kraft sum 1)."""
    words = sorted((format(c, "0%db" % b), i) for i, (c, b) in enumerate(zip(codes, bits)))
    for (a, _), (b, _) in zip(words, words[1:]):
        if b.startswith(a):
            sys.exit("gen_aac_tables: %s: %s is a prefix of %s" % (name, a, b))
    if sum(2.0 ** -b for b in bits) != 1.0:
        sys.exit("gen_aac_tables: %s is not a complete prefix code" % name)


def c_array(ctype, name, values, per_line=12):
    lines = []
    for i in range(0, len(values), per_line):
        lines.append("    " + ", ".join(str(v) for v in values[i:i + per_line]) + ",")
    return "static const %s %s[%d] = {\n%s\n};\n" % (ctype, name, len(values), "\n".join(lines))


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__.split("\n\n")[1])
    text = open(sys.argv[1]).read()
    out = []
    sf_code = array(text, "ff_aac_scalefactor_code")
    sf_bits = array(text, "ff_aac_scalefactor_bits")
    if len(sf_code) != 121 or len(sf_bits) != 121:
        sys.exit("gen_aac_tables: the scale factor codebook must have 121 entries")
    check_prefix_code("scale factor codebook", sf_code, sf_bits)
    out.append(c_array("uint32_t", "sf_code", sf_code, 8))
    out.append(c_array("uint8_t", "sf_bits", sf_bits, 16))
    for n in range(1, 12):
        codes, bits = array(text, "codes%d" % n), array(text, "bits%d" % n)
        if len(codes) != SPECTRAL_SIZES[n - 1] or len(bits) != len(codes):
            sys.exit("gen_aac_tables: spectral codebook %d has %d entries" % (n, len(codes)))
        check_prefix_code("spectral codebook %d" % n, codes, bits)
        out.append(c_array("uint16_t", "codes%d" % n, codes, 12))
        out.append(c_array("uint8_t", "bits%d" % n, bits, 16))
    offsets = {}
    for kind, rates in (("1024", LONG), ("128", SHORT)):
        for rate in sorted(set(rates), key=int, reverse=True):
            values = array(text, "swb_offset_%s_%s" % (kind, rate))
            if values[0] != 0 or values[-1] != int(kind) or values != sorted(set(values)):
                sys.exit("gen_aac_tables: swb_offset_%s_%s is not an increasing list from 0 to %s" %
                         (kind, rate, kind))
            offsets[(kind, rate)] = values
            out.append(c_array("uint16_t", "swb_long_%s" % rate if kind == "1024" else "swb_short_%s" % rate,
                               values, 12))
    tns_long = array(text, "ff_tns_max_bands_1024")
    tns_short = array(text, "ff_tns_max_bands_128")
    if len(tns_long) != 13 or len(tns_short) != 13:
        sys.exit("gen_aac_tables: the TNS band limits must have 13 entries")

    body = []
    body.append("const struct aac_codebook aac_sf_codebook = { sf_code, NULL, sf_bits, 121 };\n")
    body.append("const struct aac_codebook aac_spectral_codebooks[11] = {\n")
    for n in range(1, 12):
        body.append("    { NULL, codes%d, bits%d, %d },\n" % (n, n, SPECTRAL_SIZES[n - 1]))
    body.append("};\n\n")
    body.append("const struct aac_bands aac_bands[13] = {\n")
    for i in range(13):
        lo, so = offsets[("1024", LONG[i])], offsets[("128", SHORT[i])]
        body.append("    { swb_long_%s, %d, swb_short_%s, %d, %d, %d },\n" %
                    (LONG[i], len(lo) - 1, SHORT[i], len(so) - 1, tns_long[i], tns_short[i]))
    body.append("};\n")

    with open(OUT, "w") as f:
        f.write("""/* Generated by tools/gen_aac_tables.py from libavcodec/aactab.c of FFmpeg
 * 7.1. Do not edit.
 *
 * The file contains normative tables of ISO/IEC 14496-3: the scale factor
 * codebook, the spectral codebooks 1 to 11 (Annex 4.A), the scale factor
 * band offsets of long and short windows for each sample rate index, and
 * the TNS band limits. FFmpeg is licensed under the LGPL 2.1. The tables
 * are data that the standard defines. */
#include "aac.h"

""")
        f.write("\n".join(out))
        f.write("\n")
        f.write("".join(body))


if __name__ == "__main__":
    main()
