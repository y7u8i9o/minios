# Comparison of addr2line, ranlib and the index operations of ar with the
# GNU programs (sourced by run.sh once for each architecture).  The
# variables of run.sh are in use: ARCH, GNU, BU, AWORK, OBJ, ARCHIVE,
# EXEC, STRIPPED, KERNEL and DWARF_EXEC.
#
# The first part compiles test programs with several DWARF versions and
# two optimization levels and compares addr2line on the address of every
# function and on every fourth address of the code.  The second part
# compares ranlib and the index of ar.

A2L=$BU/addr2line
A2W=$AWORK/a2l
mkdir -p "$A2W"

cat > "$A2W/inl.h" << 'EOF'
static inline int twice(int x)
{
    return x + x;
}

static inline int clamp(int x, int hi)
{
    if (x > hi)
        return hi;
    return twice(x) > hi ? hi : x;
}

int second(int n);
EOF
cat > "$A2W/prog.c" << 'EOF'
#include "inl.h"

static inline int square(int x)
{
    return x * x;
}

static inline int cube(int x)
{
    return square(x) * x;
}

int total;

static int accumulate(int n)
{
    int sum = 0;
    for (int i = 0; i < n; i++)
        sum += cube(i) + second(i);
    return sum;
}

int add(int a, int b)
{
    return clamp(accumulate(a), 1000) + square(b);
}

int mul(int a, int b)
{
    int r = 0;
    while (b-- > 0)
        r += a;
    return r;
}

void _start(void)
{
    total = add(10, 3) + mul(3, 4);
    for (;;)
        ;
}
EOF
cat > "$A2W/other.c" << 'EOF'
#include "inl.h"

int second(int n)
{
    int r = 0;
    while (n > 0) {
        r += clamp(n % 7, 5);
        n /= 2;
    }
    return r;
}
EOF

# addr_files PROGRAM PREFIX writes PREFIX.sym with the address of every
# function symbol and PREFIX.step with every fourth address of .text.
addr_files() {
    ${GNU}nm "$1" | awk '$2 ~ /^[tTwW]$/ { print "0x" $1 }' > "$2.sym"
    set -- "$1" "$2" $(${GNU}readelf -S -W "$1" | sed 's/\[ *[0-9]*\]//' | awk '$1 == ".text" { print $3, $5 }')
    awk -v s=$((0x$3)) -v n=$((0x$4)) -v step="${STEP:-4}" 'BEGIN { for (i = -2; i < n + 2; i += step) printf "0x%x\n", s + i }' > "$2.step"
}

# compare_a2l NAME PROGRAM ADDRESSFILE OPTIONS compares the output for the
# addresses of the file read from standard input.
compare_a2l() {
    compare "$1 $4" "$A2L $4 -e $2 < $3" "${GNU}addr2line $4 -e $2 < $3"
}

OPTION_SETS="-f -s -a -p -i -fi -fip -fsia -aip -fsi"

# Compiled test programs.
for variant in "-O0" "-O2" "-O2 -gdwarf-2" "-O2 -gdwarf-3" "-O2 -gdwarf-4" "-O2 -gdwarf-5" "-O0 -gdwarf-4"; do
    tag=$(echo "$variant" | tr -d ' ')
    prog=$A2W/prog$tag.elf
    if ! "${GNU}gcc" -g $variant -ffreestanding -nostdlib -static -o "$prog" "$A2W/prog.c" "$A2W/other.c"; then
        check "compile $tag" false
        continue
    fi
    addr_files "$prog" "$A2W/prog$tag"
    for opts in $OPTION_SETS; do
        compare_a2l "prog$tag step" "$prog" "$A2W/prog$tag.step" "$opts"
    done
    for opts in -f -fip -aip; do
        compare_a2l "prog$tag sym" "$prog" "$A2W/prog$tag.sym" "$opts"
    done
done

# The program of the framework.
addr_files "$DWARF_EXEC" "$A2W/dwarf"
for opts in $OPTION_SETS; do
    compare_a2l "dwarf step" "$DWARF_EXEC" "$A2W/dwarf.step" "$opts"
done
compare_a2l "dwarf sym" "$DWARF_EXEC" "$A2W/dwarf.sym" "-fip"

# The corpus program of the build, sampled.
STEP=16 addr_files "$EXEC" "$A2W/exec"
for opts in -f -fip -fsia -aip; do
    compare_a2l "exec step" "$EXEC" "$A2W/exec.step" "$opts"
