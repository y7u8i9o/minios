#!/bin/sh
# Download the sources of pdpmake, the public domain POSIX make of Ron
# Yorston, into third_party/make. Run from the repository root:
#
#     tools/fetch_make.sh
#
# The master branch is fetched as a tarball from GitHub, so no file list is
# needed; the commit it corresponds to is recorded in third_party/make/COMMIT.
set -eu

TOP="$(cd "$(dirname "$0")/.." && pwd)"
DIR="$TOP/third_party/make"
REPO="rmyorston/pdpmake"
TARBALL="https://github.com/$REPO/archive/refs/heads/master.tar.gz"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/pdpmake.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

echo "fetching $TARBALL"
curl -fL --max-time 120 -o "$TMP/pdpmake.tar.gz" "$TARBALL"
tar -xzf "$TMP/pdpmake.tar.gz" -C "$TMP"

rm -rf "$DIR"
mkdir -p "$DIR/src"
SRC="$(find "$TMP" -mindepth 1 -maxdepth 1 -type d -name 'pdpmake-*' | head -n 1)"
# The C sources, the header and the manual page are copied into src/. The
# licence, the readme and the upstream Makefile are copied into the
# directory above it.
for f in "$SRC"/*.c "$SRC"/*.h "$SRC"/*.1; do
    [ -f "$f" ] && cp "$f" "$DIR/src/"
done
for f in LICENSE README.md Makefile; do
    [ -f "$SRC/$f" ] && cp "$SRC/$f" "$DIR/$f"
done
[ -d "$SRC/tests" ] && cp -R "$SRC/tests" "$DIR/tests"

curl -fsL --max-time 60 "https://api.github.com/repos/$REPO/commits/master" \
    | sed -n 's/^  "sha": "\(.*\)",/\1/p' > "$DIR/COMMIT" || true
date '+%Y-%m-%d' >> "$DIR/COMMIT"

echo "done"
ls "$DIR" "$DIR/src"
wc -l "$DIR"/src/*.c | tail -1
