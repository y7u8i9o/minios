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
#   --retain        do not remove the worktree after a successful release
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
# build options and builds the installation medium, the live medium
# (docs/design/live.md) and the package repository with the debugging
# options off. The release check installs
# the system. A copy of the medium that contains an answer file installs
# onto an empty disk, and the installed disk then boots until the login
# appears. The results are copied into minios-VERSION in the output
# directory and consist, for each architecture, of the compressed
# installation medium without the answer file, the live medium, the
# repository the medium contains and the kernel, and once of the public key, BUILDINFO and
# SHA256SUMS. The host checks of make check run once before the first
# architecture. The package repositories are signed with RELEASE_KEY,
# $HOME/.config/minios/release-signing.key by default, which the build
# creates when it does not exist yet. The same key must sign every
# release, because installed systems trust the public half that the base
# packages carry.
#
# RELEASE_CONFIG replaces the build options of the release build, and
# BOOT_TIMEOUT the seconds that the installation and the first boot of
# the installed disk have to take each (300). A failing step stops the
# pipeline, names its log and leaves the worktree in place for inspection.
set -eu

TOP="$(cd "$(dirname "$0")/.." && pwd)"
REF=main
ARCHES=x86_64,aarch64
CASES=""
TESTS=1
TAG=0
RETAIN=0
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
        --retain) RETAIN=1 ;;
        --out) OUT="$2"; shift ;;
        -j) JOBS="$2"; shift ;;
        -h|--help) sed -n '2,42p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
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
        $CONFIG PKG_SERIAL=0 -j"$JOBS" installer live repo > "$LOGS/build-$a.log" 2>&1 ||
        fail "the $a release build" "$LOGS/build-$a.log"

    # The repository of the medium is the one the release publishes, the
    # base packages but the tests and the applications of this version,
    # as tools/mkinstaller.sh selects them.
    step "$a release repository"
    ARCHIVES="$(ls "$RB"/base/*.mpk | grep -v '/tests-[0-9.]*\.mpk$') $(ls "$RB"/packages/*-"$VERSION".mpk "$RB"/packages/luasynth-*.mpk 2>/dev/null | sort -u)"
    # shellcheck disable=SC2086
    "$WT/tools/mkrepo.sh" "$RB/host/pkgsign" "$KEY" "$RB/medium-repo" $ARCHIVES \
        > "$LOGS/repo-$a.log" 2>&1 || fail "the $a release repository" "$LOGS/repo-$a.log"

    # The release check installs the system. A copy of the medium carries
    # the answers of tests/cases/install_auto, which make the installer
    # run without questions and power the machine off at the end.
    step "$a release installation"
    PKGHOST="$RB/host/pkg" PKGSIGN="$RB/host/pkgsign" PKG_KEY_FILE="$KEY" \
        PKG_PUB="$RB/pkg/signing.pub" MKFS="$RB/host/mkfs" MKFAT="$RB/host/mkfat" \
        MKGPT="$RB/host/mkgpt" LIMINE="$RB/host/limine" KERNEL="$RB/kernel.elf" \
        "$WT/tools/mkinstaller.sh" "$a" "$RB/base" "$RB/packages" "$RB/installer-auto.img" 1024 \
        "$WT/tests/cases/install_auto/answers.conf" > "$LOGS/medium-$a.log" 2>&1 ||
        fail "the $a answer medium" "$LOGS/medium-$a.log"
    TARGET="$RB/target.img"
    rm -f "$TARGET"
    dd if=/dev/zero of="$TARGET" bs=1048576 count=0 seek=2048 2> /dev/null
    SERIAL="$LOGS/install-$a.txt"
    : > "$SERIAL"
    # The worktree has no qemu.conf, and a QEMU_CONF of the caller's
    # environment must not bring one in.
    unset QEMU_CONF
    ARCH="$a" BUILD="$RB" "$WT/tools/run.sh" --devdisk "$RB/installer-auto.img" \
        --update "$TARGET" --no-data --no-sound --display none \
        --serial "file:$SERIAL" > "$LOGS/install-$a.log" 2>&1 &
    QPID=$!
    waited=0
    result=timeout
    while [ "$waited" -lt "$BOOT_TIMEOUT" ]; do
        if ! kill -0 "$QPID" 2>/dev/null; then
            result=ok
            break
        fi
        if grep -aq "kernel panic:" "$SERIAL"; then
            result=panic
            break
        fi
        sleep 2
        waited=$((waited + 2))
    done
    if [ "$result" != ok ]; then
        kill "$QPID" 2>/dev/null || true
    fi
    wait "$QPID" 2>/dev/null || true
    [ "$result" = ok ] || fail "the $a release installation ($result after $waited s)" "$SERIAL"
    grep -aq "installer: done" "$SERIAL" ||
        fail "the $a release installation (no completion message)" "$SERIAL"
    echo "  $a installed after about $waited s"

    # The installed disk boots alone, which ends at the greeter, or at
    # the console login without a display. QEMU continues to run after
    # that and is stopped here.
    step "$a release boot"
    SERIAL="$LOGS/boot-$a.txt"
    : > "$SERIAL"
    ARCH="$a" BUILD="$RB" "$WT/tools/run.sh" --devdisk "$TARGET" \
        --no-data --no-sound --display none \
        --serial "file:$SERIAL" > "$LOGS/boot-$a.log" 2>&1 &
    QPID=$!
    waited=0
    result=timeout
    while [ "$waited" -lt "$BOOT_TIMEOUT" ]; do
        if grep -aq "greeter: display server running\|minios login:" "$SERIAL"; then
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
    echo "  $a reached the login after about $waited s"
done

# The results. The installation medium is compressed, since it is mostly empty.
step "collecting the results in $DIST"
mkdir -p "$DIST"
for a in $ARCHES; do
    RB="$WT/build/release-$a"
    gzip -9 -c "$RB/installer.img" > "$DIST/minios-$VERSION-$a-installer.img.gz"
    cp "$RB/minios-live-$VERSION-$a.iso" "$DIST/minios-$VERSION-$a-live.iso"
    cp "$RB/kernel.elf" "$DIST/minios-$VERSION-$a-kernel.elf"
    tar -C "$RB/medium-repo" -czf "$DIST/minios-$VERSION-$a-repo.tar.gz" .
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
if [ "$RETAIN" = 0 ]; then
    git -C "$TOP" worktree remove --force "$WT"
fi
step "done"
ls -l "$DIST"
