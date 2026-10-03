#!/bin/sh
# Build the release of minios that VERSION names at a commit
# (docs/design/build.md).
#
# tools/release.sh [options] accepts the following options.
#   --ref REF       commit to release, main by default
#   --arch LIST     architectures separated by commas, x86_64,aarch64 by default
#   --cases LIST    boot cases separated by spaces, tests/release-cases of the
#                   commit by default
#   --skip-tests    leave out the host checks and the boot cases
#   --tag           create the annotated tag vVERSION at the commit afterwards
#   --keep          do not remove the worktree after a successful release
#   --out DIR       directory of the results, build/release by default
#   -j N            parallel make jobs, the number of processors by default
#
# The release is built from the commit alone. The script checks the commit
# out into a worktree below the output directory, which leaves the checkout
# in use untouched and gives a kernel version without the -dirty mark. The
# only file it copies in is the tinycc submodule, which must be checked out
# at the commit that REF records. Ignored files such as the purchased sounds
# of user/share/sounds are not part of the release.
#
# For every architecture the pipeline runs the boot cases with the default
# build options, builds the release with the debugging options off, boots
# the release image until the login prompt appears, and copies the results
# into minios-VERSION in the output directory. The host checks of make
# check run once before the first architecture. The package repositories
# are signed with RELEASE_KEY, $HOME/.config/minios/release-signing.key by
# default, which the build creates when it does not exist yet. The same key
# must sign every release, because installed systems trust the public half
# that the root image carries.
#
# RELEASE_CONFIG replaces the build options of the release build, and
# BOOT_TIMEOUT the seconds that the release image has to reach the login
# prompt (300). A failing step stops the pipeline, names its log and leaves
# the worktree in place for inspection.
set -eu

TOP="$(cd "$(dirname "$0")/.." && pwd)"
REF=main
ARCHES=x86_64,aarch64
CASES=""
TESTS=1
TAG=0
KEEP=0
OUT="$TOP/build/release"
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)"
KEY="${RELEASE_KEY:-$HOME/.config/minios/release-signing.key}"
CONFIG="${RELEASE_CONFIG:-CONFIG_TESTS=0 CONFIG_PANIC_EXIT=0 CONFIG_LOCKDEBUG=0 CONFIG_LOCKSTAT=0 CONFIG_SLABDEBUG=0 CONFIG_LOG_LEVEL=1}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-300}"

die() { echo "release: $*" >&2; exit 1; }
step() { printf '%s release: %s\n' "$(date '+%H:%M:%S')" "$*"; }

while [ $# -gt 0 ]; do
    case "$1" in
        --ref) REF="$2"; shift ;;
        --arch) ARCHES="$2"; shift ;;
        --cases) CASES="$2"; shift ;;
        --skip-tests) TESTS=0 ;;
        --tag) TAG=1 ;;
        --keep) KEEP=1 ;;
        --out) OUT="$2"; shift ;;
        -j) JOBS="$2"; shift ;;
        -h|--help) sed -n '2,36p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) die "unknown option $1" ;;
    esac
    shift
done
ARCHES="$(echo "$ARCHES" | tr ',' ' ')"
for a in $ARCHES; do
    case "$a" in
        x86_64|aarch64) ;;
        *) die "unknown architecture $a" ;;
    esac