done
compare_a2l "exec sym" "$EXEC" "$A2W/exec.sym" "-fip"
STEP=64 addr_files "$KERNEL" "$A2W/kernel"
compare_a2l "kernel step" "$KERNEL" "$A2W/kernel.step" "-fip"
compare_a2l "kernel sym" "$KERNEL" "$A2W/kernel.sym" "-fsia"
STEP=32 addr_files "$SHARED" "$A2W/shared"
compare_a2l "shared step" "$SHARED" "$A2W/shared.step" "-fip"

# A program without debug information and one without symbols.
STEP=8 addr_files "$STRIPPED" "$A2W/stripped"
compare_a2l "stripped step" "$STRIPPED" "$A2W/stripped.step" "-fip"
compare_a2l "stripped step" "$STRIPPED" "$A2W/stripped.step" "-a"
compare_a2l "stripped sym" "$STRIPPED" "$A2W/stripped.sym" "-fi"

# Addresses on the command line, outside any section, and unusual forms.
P=$A2W/prog-O2.elf
set -- $(sed -n '1p;10p;15p;30p' "$A2W/prog-O2.step")
compare "args -fip" "$A2L -fip -e $P $1 $2 $3 $4" "${GNU}addr2line -fip -e $P $1 $2 $3 $4"
compare "args -a" "$A2L -a -e $P $1 $2 $3 $4" "${GNU}addr2line -a -e $P $1 $2 $3 $4"
compare "args outside" "$A2L -fip -e $P 0x0 0x1 0xffffffffffffffff 0x10" "${GNU}addr2line -fip -e $P 0x0 0x1 0xffffffffffffffff 0x10"
compare "args outside -f" "$A2L -f -e $P 0 1 ffffffff" "${GNU}addr2line -f -e $P 0 1 ffffffff"
compare "args upper case" "$A2L -fa -e $P 0X$(echo "$2" | sed 's/^0x//' | tr a-f A-F)" "${GNU}addr2line -fa -e $P 0X$(echo "$2" | sed 's/^0x//' | tr a-f A-F)"
compare "args no prefix" "$A2L -f -e $P $(echo "$2" | sed 's/^0x//')" "${GNU}addr2line -f -e $P $(echo "$2" | sed 's/^0x//')"
compare "args invalid" "$A2L -f -e $P zz $2" "${GNU}addr2line -f -e $P zz $2"
compare "args options after" "$A2L $2 -e $P -fip $3" "${GNU}addr2line $2 -e $P -fip $3"
compare "long options" "$A2L --exe=$P --functions --basenames --addresses --pretty-print --inlines $2 $3" \
    "${GNU}addr2line --exe=$P --functions --basenames --addresses --pretty-print --inlines $2 $3"
compare "long exe separate" "$A2L --exe $P -f $2" "${GNU}addr2line --exe $P -f $2"
compare "attached exe" "$A2L -f -e$P $2" "${GNU}addr2line -f -e$P $2"
compare "no address, no options" "$A2L -e $P $2" "${GNU}addr2line -e $P $2"
compare "stripped outside" "$A2L -fip -e $STRIPPED 0x0 0x10" "${GNU}addr2line -fip -e $STRIPPED 0x0 0x10"

# Standard input.
printf '%s\n' "$1" "$2" "" "  $3" "$4 trailing" "0x1" > "$A2W/stdin.txt"
compare "stdin -fip" "$A2L -fip -e $P < $A2W/stdin.txt" "${GNU}addr2line -fip -e $P < $A2W/stdin.txt"
compare "stdin -a" "$A2L -a -e $P < $A2W/stdin.txt" "${GNU}addr2line -a -e $P < $A2W/stdin.txt"
printf '%s' "$1" > "$A2W/stdin-noeol.txt"
compare "stdin no newline" "$A2L -f -e $P < $A2W/stdin-noeol.txt" "${GNU}addr2line -f -e $P < $A2W/stdin-noeol.txt"
compare "stdin empty" "$A2L -f -e $P < /dev/null" "${GNU}addr2line -f -e $P < /dev/null"
compare "stdin pipe" "printf '%s\n' $1 $2 | $A2L -fi -e $P" "printf '%s\n' $1 $2 | ${GNU}addr2line -fi -e $P"

# The default file name a.out.
mkdir -p "$A2W/default"
cp "$P" "$A2W/default/a.out"
compare "default a.out" "cd $A2W/default && $A2L -fip $1 $2 $3" "cd $A2W/default && ${GNU}addr2line -fip $1 $2 $3"

