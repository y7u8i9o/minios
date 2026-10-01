#!/bin/sh
# Build a minios package on the host from a directory holding manifest and
# files/ (docs/design/packages.md).
# usage: mkpkg.sh DIR OUT.mpk ROOT
# ROOT is the root tree of the build (build/initrd_root); its lib/abi gives
# the ABI numbers of the system libraries. The needs lines of the manifest
# are derived from the DT_NEEDED entries of every ELF file with readelf
# ($READELF, default x86_64-elf-readelf). As with pkg build, a needs line
# of the source manifest supplies the number of a library that neither the
# package nor lib/abi provides. The arch line is derived from the ELF
# files as well.
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
# The arch line is the machine of the ELF files, from e_machine at offset
# 18 (62 x86_64, 183 aarch64). All ELF files must agree with each other and
# with an arch line of the source manifest.
find "$DIR/files" -type f | LC_ALL=C sort | while read -r f; do
    head -c 4 "$f" | od -An -c | grep -q 'E   L   F' || continue
    # od of macOS ends its output with blank lines.
    printf '%s\n' "$(od -An -tu2 -j18 -N2 "$f" | tr -d ' \n')"
done | LC_ALL=C sort -u > "$TMP/machines"
given="$(sed -n 's/^arch \(.*\)$/\1/p' "$DIR/manifest" | head -1)"
arch=""
while read -r code; do
    case "$code" in
        62) a=x86_64 ;;
        183) a=aarch64 ;;
        *) echo "mkpkg.sh: an ELF file of $DIR has the unknown machine $code" >&2; exit 1 ;;
    esac
    [ -z "$arch" ] || { echo "mkpkg.sh: the ELF files of $DIR are built for two machines" >&2; exit 1; }
    arch="$a"
done < "$TMP/machines"
if [ -n "$given" ] && [ -n "$arch" ] && [ "$given" != "$arch" ]; then
    echo "mkpkg.sh: the manifest names $given, and the ELF files are built for $arch" >&2; exit 1
fi
[ -n "$arch" ] && [ -z "$given" ] && echo "arch $arch" >> "$TMP/manifest"
# The sonames every ELF file needs, once each.
find "$DIR/files" -type f | LC_ALL=C sort | while read -r f; do
    head -c 4 "$f" | od -An -c | grep -q 'E   L   F' || continue
    "$READELF" -d "$f" 2>/dev/null | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'
done | LC_ALL=C sort -u > "$TMP/needed"
while read -r so; do
    [ -n "$so" ] || continue
    abi="$(sed -n "s/^provides $so \([0-9]*\)$/\1/p" "$DIR/manifest" | head -1)"
    [ -n "$abi" ] || abi="$(sed -n "s/^$so \([0-9]*\)$/\1/p" "$ROOT/lib/abi" | head -1)"
    [ -n "$abi" ] || abi="$(sed -n "s/^needs $so \([0-9]*\)$/\1/p" "$DIR/manifest" | head -1)"
    [ -n "$abi" ] || { echo "mkpkg.sh: the ABI number of $so is unknown" >&2; exit 1; }
    echo "needs $so $abi" >> "$TMP/manifest"
done < "$TMP/needed"
cp -R "$DIR/files" "$TMP/files"
# Members are manifest, then files/ with its tree; ustar keeps the order
# given. COPYFILE_DISABLE keeps the resource forks of macOS out.
(cd "$TMP" && COPYFILE_DISABLE=1 tar --format ustar -cf - manifest files) | gzip -9 > "$OUT"
