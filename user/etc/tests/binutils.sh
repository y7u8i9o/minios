# binutils test, run as: sh /etc/tests/binutils.sh. The programs read and
# change objects that tcc compiles inside minios (docs/design/binutils.md).
# Every failing check prints a line starting with FAIL and the value
# received. The last line gives the number of failed checks, and the case
# requires 0. The host comparison with the GNU binutils is make
# check-binutils.
failed=0
check() {
    if test "$2" != "$3"; then
        echo "FAIL $1: [$2]"
        failed=$((failed + 1))
    fi
}
mkdir /bt
cd /bt
cat > twice.c <<'C'
int twice(int x)
{
    return 2 * x;
}
C
cat > main.c <<'C'
#include <stdio.h>
int twice(int);
int counter;
int main(void)
{
    printf("%d\n", twice(21));
    return 0;
}
C
tcc -gdwarf -c twice.c -o twice.o; check compile-twice "$?" "0"
tcc -gdwarf -c main.c -o main.o; check compile-main "$?" "0"
tcc -gdwarf -o prog main.o twice.o; check link "$?" "0"
check run "$(./prog)" "42"

# nm, size and strings.
check nm-defined "$(nm twice.o | grep ' T ' | cut -d' ' -f2-)" "T twice"
check nm-undefined "$(nm -u main.o | awk '{ print $1 $2 }' | tr '\n' ' ')" "Uprintf Utwice "
check nm-common "$(nm main.o | grep -c -E ' [BC] counter$')" "1"
check nm-posix "$(nm -P twice.o | grep '^twice ' | cut -d' ' -f1-2)" "twice T"
check size-header "$(size twice.o | head -n 1 | tr -s ' \t' ' ')" " text data bss dec hex filename"
check strings "$(strings prog | grep -c '^/lib/ld\.so$')" "1"

# readelf and objdump.
check readelf-type "$(readelf -h twice.o | grep Type: | tr -s ' ')" " Type: REL (Relocatable file)"
check readelf-machine "$(readelf -h prog | grep -c -E 'Machine: +(Advanced Micro Devices X86-64|AArch64)')" "1"
check readelf-symbol "$(readelf -s twice.o | grep -c ' FUNC  *GLOBAL .* twice$')" "1"
check readelf-needed "$(readelf -d prog | grep -c 'Shared library: \[libc.so\]')" "1"
check objdump-text "$(objdump -h twice.o | grep -c ' \.text ')" "1"
check objdump-syms "$(objdump -t twice.o | grep -c 'F \.text.*twice$')" "1"

# addr2line on the DWARF information of tcc.
addr=$(nm prog | grep ' T twice$' | cut -d' ' -f1)
check addr2line "$(addr2line -f -s -e prog 0x$addr | tr '\n' ' ' | grep -c -E '^twice twice\.c:[12] $')" "1"

# ar, nm -s and ranlib.
ar rcS libt.a twice.o; check ar-no-index "$(nm -s libt.a | grep -c 'Archive index')" "0"
ranlib libt.a; check ranlib "$(nm -s libt.a | grep -c '^twice in twice.o$')" "1"
ar rcs libt2.a twice.o; check ar-index "$(nm -s libt2.a | grep -c '^twice in twice.o$')" "1"
tcc -o prog2 main.o -L. -lt; check link-archive "$(./prog2)" "42"

# strip and objcopy.
cp prog prog.s
strip prog.s; check strip-status "$?" "0"
check strip-run "$(./prog.s)" "42"
check strip-symtab "$(readelf -S prog.s | grep -c -E ' \.symtab | \.debug_')" "0"
objcopy --strip-debug prog prog.g; check strip-debug-run "$(./prog.g)" "42"
check strip-debug-symtab "$(nm prog.g | grep -c ' T twice$')" "1"
strip --strip-unneeded -o twice.u.o twice.o
tcc -o prog3 main.o twice.u.o; check strip-unneeded-link "$(./prog3)" "42"
objcopy -O binary -j .text twice.o twice.bin; check objcopy-binary "$?" "0"
check objcopy-binary-size "$(wc -c < twice.bin | tr -d ' ')" "$(size -A twice.o | awk '$1 == ".text" { print $2 }')"
echo "binutils: $failed failed"