# Section-relative offsets.
compare "section .text" "$A2L -fip -j .text -e $P 0 4 8 0x10 0x20 0x30 0x40" "${GNU}addr2line -fip -j .text -e $P 0 4 8 0x10 0x20 0x30 0x40"
compare "section long" "$A2L -fa --section=.text -e $P 0 8 0x18" "${GNU}addr2line -fa --section=.text -e $P 0 8 0x18"
compare "section beyond" "$A2L -f -j .text -e $P 0x100000" "${GNU}addr2line -f -j .text -e $P 0x100000"
compare "section data" "$A2L -f -j .data -e $P 0 1 2" "${GNU}addr2line -f -j .data -e $P 0 1 2"

# An object file: the debug sections receive their relocations.
n=$(${GNU}readelf -S -W "$OBJ" | sed 's/\[ *[0-9]*\]//' | awk '$1 == ".text" { print $5 }')
awk -v n=$((0x$n)) 'BEGIN { for (i = 0; i < n + 2; i += 3) printf "0x%x\n", i }' > "$A2W/obj.step"
for opts in "-fip -j .text" "-fsia -j .text" "-fi -j .text" "-fip"; do
    compare "object $opts" "$A2L $opts -e $OBJ < $A2W/obj.step" "${GNU}addr2line $opts -e $OBJ < $A2W/obj.step"
done
"${GNU}gcc" -g -O2 -ffunction-sections -c -o "$A2W/sections.o" "$A2W/prog.c"
for sec in $(${GNU}readelf -S -W "$A2W/sections.o" | sed 's/\[ *[0-9]*\]//' | awk '$1 ~ /^\.text\./ { print $1 }'); do
    awk 'BEGIN { for (i = 0; i < 160; i += 2) printf "0x%x\n", i }' > "$A2W/sections.step"
    compare "object $sec" "$A2L -fip -j $sec -e $A2W/sections.o < $A2W/sections.step" \
        "${GNU}addr2line -fip -j $sec -e $A2W/sections.o < $A2W/sections.step"
done

# Errors.
check "missing file" "! $A2L -e $A2W/none 0x0"
check "bad format" "! $A2L -e $A2W/prog.c 0x0"
check "missing section" "! $A2L -j .nosuch -e $P 0x0"
check "bad option" "! $A2L -Z -e $P 0x0"

# ---- ranlib and the index of ar ----

RL=$AWORK/ranlib
mkdir -p "$RL"
cd "$RL" || exit 1
cp "$A2W/prog.c" "$A2W/other.c" "$A2W/inl.h" .
"${GNU}gcc" -g -O1 -c prog.c -o prog.o
"${GNU}gcc" -c other.c -o other.o
cp "$OBJ" corpus_object_with_a_long_member_name.o
"${GNU}ar" rcS base.a prog.o other.o corpus_object_with_a_long_member_name.o

# ranlib_pair NAME OPTIONS SOURCE compares our ranlib with the GNU ranlib
# on copies of the archive.
ranlib_pair() {
    cp "$3" "$RL/ours.a"
    cp "$3" "$RL/gnu.a"
    chmod u+w "$RL/ours.a" "$RL/gnu.a"
    "$BU/ranlib" $2 "$RL/ours.a" || check "$1 run" false
    "${GNU}ranlib" $2 "$RL/gnu.a"
    compare "$1 nm -s" "${GNU}nm -s $RL/ours.a" "${GNU}nm -s $RL/gnu.a"
    compare "$1 ar t" "${GNU}ar t $RL/ours.a" "${GNU}ar t $RL/gnu.a"
    compare "$1 ar p" "${GNU}ar p $RL/ours.a" "${GNU}ar p $RL/gnu.a"
}
ranlib_pair "ranlib base" "" "$RL/base.a"
ranlib_pair "ranlib -D base" "-D" "$RL/base.a"
compare "ranlib -D header" "head -c 68 $RL/ours.a" "head -c 68 $RL/gnu.a"
ranlib_pair "ranlib -U base" "-U" "$RL/base.a"
"${GNU}ar" rcs indexed.a prog.o other.o
ranlib_pair "ranlib -t indexed" "-t" "$RL/indexed.a"
compare "ranlib -t indexed bytes" "cat $RL/ours.a" "cat $RL/gnu.a"
cp base.a no_index.a
check "ranlib -t without index" "! $BU/ranlib -t $RL/no_index.a 2>/dev/null && cmp $RL/base.a $RL/no_index.a"
ranlib_pair "ranlib libedit" "" "$ARCHIVE"
ranlib_pair "ranlib -D libedit" "-D" "$ARCHIVE"
ranlib_pair "ranlib again" "" "$RL/indexed.a"

