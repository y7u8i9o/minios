#!/bin/sh
# Download the sources of FreeBSD sed and the One True AWK into
# third_party/sed and third_party/awk. Run from the repository root:
#
#     tools/fetch_sedawk.sh
#
# Both come from the GitHub mirror of the FreeBSD source tree: sed from
# usr.bin/sed (BSD 3-clause licence) and the One True AWK of Brian Kernighan
# from contrib/one-true-awk (MIT style licence).
set -eu

TOP="$(cd "$(dirname "$0")/.." && pwd)"
SED_DIR="$TOP/third_party/sed"
AWK_DIR="$TOP/third_party/awk"
SED_URL="https://raw.githubusercontent.com/freebsd/freebsd-src/main/usr.bin/sed"

mkdir -p "$SED_DIR/src"

curl -fL --max-time 60 -o "$SED_DIR/LICENSE" \
    "https://raw.githubusercontent.com/freebsd/freebsd-src/main/COPYRIGHT"
# Record the commit of the mirror the files were taken from.
curl -fsL --max-time 60 "https://api.github.com/repos/freebsd/freebsd-src/commits/main" \
    | sed -n 's/^  "sha": "\(.*\)",/\1/p' > "$SED_DIR/COMMIT" || true

AWK_URL="https://raw.githubusercontent.com/freebsd/freebsd-src/main/contrib/one-true-awk"
mkdir -p "$AWK_DIR/src"
for f in awk.h awkgram.y b.c lex.c lib.c main.c maketab.c parse.c proto.h run.c tran.c awk.1; do
    echo "awk: $f"
    curl -fL --max-time 60 -o "$AWK_DIR/src/$f" "$AWK_URL/$f"
done
for f in LICENSE README.md FIXES; do
    echo "awk: $f"
    curl -fL --max-time 60 -o "$AWK_DIR/$f" "$AWK_URL/$f" || rm -f "$AWK_DIR/$f"
done
cp "$SED_DIR/COMMIT" "$AWK_DIR/COMMIT" 2>/dev/null || true

echo "done"
wc -l "$SED_DIR"/src/* "$AWK_DIR"/src/* | tail -1
