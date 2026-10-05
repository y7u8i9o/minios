#!/bin/sh
# Download the MP3 decoder minimp3 of Lieff (CC0) and the MP3 encoder
# shine (GNU Library GPL version 2) at fixed commits into third_party.
# Run from the repository root:
#
#     tools/fetch_mp3.sh
#
# The module lib/libcodec/modules/mp3 compiles both. minimp3 is used
# without changes. shine receives tools/patches/shine.patch with two
# corrections: the bytes of a frame are computed exactly, and stuffing bits
# that the granules cannot take are written as ancillary data
# (docs/design/codecs.md, MP3).
set -eu

TOP="$(cd "$(dirname "$0")/.." && pwd)"
MINIMP3=ea99364f61c14656440e8d77e9c233ccf3124633
SHINE=ab5e3526b64af1a2eaa43aa6f441a7312e013519

get() {
    curl -fL --max-time 60 -o "$1" "$2"
}

dir="$TOP/third_party/minimp3"
mkdir -p "$dir"
base="https://raw.githubusercontent.com/lieff/minimp3/$MINIMP3"
for f in minimp3.h minimp3_ex.h LICENSE; do get "$dir/$f" "$base/$f"; done
echo "https://github.com/lieff/minimp3 $MINIMP3" > "$dir/ORIGIN"

dir="$TOP/third_party/shine"
mkdir -p "$dir"
base="https://raw.githubusercontent.com/toots/shine/$SHINE"
for f in bitstream.c bitstream.h huffman.c huffman.h l3bitstream.c l3bitstream.h l3loop.c l3loop.h \
         l3mdct.c l3mdct.h l3subband.c l3subband.h layer3.c layer3.h mult_noarch_gcc.h \
         reservoir.c reservoir.h tables.c tables.h types.h; do
    get "$dir/$f" "$base/src/lib/$f"
done
get "$dir/COPYING" "$base/COPYING"
patch -s -d "$dir" -p1 < "$TOP/tools/patches/shine.patch"
printf 'https://github.com/toots/shine %s\nwith tools/patches/shine.patch\n' "$SHINE" > "$dir/ORIGIN"
