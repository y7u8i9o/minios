#!/bin/sh
# Download the dictionaries of the input method engines of imed
# (docs/design/ime.md) at pinned commits into third_party/imedata, which
# tools/genime.py reads. Run from any directory; needs curl and network
# access.
#
#   tools/fetch_imedata.sh          downloads what is missing
#   tools/fetch_imedata.sh -f       downloads everything again
#
# rime-pinyin-simp (Apache-2.0): pinyin_simp.dict.yaml, the characters and
# words of simplified Chinese with their pinyin and weights.
# Mozc dictionary_oss (IPAdic license, public domain and BSD 3-clause, see
# mozc.README.txt): the readings, parts of speech and costs of Japanese
# words and the connection costs between the parts of speech.
set -e
TOP="$(cd "$(dirname "$0")/.." && pwd)"
DIR="$TOP/third_party/imedata"
RIME_PINYIN_SIMP=0c6861ef7420ee780270ca6d993d18d4101049d0
MOZC=c7538e6f8ee56ff94789494106ad5d6d658cd4f6
RAW=https://raw.githubusercontent.com
mkdir -p "$DIR"

fetch() {
    url="$1" out="$DIR/$2"
    if [ "$FORCE" = 1 ] || [ ! -s "$out" ]; then
        echo "fetch   $url"
        curl -fsSL --retry 3 -o "$out.tmp" "$url"
        mv "$out.tmp" "$out"
    fi
}

FORCE=0
[ "$1" = -f ] && FORCE=1
fetch "$RAW/rime/rime-pinyin-simp/$RIME_PINYIN_SIMP/pinyin_simp.dict.yaml" pinyin_simp.dict.yaml
fetch "$RAW/rime/rime-pinyin-simp/$RIME_PINYIN_SIMP/LICENSE" pinyin_simp.LICENSE
MOZC_DATA="$RAW/google/mozc/$MOZC/src/data/dictionary_oss"
for i in 0 1 2 3 4 5 6 7 8 9; do
    fetch "$MOZC_DATA/dictionary0$i.txt" "mozc.dictionary0$i.txt"
done
fetch "$MOZC_DATA/id.def" mozc.id.def
fetch "$MOZC_DATA/connection_single_column.txt" mozc.connection_single_column.txt
fetch "$MOZC_DATA/README.txt" mozc.README.txt
echo "done: $DIR"
