# The comparison of nm, size and strings with the GNU programs
# (docs/design/binutils.md). run.sh sources this file once per
# architecture. compare and check, the variables of the corpus, BU, GNU
# and AWORK come from run.sh.

# The object syms.o has symbols of every class: common, weak, thread
# local, indirect, absolute, undefined and symbols in sections without
# the allocate flag.
cat > "$AWORK/syms.c" << 'EOT'
int data1 = 5;
static int sdata = 6;
const int rodata1 = 7;
int zero_init = 0;
static int sbss;
int common1;
int common2[10];
__thread int tls1 = 3;
__thread int tlsbss;
extern int undef_data;
extern void undef_func(void);
extern int weak_undef __attribute__((weak));
extern void weak_func_undef(void) __attribute__((weak));
int weak_data __attribute__((weak)) = 1;
void weak_func(void) __attribute__((weak));
void weak_func(void) {}
static void sfunc(void) {}
void func(void)
{
    undef_func();
    if (weak_func_undef)
        weak_func_undef();
    undef_data++;
    weak_undef++;
    sfunc();
    sdata++;
    sbss++;
}
__attribute__((section(".mydata"))) int mydata = 1;
__attribute__((section(".myro"), used)) const int myro = 2;
__attribute__((section(".mytext"), used)) void mytext(void) {}
__asm__(".globl abs_sym\n.set abs_sym, 0x1234\n.set abs_local, 0x55\n"
        ".section .noalloc,\"\"\nnoalloc_local: .long 1\n.globl noalloc_global\nnoalloc_global: .long 2\n"
        ".globl ifunc_f\n.type ifunc_f, STT_GNU_IFUNC\nifunc_f: .long 0\n"
        ".section .noallocw,\"w\"\nnoallocw_sym: .long 3\n.text\n");
EOT
SYMS=$AWORK/syms.o
"${GNU}gcc" -c -fcommon -ffreestanding -O1 -g0 -o "$SYMS" "$AWORK/syms.c" || fail=1
printf 'this is not an ELF file\n' > "$AWORK/plain.txt"
cp "$ARCHIVE" "$AWORK/lib.a"
"${GNU}ar" rc "$AWORK/mixed.a" "$SYMS" "$AWORK/plain.txt" "$OBJ" 2> /dev/null
"${GNU}ar" rcS "$AWORK/noindex.a" "$SYMS" "$OBJ" 2> /dev/null
: > "$AWORK/empty"

# nmc ARGS runs nm and the GNU nm with the same arguments.
nmc() {
    compare "nm $*" "$BU/nm $*" "${GNU}nm $*"
}
sizec() {
    compare "size $*" "$BU/size $*" "${GNU}size $*"
}
strc() {
    compare "strings $*" "$BU/strings $*" "${GNU}strings $*"
}

# ---- nm: files and default options ----
for f in "$OBJ" "$ARCHIVE" "$SHARED" "$EXEC" "$STRIPPED" "$KERNEL" "$SYMS" "$DWARF_EXEC" "$AWORK/mixed.a"; do
    nmc "$f"
done
(cd "$AWORK" && cp "$SYMS" a.out && compare "nm a.out" "$BU/nm" "${GNU}nm")
nmc "$OBJ" "$SHARED"
nmc "$SYMS" "$ARCHIVE" "$EXEC"
nmc "$AWORK/plain.txt"
nmc "$AWORK/plain.txt" "$SYMS"
nmc "$AWORK/empty"
nmc "$AWORK/does-not-exist" "$SYMS"
nmc "$SYMS" "$SYMS"
nmc "$AWORK/noindex.a"
nmc -s "$AWORK/noindex.a"

# ---- nm: selection ----
for f in "$SYMS" "$SHARED" "$EXEC" "$STRIPPED" "$ARCHIVE"; do
    nmc -a "$f"
    nmc -g "$f"
    nmc -u "$f"
    nmc -U "$f"
    nmc -W "$f"
    nmc -D "$f"
done
nmc -a -g "$EXEC"
nmc -g -u "$SYMS"
nmc -g -U "$SYMS"
nmc -u -U "$SYMS"
nmc -a -u "$SYMS"
nmc -D -g "$SHARED"
nmc -D -u "$SHARED"
nmc -D -U "$SHARED"
nmc -D "$KERNEL"
nmc -D "$AWORK/mixed.a"
nmc -a "$KERNEL"
nmc -g "$KERNEL"

# ---- nm: sorting ----
for f in "$SYMS" "$SHARED" "$EXEC" "$KERNEL" "$OBJ"; do
    nmc -n "$f"
    nmc -v "$f"
    nmc -p "$f"
    nmc -r "$f"
    nmc -n -r "$f"
    nmc --size-sort "$f"
    nmc --size-sort -r "$f"
    nmc --numeric-sort -S "$f"
