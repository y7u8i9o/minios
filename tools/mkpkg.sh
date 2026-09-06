#!/bin/sh
# Build a minios package on the host from a directory holding manifest and
# files/ (docs/design/packages.md).
# usage: mkpkg.sh DIR OUT.mpk ROOT
# ROOT is the root tree of the build (build/initrd_root); its lib/abi gives
# the ABI numbers of the system libraries. The needs lines of the manifest
# are derived from the DT_NEEDED entries of every ELF file with readelf
# ($READELF, default x86_64-elf-readelf).
set -e
DIR="$1"; OUT="$2"; ROOT="$3"
READELF="${READELF:-x86_64-elf-readelf}"
[ -n "$DIR" ] && [ -n "$OUT" ] && [ -n "$ROOT" ] || { echo "usage: mkpkg.sh DIR OUT.mpk ROOT" >&2; exit 2; }
[ -f "$DIR/manifest" ] || { echo "mkpkg.sh: $DIR/manifest: no such file" >&2; exit 1; }
[ -d "$DIR/files" ] || { echo "mkpkg.sh: $DIR/files: no such directory" >&2; exit 1; }
[ -f "$ROOT/lib/abi" ] || { echo "mkpkg.sh: $ROOT/lib/abi: no such file" >&2; exit 1; }
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
grep -v '^needs ' "$DIR/manifest" > "$TMP/manifest"
# The sonames every ELF file needs, once each.
find "$DIR/files" -type f | LC_ALL=C sort | while read -r f; do
    head -c 4 "$f" | od -An -c | grep -q 'E   L   F' || continue
    "$READELF" -d "$f" 2>/dev/null | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'
done | LC_ALL=C sort -u > "$TMP/needed"
while read -r so; do
    [ -n "$so" ] || continue
    abi="$(sed -n "s/^provides $so \([0-9]*\)$/\1/p" "$DIR/manifest" | head -1)"
    [ -n "$abi" ] || abi="$(sed -n "s/^$so \([0-9]*\)$/\1/p" "$ROOT/lib/abi" | head -1)"
    [ -n "$abi" ] || { echo "mkpkg.sh: the ABI number of $so is unknown" >&2; exit 1; }
    echo "needs $so $abi" >> "$TMP/manifest"
done < "$TMP/needed"
cp -R "$DIR/files" "$TMP/files"
# Members are manifest, then files/ with its tree; ustar keeps the order
# given. COPYFILE_DISABLE keeps the resource forks of macOS out.
(cd "$TMP" && COPYFILE_DISABLE=1 tar --format ustar -cf - manifest files) | gzip -9 > "$OUT"
