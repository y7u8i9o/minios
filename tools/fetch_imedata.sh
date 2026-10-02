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
set -e
TOP="$(cd "$(dirname "$0")/.." && pwd)"
DIR="$TOP/third_party/imedata"
RIME_PINYIN_SIMP=0c6861ef7420ee780270ca6d993d18d4101049d0
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
echo "done: $DIR"