# An archive with a member that is no object file, and an empty archive.
echo "plain text" > notes.txt
"${GNU}ar" rcS text.a notes.txt
ranlib_pair "ranlib text" "" "$RL/text.a"
"${GNU}ar" rcS mixed.a notes.txt prog.o
ranlib_pair "ranlib mixed" "" "$RL/mixed.a"

# Two archives in one call.
mkdir -p two_ours two_gnu
cp base.a text.a two_ours
cp base.a text.a two_gnu
"$BU/ranlib" two_ours/base.a two_ours/text.a
"${GNU}ranlib" two_gnu/base.a two_gnu/text.a
compare "ranlib two archives" "cd $RL/two_ours && ${GNU}nm -s base.a text.a" "cd $RL/two_gnu && ${GNU}nm -s base.a text.a"
check "ranlib missing" "! $BU/ranlib $RL/none.a"
check "ranlib no archive" "! $BU/ranlib $RL/notes.txt"
check "ranlib no arguments" "! $BU/ranlib"
check "ranlib dates" "$BU/ranlib -D $RL/two_ours/base.a && head -c 36 $RL/two_ours/base.a | tail -c 12 | grep -q '^0 *\$'"

# ar_pair NAME OURS GNU runs the same ar operation in two directories and
# compares the index and the members.
ar_pair() {
    rm -f "$RL/ours.a" "$RL/gnu.a"
    ( cd "$RL" && $2 ) > /dev/null 2>&1
    ( cd "$RL" && $3 ) > /dev/null 2>&1
    compare "$1 nm -s" "${GNU}nm -s $RL/ours.a" "${GNU}nm -s $RL/gnu.a"
    compare "$1 ar t" "${GNU}ar t $RL/ours.a" "${GNU}ar t $RL/gnu.a"
    compare "$1 ar p" "${GNU}ar p $RL/ours.a" "${GNU}ar p $RL/gnu.a"
}
ar_pair "ar rcs" "$BU/ar rcs ours.a prog.o other.o" "${GNU}ar rcs gnu.a prog.o other.o"
ar_pair "ar rc" "$BU/ar rc ours.a prog.o other.o" "${GNU}ar rc gnu.a prog.o other.o"
ar_pair "ar rcS" "$BU/ar rcS ours.a prog.o other.o" "${GNU}ar rcS gnu.a prog.o other.o"
ar_pair "ar rcs long name" "$BU/ar rcs ours.a prog.o corpus_object_with_a_long_member_name.o" \
    "${GNU}ar rcs gnu.a prog.o corpus_object_with_a_long_member_name.o"
ar_pair "ar s" "cp base.a ours.a && $BU/ar s ours.a" "cp base.a gnu.a && ${GNU}ar s gnu.a"
ar_pair "ar s libedit" "cp $ARCHIVE ours.a && chmod u+w ours.a && $BU/ar s ours.a" \
    "cp $ARCHIVE gnu.a && chmod u+w gnu.a && ${GNU}ar s gnu.a"
ar_pair "ar d" "cp base.a ours.a && $BU/ar d ours.a prog.o" "cp base.a gnu.a && ${GNU}ar d gnu.a prog.o"
ar_pair "ar d indexed" "$BU/ar rcs ours.a prog.o other.o && $BU/ar d ours.a other.o" \
    "${GNU}ar rcs gnu.a prog.o other.o && ${GNU}ar d gnu.a other.o"
ar_pair "ar d all objects" "cp base.a ours.a && $BU/ar d ours.a prog.o other.o corpus_object_with_a_long_member_name.o" \
    "cp base.a gnu.a && ${GNU}ar d gnu.a prog.o other.o corpus_object_with_a_long_member_name.o"
ar_pair "ar q" "$BU/ar q ours.a prog.o other.o" "${GNU}ar q gnu.a prog.o other.o"
ar_pair "ar qs" "$BU/ar qs ours.a prog.o other.o" "${GNU}ar qs gnu.a prog.o other.o"
ar_pair "ar q append" "cp base.a ours.a && $BU/ar q ours.a other.o" "cp base.a gnu.a && ${GNU}ar q gnu.a other.o"
ar_pair "ar r replace" "$BU/ar rcs ours.a prog.o other.o && $BU/ar r ours.a other.o" \
    "${GNU}ar rcs gnu.a prog.o other.o && ${GNU}ar r gnu.a other.o"
cd "$TOP" || exit 1