done
case "$OUT" in
    /*) ;;
    *) OUT="$(pwd)/$OUT" ;;
esac
case "$KEY" in
    /*) ;;
    *) KEY="$(pwd)/$KEY" ;;
esac

# The commit, its version and its tag.
COMMIT="$(git -C "$TOP" rev-parse --verify --quiet "$REF^{commit}")" || die "no commit $REF"
VERSION="$(git -C "$TOP" show "$COMMIT:VERSION" | tr -d ' \n')"
echo "$VERSION" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' || die "VERSION of $REF is not MAJOR.MINOR.PATCH: $VERSION"
TAGNAME="v$VERSION"
if git -C "$TOP" rev-parse --verify --quiet "refs/tags/$TAGNAME" > /dev/null; then
    TAGGED="$(git -C "$TOP" rev-parse "$TAGNAME^{commit}")"
    [ "$TAGGED" = "$COMMIT" ] || die "tag $TAGNAME exists at $TAGGED, not at $COMMIT"
    TAG=0
fi
SHORT="$(git -C "$TOP" rev-parse --short "$COMMIT")"
# The build number of the release kernels is the number of commits up to
# the released one, the same for every architecture and growing from one
# release to the next.
BUILD_NUMBER="$(git -C "$TOP" rev-list --count "$COMMIT")"
export BUILD_NUMBER

DIST="$OUT/minios-$VERSION"
WT="$OUT/work-$VERSION"
LOGS="$OUT/logs-$VERSION"
[ -e "$DIST" ] && die "$DIST exists, move it away before building $VERSION again"
[ -e "$WT" ] && die "$WT exists, remove it with git worktree remove before building again"
mkdir -p "$OUT" "$LOGS"

# The tinycc submodule of the commit must match the checked out one.
WANT="$(git -C "$TOP" ls-tree "$COMMIT" third_party/tinycc | awk '{ print $3 }')"
HAVE="$(git -C "$TOP/third_party/tinycc" rev-parse HEAD 2>/dev/null)" ||
    die "third_party/tinycc is not checked out, run git submodule update --init"
[ "$WANT" = "$HAVE" ] || die "third_party/tinycc is at $HAVE, the commit records $WANT"
git -C "$TOP/third_party/tinycc" diff --quiet HEAD -- ||
    die "third_party/tinycc has local changes"

step "minios $VERSION from $SHORT ($REF), architectures $ARCHES"
git -C "$TOP" worktree add --quiet --detach "$WT" "$COMMIT"
fail() {
    echo "release: $1 failed, see $2" >&2
    echo "release: the worktree is left at $WT for inspection" >&2
    exit 1
}
mkdir -p "$WT/third_party/tinycc"
(cd "$TOP/third_party/tinycc" && tar cf - --exclude .git .) | (cd "$WT/third_party/tinycc" && tar xf -)
git -C "$WT" diff-index --quiet HEAD -- || die "the worktree differs from $SHORT after the copy of tinycc"
# The boot cases come from the commit, or from the checkout in use for a
# commit older than the list.
if [ -z "$CASES" ]; then
    LIST="$WT/tests/release-cases"
    [ -f "$LIST" ] || LIST="$TOP/tests/release-cases"
    [ -f "$LIST" ] || die "no tests/release-cases, name the cases with --cases"
    CASES="$(sed 's/#.*//' "$LIST" | tr -s ' \n' '  ' | sed 's/^ //; s/ $//')"
    # An older commit lacks some of the cases of a newer list.
    PRESENT=""
    for c in $CASES; do
        if [ -d "$WT/tests/cases/$c" ]; then
            PRESENT="$PRESENT $c"
        else
            step "case $c does not exist at $SHORT and is left out"
        fi
    done
    CASES="${PRESENT# }"
fi

if [ "$TESTS" = 1 ]; then
    step "host checks"
    make -C "$WT" -j"$JOBS" check > "$LOGS/check.log" 2>&1 || fail "make check" "$LOGS/check.log"
fi

[ -f "$KEY" ] || step "creating the release signing key $KEY"

