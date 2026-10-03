#!/bin/sh
# Download the sources of su, doas and sudo for the ports of
# docs/design/users.md into third_party. Run from any directory, with git,
# curl and network access:
#
#     tools/fetch_privilege.sh
#
# third_party/ubase receives su of ubase, the suckless Linux base
# utilities (MIT licence), with su.c, its manual page, the headers it
# includes and the utility library. The repository is cloned with depth 1 from
# git.suckless.org.
#
# third_party/opendoas receives OpenDoas, the portable doas of OpenBSD
# (ISC licence), at the release tag DOAS_VERSION, cloned from GitHub.
#
# third_party/sudo receives sudo (ISC licence) from the release tarball of
# version SUDO_VERSION from sudo.ws, limited to the headers, the C sources
# of the front end, of the sudoers policy and of the libraries that
# user/Makefile builds, the licence and the readme. The configure script ran once on the whole
# tarball, and its output is kept in user/ports/sudo.
#
# Each directory records its source and version in a file named ORIGIN.
# SUDO_VERSION and DOAS_VERSION may be set in the environment to fetch
# other releases.
set -eu

TOP="$(cd "$(dirname "$0")/.." && pwd)"
DOAS_VERSION="${DOAS_VERSION:-v6.8.2}"
SUDO_VERSION="${SUDO_VERSION:-1.9.17p1}"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/privilege.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

# su of ubase.
REPO="https://git.suckless.org/ubase"
echo "cloning $REPO"
git clone -q --depth 1 "$REPO" "$TMP/ubase"
DIR="$TOP/third_party/ubase"
rm -rf "$DIR"
mkdir -p "$DIR/src/libutil"
cp "$TMP/ubase/su.c" "$TMP/ubase/su.1" "$DIR/src/"
cp "$TMP/ubase"/*.h "$DIR/src/"
cp "$TMP/ubase"/libutil/*.c "$DIR/src/libutil/"
cp "$TMP/ubase/LICENSE" "$TMP/ubase/README" "$TMP/ubase/Makefile" "$DIR/"
{
    echo "$REPO"
    git -C "$TMP/ubase" log -1 --format='%H'
    date '+%Y-%m-%d'
} > "$DIR/ORIGIN"

# OpenDoas.
REPO="https://github.com/Duncaen/OpenDoas"
echo "cloning $REPO at $DOAS_VERSION"
git clone -q --depth 1 --branch "$DOAS_VERSION" "$REPO" "$TMP/opendoas"
DIR="$TOP/third_party/opendoas"
rm -rf "$DIR"
mkdir -p "$DIR"
(cd "$TMP/opendoas" && tar cf - --exclude .git .) | (cd "$DIR" && tar xf -)
{
    echo "$REPO"
    echo "$DOAS_VERSION"
    git -C "$TMP/opendoas" log -1 --format='%H'
    date '+%Y-%m-%d'
} > "$DIR/ORIGIN"

# sudo.
URL="https://www.sudo.ws/dist/sudo-$SUDO_VERSION.tar.gz"
echo "fetch   $URL"
curl -fsSL --retry 3 -o "$TMP/sudo.tar.gz" "$URL"
mkdir "$TMP/sudo"
tar xzf "$TMP/sudo.tar.gz" -C "$TMP/sudo" --strip-components 1
DIR="$TOP/third_party/sudo"
rm -rf "$DIR"
mkdir -p "$DIR"
(cd "$TMP/sudo" && tar cf - LICENSE.md README.md include/*.h include/*/*.h \
    lib/util/*.[ch] lib/iolog/*.[ch] lib/eventlog/*.[ch] lib/protobuf-c/*.c src/*.[ch] \
    plugins/sudoers/*.[ch] plugins/sudoers/auth/*.[ch]) | (cd "$DIR" && tar xf -)
{
    echo "$URL"
    echo "$SUDO_VERSION"
    shasum -a 256 "$TMP/sudo.tar.gz" | cut -d' ' -f1
    date '+%Y-%m-%d'
} > "$DIR/ORIGIN"

echo "done"
cat "$TOP/third_party/ubase/ORIGIN" "$TOP/third_party/opendoas/ORIGIN" "$TOP/third_party/sudo/ORIGIN"
