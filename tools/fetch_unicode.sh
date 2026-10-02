#!/bin/sh
# Download the files of the Unicode Character Database that
# tools/genunicode.py reads into third_party/unicode, together with the
# Unicode licence. Run from any directory; needs curl and network access.
#
#   tools/fetch_unicode.sh          downloads what is missing
#   tools/fetch_unicode.sh -f       downloads everything again
set -e
TOP="$(cd "$(dirname "$0")/.." && pwd)"
DIR="$TOP/third_party/unicode"
VERSION=16.0.0
BASE="https://www.unicode.org/Public/$VERSION/ucd"
FORCE=""
[ "$1" = -f ] && FORCE=1
mkdir -p "$DIR"

fetch() {
    url="$1"
    out="$2"
    if [ -z "$FORCE" ] && [ -s "$out" ]; then
        echo "have    $out"
        return
    fi
    echo "fetch   $url"
    curl -fsSL --retry 3 -o "$out.tmp" "$url"
    mv "$out.tmp" "$out"
}

fetch "https://www.unicode.org/license.txt" "$DIR/LICENSE.txt"
for f in UnicodeData.txt EastAsianWidth.txt DerivedCoreProperties.txt PropList.txt; do
    fetch "$BASE/$f" "$DIR/$f"
done
echo "$VERSION" > "$DIR/VERSION"
echo "done: Unicode $VERSION in $DIR"