for a in $ARCHES; do
    if [ "$TESTS" = 1 ]; then
        step "$a boot cases"
        make -C "$WT" ARCH="$a" -j"$JOBS" test CASES="$CASES" > "$LOGS/test-$a.log" 2>&1 ||
            fail "the $a boot cases" "$LOGS/test-$a.log"
        grep "^tests:" "$LOGS/test-$a.log" | sed "s/^/  $a /"
    fi

    # The release build has its own build directory, because the build
    # options are not dependencies of the objects.
    RB="$WT/build/release-$a"
    step "$a release build"
    # shellcheck disable=SC2086
    make -C "$WT" ARCH="$a" BUILD="$RB" REPO="$RB/repo" DATA="$RB/data.img" PKG_KEY="$KEY" \
        $CONFIG -j"$JOBS" image repo > "$LOGS/build-$a.log" 2>&1 ||
        fail "the $a release build" "$LOGS/build-$a.log"

    # The release image boots without test= on the command line, which
    # ends at the login prompt of the console. QEMU continues to run after
    # that and is stopped here.
    step "$a release boot"
    SERIAL="$LOGS/boot-$a.txt"
    : > "$SERIAL"
    # The worktree has no qemu.conf, and a QEMU_CONF of the caller's
    # environment must not bring one in.
    unset QEMU_CONF
    ARCH="$a" BUILD="$RB" ISO="$RB/minios.iso" DISK="$RB/disk.img" SWAP="$RB/swap.img" \
        "$WT/tools/run.sh" --no-data --no-sound --display none \
        --serial "file:$SERIAL" > "$LOGS/boot-$a.log" 2>&1 &
    QPID=$!
    waited=0
    result=timeout
    while [ "$waited" -lt "$BOOT_TIMEOUT" ]; do
        if grep -aq "minios login:" "$SERIAL"; then
            result=ok
            break
        fi
        if grep -aq "kernel panic:" "$SERIAL"; then
            result=panic
            break
        fi
        kill -0 "$QPID" 2>/dev/null || { result=exited; break; }
        sleep 2
        waited=$((waited + 2))
    done
    kill "$QPID" 2>/dev/null || true
    wait "$QPID" 2>/dev/null || true
    [ "$result" = ok ] || fail "the $a release boot ($result after $waited s)" "$SERIAL"
    echo "  $a reached the login prompt after about $waited s"
done

# The results. The root image is compressed, since it is mostly empty.
step "collecting the results in $DIST"
mkdir -p "$DIST"
for a in $ARCHES; do
    RB="$WT/build/release-$a"
    cp "$RB/minios.iso" "$DIST/minios-$VERSION-$a.iso"
    gzip -9 -c "$RB/disk.img" > "$DIST/minios-$VERSION-$a-root.img.gz"
    cp "$RB/kernel.elf" "$DIST/minios-$VERSION-$a-kernel.elf"
    tar -C "$RB/repo" -czf "$DIST/minios-$VERSION-$a-repo.tar.gz" .
    cp "$RB/pkg/signing.pub" "$DIST/minios-$VERSION-signing.pub"
done
{
    echo "minios $VERSION"
    echo "commit $COMMIT"
    echo "built $(date -u '+%Y-%m-%d %H:%M:%S') UTC on $(uname -sm)"
    echo "options $CONFIG"
    for a in $ARCHES; do
        echo "compiler $a $("$a-elf-gcc" --version 2>/dev/null | head -n 1 || echo unknown)"
    done
    echo "kernel $(strings "$WT/build/release-$(echo "$ARCHES" | cut -d' ' -f1)/kernel.elf" | grep -m 1 "^#[0-9]* $SHORT" || echo unknown)"
    if [ "$TESTS" = 1 ]; then
        echo "boot cases $CASES"
    else
        echo "boot cases skipped"
    fi
} > "$DIST/BUILDINFO"
(cd "$DIST" && shasum -a 256 minios-* BUILDINFO > SHA256SUMS)

if [ "$TAG" = 1 ]; then
    git -C "$TOP" tag -a "$TAGNAME" -m "minios $VERSION" "$COMMIT"
    step "tagged $TAGNAME at $SHORT, push it with git push origin $TAGNAME"
fi
if [ "$KEEP" = 0 ]; then
    git -C "$TOP" worktree remove --force "$WT"
fi
step "done"
ls -l "$DIST"
