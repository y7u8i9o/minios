#!/bin/sh
# Download the Unihan database of Unicode 16.0 and extract the files that
# tools/genime.py reads into third_party/unihan. Run from any directory;
# needs curl, unzip and network access.
#
#   tools/fetch_unihan.sh           downloads what is missing
#   tools/fetch_unihan.sh -f        downloads everything again
set -e
TOP="$(cd "$(dirname "$0")/.." && pwd)"
DIR="$TOP/third_party/unihan"
URL="https://www.unicode.org/Public/16.0.0/ucd/Unihan.zip"
mkdir -p "$DIR"
if [ "$1" = -f ] || [ ! -s "$DIR/Unihan_Readings.txt" ]; then
    echo "fetch   $URL"
    curl -fsSL --retry 3 -o "$DIR/Unihan.zip.tmp" "$URL"
    unzip -o -q "$DIR/Unihan.zip.tmp" Unihan_Readings.txt Unihan_OtherMappings.txt Unihan_Variants.txt -d "$DIR"
    rm -f "$DIR/Unihan.zip.tmp"
fi
[ -s "$DIR/LICENSE.txt" ] || cp "$TOP/third_party/unicode/LICENSE.txt" "$DIR/LICENSE.txt"
echo "done: $DIR"