done
nmc -g -n "$EXEC"
nmc -g -n "$KERNEL"
nmc -u -n "$SYMS"
nmc -a -n "$SYMS"
nmc -a -n "$DWARF_EXEC"
nmc -n -S --size-sort "$SHARED"
nmc --no-sort "$SYMS"
nmc --reverse-sort "$SYMS"

# ---- nm: sizes, radix and formats ----
for f in "$SYMS" "$SHARED" "$EXEC" "$ARCHIVE"; do
    nmc -S "$f"
    nmc -S -n "$f"
    nmc -t d "$f"
    nmc -t o "$f"
    nmc -t x "$f"
    nmc -S -t d "$f"
    nmc -B "$f"
    nmc -P "$f"
    nmc -P -t d "$f"
    nmc -P -t o "$f"
    nmc -P -S "$f"
    nmc -f posix "$f"
    nmc -f sysv "$f"
    nmc -f sysv -S "$f"
    nmc -f sysv -t d "$f"
    nmc -f sysv -t o "$f"
    nmc -f bsd "$f"
    nmc -f just-symbols "$f"
done
nmc --format=sysv "$SYMS"
nmc --format posix "$SYMS"
nmc --radix=d "$SYMS"
nmc --radix o -S "$SYMS"
nmc -td -S "$SYMS"
nmc -fsysv -u "$SYMS"
nmc -f sysv -u "$ARCHIVE"
nmc -f sysv -a "$EXEC"
nmc -f sysv -g "$SHARED"
nmc -f sysv -D "$SHARED"
nmc -P -u "$ARCHIVE"
nmc -P -g -n "$SYMS"
nmc -P -a "$EXEC"
nmc -P "$OBJ" "$SHARED"
nmc -P -A "$OBJ" "$SHARED"
nmc -B -S -t d "$SYMS"
nmc -j "$SYMS"

# ---- nm: file names and the symbol index of archives ----
for f in "$SYMS" "$ARCHIVE" "$SHARED" "$AWORK/mixed.a"; do
    nmc -A "$f"
    nmc -o "$f"
    nmc --print-file-name "$f"
    nmc -A -u "$f"
    nmc -A -g "$f"
    nmc -A -P "$f"
    nmc -A -f sysv "$f"
    nmc -s "$f"
    nmc -s -P "$f"
    nmc -s -f sysv "$f"
    nmc -s -A "$f"
done
nmc -A "$OBJ" "$SHARED"
nmc -A -u "$OBJ" "$SHARED" "$ARCHIVE"
nmc -A -n "$ARCHIVE"
nmc --print-armap "$ARCHIVE" "$OBJ"
nmc -s "$OBJ" "$ARCHIVE" "$AWORK/mixed.a"

# ---- nm: option errors and the output on standard error ----
check "nm unknown option" "$BU/nm --bogus $SYMS > /dev/null 2>&1; [ \$? -ne 0 ]"
check "nm bad radix" "$BU/nm -t z $SYMS > /dev/null 2>&1; [ \$? -ne 0 ]"
check "nm missing file status" "$BU/nm $AWORK/does-not-exist > /dev/null 2>&1; [ \$? -ne 0 ]"
check "nm non-ELF status" "$BU/nm $AWORK/plain.txt > /dev/null 2>&1; [ \$? -ne 0 ]"
check "nm non-ELF message" "$BU/nm $AWORK/plain.txt 2>&1 | grep -q 'file format not recognized'"

# ---- size ----
for f in "$OBJ" "$ARCHIVE" "$SHARED" "$EXEC" "$STRIPPED" "$KERNEL" "$SYMS" "$DWARF_EXEC" "$AWORK/mixed.a"; do
    sizec "$f"
    sizec -A "$f"
    sizec -B "$f"
    sizec -G "$f"
    sizec -d "$f"
    sizec -o "$f"
    sizec -x "$f"
    sizec -A -x "$f"
    sizec -A -d "$f"
    sizec -A -o "$f"
    sizec -G -x "$f"
    sizec -G -o "$f"
    sizec -t "$f"
    sizec --totals -x "$f"
    sizec --format=berkeley --radix=16 "$f"
    sizec --format=sysv --radix=8 "$f"
    sizec --common "$f"
    sizec --common -A "$f"
