#!/bin/sh
# Download the sources of uACPI into third_party/uacpi. Run from the
# repository root:
#
#     tools/fetch_uacpi.sh
#
# uACPI is a portable ACPI implementation in C under the MIT licence
# (https://github.com/uACPI/uACPI). The aarch64 kernel builds it in the
# barebones mode, which reads the ACPI tables without the AML interpreter
# (docs/design/acpi.md). The script takes the release named by VERSION and
# copies its LICENSE, README.md, include/ and source/ directories.
set -eu

VERSION="${VERSION:-6.1.1}"
TOP="$(cd "$(dirname "$0")/.." && pwd)"
DIR="$TOP/third_party/uacpi"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

curl -fL --max-time 120 -o "$TMP/uacpi.tar.gz" \
    "https://github.com/uACPI/uACPI/archive/refs/tags/$VERSION.tar.gz"
tar -xzf "$TMP/uacpi.tar.gz" -C "$TMP"
SRC="$TMP/uACPI-$VERSION"

rm -rf "$DIR"
mkdir -p "$DIR"
cp "$SRC/LICENSE" "$SRC/README.md" "$DIR/"
cp -R "$SRC/include" "$SRC/source" "$DIR/"
rm -f "$DIR/source/files.cmake"
echo "$VERSION" > "$DIR/VERSION"

echo "done"
find "$DIR/source" -name '*.c' | xargs wc -l | tail -1
