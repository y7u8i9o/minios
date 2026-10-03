#!/bin/sh
# Build the update medium of the development disk (docs/design/packages.md,
# P8 of docs/plan/packaging.md).
# usage: mkupdate.sh ARCH BASE APPS OUT
#
# PKGSIGN and PKG_KEY_FILE come from the environment as the top level
# Makefile exports them. The medium is a GPT disk with one partition of
# the type repo, whose mfs contains below repo/ARCH the signed repository of
# every base package of BASE and of the applications of this version in
# APPS. pkg-update of the development disk finds it at boot and upgrades
# the installed packages from it. When VIDEO is set, the medium also
# contains the file video with that mode, which pkg-update writes into the
# kernel command line of the disk.
set -e
ARCH="$1"; BASE="$2"; APPS="$3"; OUT="$4"
[ -n "$OUT" ] || { echo "usage: mkupdate.sh ARCH BASE APPS OUT" >&2; exit 2; }
TOP="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="$(cat "$TOP/VERSION")"
WORK="$OUT.d"
rm -rf "$WORK"
mkdir -p "$WORK/medium/repo"
trap 'rm -rf "$WORK"' EXIT
ARCHIVES="$(ls "$BASE"/*.mpk) $(ls "$APPS"/*-"$VERSION".mpk "$APPS"/luasynth-*.mpk 2>/dev/null | sort -u)"
"$TOP/tools/mkrepo.sh" "$PKGSIGN" "$PKG_KEY_FILE" "$WORK/medium/repo/$ARCH" $ARCHIVES > /dev/null
if [ -n "${VIDEO:-}" ]; then
    echo "$VIDEO" > "$WORK/medium/video"
fi
MED_MB=$(( $(du -sk "$WORK/medium" | cut -f1) / 1024 + 32 ))
"$MKFS" "$WORK/medium.img" "$MED_MB" "$WORK/medium" > /dev/null
"$MKGPT" "$OUT" $((MED_MB + 3)) "repo:rest:$WORK/medium.img" > /dev/null
