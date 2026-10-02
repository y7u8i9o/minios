#!/bin/sh
# Download the CJK fallback font, Droid Sans Fallback Full of the Android
# Open Source Project (Apache License 2.0), with its notice into
# third_party/droidfallback. Run from any directory; needs curl and network
# access.
#
#   tools/fetch_fonts.sh            downloads what is missing
#   tools/fetch_fonts.sh -f         downloads everything again
set -e
TOP="$(cd "$(dirname "$0")/.." && pwd)"
DIR="$TOP/third_party/droidfallback"
BASE="https://raw.githubusercontent.com/aosp-mirror/platform_frameworks_base/android-6.0.1_r81/data/fonts"
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

fetch "$BASE/DroidSansFallbackFull.ttf" "$DIR/DroidSansFallbackFull.ttf"
fetch "$BASE/NOTICE" "$DIR/NOTICE"
echo "done: $DIR"
