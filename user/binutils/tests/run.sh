#!/bin/sh
# The comparison of the binutils of minios with the GNU binutils
# (docs/design/binutils.md). make check-binutils compiles the programs of
# user/binutils for the host into BU and runs this script with TOP, the
# repository, WORK, a scratch directory, and BU_GROUPS, the names of the
# scripts in this directory to run. Each script calls compare for one
# command line on the files of the corpus of both architectures. compare
# runs the minios program and the GNU program with the same arguments and
# requires the same standard output.
#
# The corpus consists of files of the current build. ARCH_DIR is build for
# x86_64 and build/aarch64 for aarch64:
#   OBJ      an object file of libgui with debug information
#   ARCHIVE  the static archive libedit.a
#   SHARED   the shared object libjson.so
#   EXEC     a program with debug information (build/user/2048.elf)
#   STRIPPED the installed sh, whose debug information is removed
#   KERNEL   the kernel
# DWARF_EXEC is a program that the script compiles with -g, for addr2line.
set -u
: "${TOP:?}" "${BU:?}" "${WORK:?}"
BU_GROUPS=${BU_GROUPS-nm readelf objcopy addr2line}
rm -rf "$WORK"
mkdir -p "$WORK"
fail=0
checks=0

# compare NAME OURS GNU runs both command lines in the shell and compares
# their standard output. A difference prints the first lines of the diff.
compare() {
    checks=$((checks + 1))
    sh -c "$2" > "$WORK/ours.out" 2> "$WORK/ours.err"
    sh -c "$3" > "$WORK/gnu.out" 2> "$WORK/gnu.err"
    if ! cmp -s "$WORK/ours.out" "$WORK/gnu.out"; then
        echo "FAIL $ARCH $1"
        echo "  ours: $2"
        echo "  gnu:  $3"
        diff -u "$WORK/gnu.out" "$WORK/ours.out" | sed -n '3,22p' | sed 's/^/  /'
        fail=1
    fi
}

# check NAME CONDITION records a check that is no comparison.
check() {
    checks=$((checks + 1))
    if ! sh -c "$2" > "$WORK/check.out" 2>&1; then
        echo "FAIL $ARCH $1"
        sed -n '1,10p' "$WORK/check.out" | sed 's/^/  /'
        fail=1
    fi
}

for ARCH in x86_64 aarch64; do
    if [ "$ARCH" = x86_64 ]; then
        ARCH_DIR=$TOP/build
    else
        ARCH_DIR=$TOP/build/$ARCH
    fi
    GNU=$ARCH-elf-
    OBJ=$ARCH_DIR/libgui/src/theme.o
    ARCHIVE=$ARCH_DIR/sysroot/usr/lib/libedit.a
    SHARED=$ARCH_DIR/lib/libjson.so
    EXEC=$ARCH_DIR/user/2048.elf
    STRIPPED=$ARCH_DIR/sysroot/usr/bin/sh
    KERNEL=$ARCH_DIR/kernel.elf
    missing=""
    for f in "$OBJ" "$ARCHIVE" "$SHARED" "$EXEC" "$STRIPPED" "$KERNEL"; do
        [ -f "$f" ] || missing="$missing $f"
    done
    if [ -n "$missing" ]; then
        echo "check-binutils: $ARCH skipped, the build lacks:$missing"
        continue
    fi
    AWORK=$WORK/$ARCH
    mkdir -p "$AWORK"
    DWARF_EXEC=$AWORK/dwarf.elf
    cat > "$AWORK/dwarf.c" << 'EOF'
static int square(int x)
{
    return x * x;
}

int total;

void add(int n)
{
    for (int i = 0; i < n; i++)
        total += square(i);
}

void _start(void)
{
    add(10);
    for (;;)
        ;
}
EOF
    "${GNU}gcc" -g -O0 -ffreestanding -nostdlib -static -o "$DWARF_EXEC" "$AWORK/dwarf.c" || fail=1
    for group in $BU_GROUPS; do
        . "$TOP/user/binutils/tests/$group.sh"
    done
done
if [ $fail -eq 0 ]; then
    echo "check-binutils: $checks checks passed"
else
    echo "check-binutils: failures among $checks checks"
fi
exit $fail
