#!/bin/sh
# mkrepo.sh writes a signed package repository (docs/design/packages.md).
# It is called as mkrepo.sh PKGSIGN KEY OUTDIR ARCHIVE...
# PKGSIGN is the host tool tools/pkgsign, KEY a secret key file it wrote.
# OUTDIR is emptied and receives the archives, the index listing each of
# them with the manifest keys pkg resolves by (name, version, summary,
# depends, conflicts, provides, needs) and its path, size and SHA-256
# digest, and index.sig, the Ed25519 signature of the index.
set -e
PKGSIGN="$1"; KEY="$2"; OUT="$3"
[ -n "$PKGSIGN" ] && [ -n "$KEY" ] && [ -n "$OUT" ] && [ $# -ge 4 ] || {
    echo "usage: mkrepo.sh PKGSIGN KEY OUTDIR ARCHIVE..." >&2; exit 2; }
shift 3
[ -f "$KEY" ] || { echo "mkrepo.sh: $KEY: no such file" >&2; exit 1; }
rm -rf "$OUT"
mkdir -p "$OUT"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
echo "minios-pkg-index 1" > "$TMP/index"
for archive in "$@"; do
    base="$(basename "$archive")"
    [ -e "$OUT/$base" ] && { echo "mkrepo.sh: $base named twice" >&2; exit 1; }
    cp "$archive" "$OUT/$base"
    # The manifest is the first member of every package.
    tar -xzOf "$archive" manifest > "$TMP/manifest" ||
        { echo "mkrepo.sh: $archive: no manifest" >&2; exit 1; }
    digest="$("$PKGSIGN" digest "$OUT/$base")"
    size="${digest% *}"; sha="${digest#* }"
    {
        # An entry begins with its name line.
        echo
        grep -E '^name[[:space:]]' "$TMP/manifest"
        grep -E '^(version|summary|depends|conflicts|provides|needs)[[:space:]]' "$TMP/manifest"
        echo "path $base"
        echo "size $size"
        echo "sha256 $sha"
    } >> "$TMP/index"
done
cp "$TMP/index" "$OUT/index"
"$PKGSIGN" sign "$KEY" "$OUT/index"
