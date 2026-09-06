#!/bin/sh
# Download the tar of sbase, the suckless base utilities, into
# third_party/sbase. Run from the repository root:
#
#     tools/fetch_tar.sh
#
# The repository is cloned with depth 1 from git.suckless.org. tar.c, the
# headers it includes, the utility library, the manual page, the licence,
# the readme and the upstream Makefile are copied; the clone is removed.
# The commit is recorded in third_party/sbase/COMMIT.
set -eu

TOP="$(cd "$(dirname "$0")/.." && pwd)"
DIR="$TOP/third_party/sbase"
REPO="https://git.suckless.org/sbase"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/sbase.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

echo "cloning $REPO"
git clone -q --depth 1 "$REPO" "$TMP/sbase"

rm -rf "$DIR"
mkdir -p "$DIR/src" "$DIR/src/libutil"
cp "$TMP/sbase/tar.c" "$TMP/sbase/tar.1" "$DIR/src/"
for f in arg.h fs.h util.h text.h utf.h queue.h; do
    [ -f "$TMP/sbase/$f" ] && cp "$TMP/sbase/$f" "$DIR/src/"
done
cp "$TMP/sbase"/libutil/*.c "$DIR/src/libutil/"
[ -f "$TMP/sbase/libutil/libutil.h" ] && cp "$TMP/sbase/libutil/libutil.h" "$DIR/src/libutil/"
cp "$TMP/sbase/LICENSE" "$DIR/LICENSE"
cp "$TMP/sbase/README" "$DIR/README"
cp "$TMP/sbase/Makefile" "$DIR/Makefile"
git -C "$TMP/sbase" log -1 --format='%H' > "$DIR/COMMIT"
date '+%Y-%m-%d' >> "$DIR/COMMIT"

echo "done"
ls "$DIR" "$DIR/src" "$DIR/src/libutil"