done
(cd "$AWORK" && compare "size a.out" "$BU/size" "${GNU}size")
sizec "$OBJ" "$SHARED" "$EXEC"
sizec -t "$OBJ" "$SHARED" "$EXEC"
sizec -A "$OBJ" "$SHARED"
sizec -G -t "$OBJ" "$SHARED"
sizec -x -t "$ARCHIVE" "$OBJ"
sizec "$AWORK/plain.txt" "$SYMS"
sizec "$AWORK/does-not-exist" "$SYMS"
sizec --format gnu "$SYMS"
sizec --radix 10 "$SYMS"
sizec -f "$SYMS"
check "size missing file status" "$BU/size $AWORK/does-not-exist > /dev/null 2>&1; [ \$? -ne 0 ]"
check "size bad radix" "$BU/size --radix=3 $SYMS > /dev/null 2>&1; [ \$? -ne 0 ]"

# ---- strings ----
head -c 100000 /dev/zero > "$AWORK/zeros.bin"
printf 'abc\000defgh\001ij klm\tnop\nqrst\r\nuvwxyz\0\0' > "$AWORK/mix.bin"
printf 'a\000b\000c\000d\000e\000f\000\000\000h\000i\000j\000k\000\000\000' > "$AWORK/utf16le.bin"
printf '\000a\000b\000c\000d\000e\000f\000\000\000h\000i\000j\000k\000' > "$AWORK/utf16be.bin"
printf 'a\000\000\000b\000\000\000c\000\000\000d\000\000\000e\000\000\000' > "$AWORK/utf32le.bin"
printf '\000\000\000a\000\000\000b\000\000\000c\000\000\000d\000\000\000e' > "$AWORK/utf32be.bin"
printf 'caf\303\251 latin \351t\351 plain\200\201\202\203 end' > "$AWORK/high.bin"
for f in "$OBJ" "$ARCHIVE" "$SHARED" "$EXEC" "$STRIPPED" "$KERNEL" "$SYMS" "$AWORK/mix.bin" "$AWORK/high.bin"; do
    strc "$f"
    strc -a "$f"
    strc --all "$f"
    strc -d "$f"
    strc --data "$f"
    strc -f "$f"
    strc --print-file-name "$f"
    strc -n 8 "$f"
    strc -n8 "$f"
    strc --bytes=12 "$f"
    strc --bytes 3 "$f"
    strc -3 "$f"
    strc -10 "$f"
    strc -t d "$f"
    strc -t o "$f"
    strc -t x "$f"
    strc -tx "$f"
    strc --radix=x "$f"
    strc -o "$f"
    strc -t x -n 8 "$f"
    strc -f -t x "$f"
    strc -d -t x "$f"
    strc -d -f -n 6 "$f"
    strc -w "$f"
    strc --include-all-whitespace "$f"
    strc -e S "$f"
    strc -e s "$f"
    strc -s '|' "$f"
    strc --output-separator=', ' "$f"
done
strc -e b "$AWORK/utf16be.bin"
strc -e l "$AWORK/utf16le.bin"
strc -e L "$AWORK/utf32le.bin"
strc -e B "$AWORK/utf32be.bin"
strc -e l -t x "$AWORK/utf16le.bin"
strc -e b -f "$AWORK/utf16be.bin"
strc -e l "$AWORK/utf16be.bin"
strc -e L -n 3 "$AWORK/utf32le.bin"
strc --encoding=S "$AWORK/high.bin"
strc --encoding l "$AWORK/utf16le.bin"
strc -e S -t d "$AWORK/high.bin"
strc -e l "$EXEC"
strc -e b "$KERNEL"
strc -w -n 2 "$AWORK/mix.bin"
strc -w -t d "$AWORK/mix.bin"
strc "$SYMS" "$OBJ"
strc -f "$SYMS" "$OBJ"
strc -d "$SYMS" "$OBJ"
strc -t x "$AWORK/zeros.bin" "$AWORK/empty"
strc "$AWORK/does-not-exist" "$AWORK/mix.bin"
compare "strings stdin" "$BU/strings < $AWORK/mix.bin" "${GNU}strings < $AWORK/mix.bin"
compare "strings stdin -t x" "$BU/strings -t x < $AWORK/mix.bin" "${GNU}strings -t x < $AWORK/mix.bin"
compare "strings stdin -f" "$BU/strings -f < $AWORK/mix.bin" "${GNU}strings -f < $AWORK/mix.bin"
compare "strings stdin pipe" "cat $EXEC | $BU/strings -n 6" "cat $EXEC | ${GNU}strings -n 6"
compare "strings dash" "$BU/strings - < $AWORK/mix.bin" "${GNU}strings - < $AWORK/mix.bin"
check "strings missing file status" "$BU/strings $AWORK/does-not-exist > /dev/null 2>&1; [ \$? -ne 0 ]"
check "strings bad radix" "$BU/strings -t z $SYMS > /dev/null 2>&1; [ \$? -ne 0 ]"
