# The comparison of readelf and objdump with the GNU programs
# (docs/design/binutils.md). run.sh sources this file once per
# architecture. compare and check, the variables of the corpus, BU, GNU,
# ARCH and AWORK come from run.sh. The files that this script writes start
# with rd_.

# re_cmp NAME ARGS... compares readelf -W with the GNU program. ARGS end
# with the files.
re_cmp() {
    name=$1
    shift
    compare "readelf $name" "$BU/readelf -W $*" "${GNU}readelf -W $*"
}

# ob_cmp NAME ARGS... compares objdump with the GNU program.
ob_cmp() {
    name=$1
    shift
    compare "objdump $name" "$BU/objdump $*" "${GNU}objdump $*"
}

# ---- files that the corpus lacks ----

# rd_lib.c has thread local data, data with relocations, a function table
# and symbols with versions.
cat > "$AWORK/rd_lib.c" << 'EOT'
int exported_value = 5;
int *exported_pointer = &exported_value;
__thread int thread_value = 3;
extern int external_value;
static int helper(int a)
{
    return a + 1;
}
int (*function_pointer)(int) = helper;
const char *message = "hello";
int get_total(void)
{
    return external_value + *exported_pointer + thread_value + function_pointer(1);
}
EOT
cat > "$AWORK/rd_lib.map" << 'EOT'
VERS_1 { global: get_total; exported_pointer; function_pointer; local: *; };
VERS_2 { global: message; } VERS_1;
EOT
cat > "$AWORK/rd_user.c" << 'EOT'
extern int get_total(void);
int external_value;
void *__tls_get_addr(void *p)
{
    return p;
}
void _start(void)
{
    get_total();
}
EOT
# rd_symbols.c has a symbol of each binding, visibility and type.
cat > "$AWORK/rd_symbols.c" << 'EOT'
int global_data = 1;
int weak_data __attribute__((weak)) = 2;
int hidden_data __attribute__((visibility("hidden"))) = 3;
int protected_data __attribute__((visibility("protected"))) = 4;
int common_data;
int common_array[10];
__thread int thread_data = 5;
static int local_data = 6;
void global_function(void) {}
void weak_function(void) __attribute__((weak));
void weak_function(void) {}
void hidden_function(void) __attribute__((visibility("hidden")));
void hidden_function(void) {}
void use(void)
{
    local_data++;
}
__attribute__((section(".custom_data"))) int custom_data = 7;
__asm__(".globl absolute_symbol\n.set absolute_symbol, 0x1234\n");
EOT
PB='@progbits'
NT='@note'
if [ "$ARCH" = aarch64 ]; then
    PB='%progbits'
    NT='%note'
fi
# rd_group.s has two COMDAT groups. rd_notes.s has notes of several kinds.
cat > "$AWORK/rd_group.s" << EOT
.section .text.first,"axG",$PB,group_one,comdat
.globl first
first: .long 1
.section .data.second,"awG",$PB,group_one,comdat
.long 2
.section .text.third,"axG",$PB,group_two,comdat
third: .long 3
.section .text.plain,"ax",$PB
.long 4
EOT
cat > "$AWORK/rd_notes.s" << EOT
.section .note.abi,"a",$NT
.balign 4
.long 4, 16, 1
.string "GNU"
.long 0, 3, 2, 0
.section .note.gold,"a",$NT
.balign 4
.long 4, 12, 4
.string "GNU"
.string "gold 1.16"
.byte 0, 0, 0
.section .note.hwcap,"a",$NT
.balign 4
.long 4, 8, 2
.string "GNU"
.long 1, 2
.section .note.other,"a",$NT
.balign 4
.long 4, 5, 77
.string "FOO"
.byte 1, 2, 3, 4, 5, 0, 0, 0
.section .note.empty,"a",$NT
.balign 4
.long 1, 0, 9
.byte 0, 0, 0, 0
EOT
# rd_strings.s has strings with control characters and new lines.
cat > "$AWORK/rd_strings.s" << EOT
.section .strdata,"a",$PB
.byte 0x61, 0x09, 0x62, 0x0d, 0x63, 0x7f, 0x64, 0x01, 0x65, 0x1b, 0x66, 0x0a, 0x67, 0x00
.byte 0x68, 0x0a, 0x0a, 0x00, 0x69, 0x00, 0x6a, 0x0a, 0x00, 0x6b, 0x00
.byte 0x41, 0xc3, 0xa9, 0x42, 0x00, 0x80, 0x81, 0x43, 0x00
EOT
RD_LIB_O=$AWORK/rd_lib.o
RD_SO=$AWORK/rd_lib.so
RD_SO_LAZY=$AWORK/rd_lazy.so
RD_USER_O=$AWORK/rd_user.o
RD_EXEC=$AWORK/rd_user.elf
RD_SYMBOLS=$AWORK/rd_symbols.o
RD_GROUP=$AWORK/rd_group.o
RD_NOTES=$AWORK/rd_notes.o
RD_STRINGS=$AWORK/rd_strings.o
RD_DYN=$AWORK/rd_dyn.so
RD_AR=$AWORK/rd_mixed.a
printf 'this is not an ELF file\n' > "$AWORK/rd_plain.txt"
"${GNU}gcc" -c -fPIC -O1 -ffreestanding -o "$RD_LIB_O" "$AWORK/rd_lib.c" || fail=1
"${GNU}gcc" -c -O1 -ffreestanding -o "$RD_USER_O" "$AWORK/rd_user.c" || fail=1
"${GNU}gcc" -c -fcommon -O1 -ffreestanding -o "$RD_SYMBOLS" "$AWORK/rd_symbols.c" || fail=1
"${GNU}as" -o "$RD_GROUP" "$AWORK/rd_group.s" || fail=1
"${GNU}as" -o "$RD_NOTES" "$AWORK/rd_notes.s" || fail=1
"${GNU}as" -o "$RD_STRINGS" "$AWORK/rd_strings.s" || fail=1
"${GNU}ld" -shared -z now --hash-style=both --build-id --version-script="$AWORK/rd_lib.map" -o "$RD_SO" "$RD_LIB_O" || fail=1
"${GNU}ld" -shared -z lazy --version-script="$AWORK/rd_lib.map" -o "$RD_SO_LAZY" "$RD_LIB_O" || fail=1
"${GNU}ld" -o "$RD_EXEC" "$RD_USER_O" "$RD_SO" --dynamic-linker /lib/ld.so -rpath /lib || fail=1
"${GNU}ld" -shared -soname librd.so -rpath /a:/b -z origin -z nodelete -z initfirst -Bsymbolic -o "$RD_DYN" "$RD_LIB_O" || fail=1
"${GNU}ar" rc "$RD_AR" "$RD_SYMBOLS" "$AWORK/rd_plain.txt" "$RD_GROUP" 2> /dev/null

FILES="$OBJ $ARCHIVE $SHARED $EXEC $STRIPPED $KERNEL $DWARF_EXEC"
EXTRA="$RD_LIB_O $RD_SO $RD_SO_LAZY $RD_EXEC $RD_SYMBOLS $RD_GROUP $RD_NOTES $RD_DYN"

# ---- readelf on every file of the corpus ----

for f in $FILES; do
    base=$(basename "$f")
    re_cmp "-h $base" -h "$f"
    re_cmp "-l $base" -l "$f"
    re_cmp "-S $base" -S "$f"
    re_cmp "-s $base" -s "$f"
    re_cmp "-r $base" -r "$f"
    re_cmp "-d $base" -d "$f"
    re_cmp "-a $base" -a "$f"
done

# ---- readelf on selected files, option by option ----

for f in $OBJ $ARCHIVE $SHARED $EXEC $STRIPPED $KERNEL; do
    base=$(basename "$f")
    re_cmp "-e $base" -e "$f"
    re_cmp "--dyn-syms $base" --dyn-syms "$f"
    re_cmp "-n $base" -n "$f"
    re_cmp "-V $base" -V "$f"
    re_cmp "-A $base" -A "$f"
    re_cmp "-u $base" -u "$f"
    re_cmp "-g $base" -g "$f"
    re_cmp "-I $base" -I "$f"
    re_cmp "--got-contents $base" --got-contents "$f"
    re_cmp "-x .comment $base" -x .comment "$f"
    re_cmp "-p .comment $base" -p .comment "$f"
done
for f in $OBJ $SHARED $EXEC $STRIPPED $KERNEL; do
    base=$(basename "$f")
    re_cmp "-x .rodata $base" -x .rodata "$f"
    re_cmp "-p .rodata $base" -p .rodata "$f"
    re_cmp "-x .text $base" -x .text "$f"
    re_cmp "-p .strtab $base" -p .strtab "$f"
    re_cmp "-x 1 -p 1 $base" -x 1 -p 1 "$f"
done

# ---- combinations and several files ----

re_cmp "-S -s" -S -s "$EXEC"
re_cmp "-S -s -r" -S -s -r "$OBJ"
re_cmp "-a on the archive" -a "$ARCHIVE"
re_cmp "-s on the archive" -s "$ARCHIVE"
re_cmp "-h on two files" -h "$SHARED" "$EXEC"
re_cmp "-S on a file and an archive" -S "$OBJ" "$ARCHIVE"
re_cmp "-l -d on three files" -l -d "$SHARED" "$EXEC" "$KERNEL"
re_cmp "-x -p .comment on two files" -x .comment -p .comment "$OBJ" "$SHARED"
re_cmp "options after the file" "$EXEC" -h -W
re_cmp "--file-header" --file-header "$EXEC"
re_cmp "--program-headers" --program-headers "$EXEC"
re_cmp "--segments" --segments "$EXEC"
re_cmp "--sections" --sections "$EXEC"
re_cmp "--section-headers" --section-headers "$EXEC"
re_cmp "--headers" --headers "$EXEC"
re_cmp "--syms" --syms "$EXEC"
re_cmp "--symbols" --symbols "$EXEC"
re_cmp "--relocs" --relocs "$OBJ"
re_cmp "--dynamic" --dynamic "$SHARED"
re_cmp "--notes" --notes "$EXEC"
re_cmp "--version-info" --version-info "$SHARED"
re_cmp "--arch-specific" --arch-specific "$EXEC"
re_cmp "--unwind" --unwind "$EXEC"
re_cmp "--section-groups" --section-groups "$OBJ"
re_cmp "--histogram" --histogram "$SHARED"
re_cmp "--all" --all "$SHARED"
re_cmp "--hex-dump" --hex-dump=.comment "$EXEC"
re_cmp "--string-dump" --string-dump=.comment "$EXEC"
re_cmp "-x number" -x 1 "$SHARED"
re_cmp "-x missing section" -x .missing "$EXEC"
re_cmp "-x number out of range" -x 999 "$EXEC"
re_cmp "-x .bss" -x .bss "$EXEC"
re_cmp "text file" -h "$AWORK/rd_plain.txt"
re_cmp "missing file" -h "$AWORK/rd_missing"
re_cmp "archive with a text member" -h "$RD_AR"

# ---- readelf on the files that the script builds ----

for f in $EXTRA; do
    base=$(basename "$f")
    re_cmp "-a $base" -a "$f"
    re_cmp "-S -l $base" -S -l "$f"
    re_cmp "-s -r $base" -s -r "$f"
done
re_cmp "-g rd_group.o" -g "$RD_GROUP"
re_cmp "-n rd_notes.o" -n "$RD_NOTES"
re_cmp "-V rd_lib.so" -V "$RD_SO"
re_cmp "-V rd_user.elf" -V "$RD_EXEC"
re_cmp "-I rd_lib.so" -I "$RD_SO"
re_cmp "--got-contents rd_lazy.so" --got-contents "$RD_SO_LAZY"
re_cmp "-p .strdata" -p .strdata "$AWORK/rd_strings.o"
re_cmp "-x .strdata" -x .strdata "$AWORK/rd_strings.o"
re_cmp "-a on the archive of mixed members" -a "$RD_AR"

# ---- objdump on every file of the corpus ----

for f in $FILES; do
    base=$(basename "$f")
    ob_cmp "-f $base" -f "$f"
    ob_cmp "-h $base" -h "$f"
    ob_cmp "-p $base" -p "$f"
    ob_cmp "-x $base" -x "$f"
    ob_cmp "-t $base" -t "$f"
    ob_cmp "-T $base" -T "$f"
    ob_cmp "-r $base" -r "$f"
    ob_cmp "-R $base" -R "$f"
    ob_cmp "-s $base" -s "$f"
done
for f in $FILES; do
    base=$(basename "$f")
    ob_cmp "-a $base" -a "$f"
    ob_cmp "-h -w $base" -h -w "$f"
    ob_cmp "-s -j .comment $base" -s -j .comment "$f"
done
for f in $OBJ $SHARED $EXEC $STRIPPED $KERNEL; do
    base=$(basename "$f")
    ob_cmp "-s -j .text $base" -s -j .text "$f"
    ob_cmp "-h -j .text -j .data $base" -h -j .text -j .data "$f"
    ob_cmp "-t -j .text $base" -t -j .text "$f"
done

# ---- objdump combinations and several files ----

ob_cmp "-h -t" -h -t "$OBJ"
ob_cmp "-f -h -p" -f -h -p "$SHARED"
ob_cmp "-r -s -j .comment" -r -s -j .comment "$OBJ"
ob_cmp "-x -w" -x -w "$EXEC"
ob_cmp "-t -T -R" -t -T -R "$EXEC"
ob_cmp "-r -j .text" -r -j .text "$OBJ"
ob_cmp "-s -w" -s -w "$EXEC"
ob_cmp "-a on the archive" -a "$ARCHIVE"
ob_cmp "-x on the archive" -x "$ARCHIVE"
ob_cmp "-f on a file and an archive" -f "$OBJ" "$ARCHIVE"
ob_cmp "-h on two files" -h "$SHARED" "$EXEC"
ob_cmp "-a -f -h on the archive" -afh "$ARCHIVE"
ob_cmp "--archive-headers" --archive-headers "$ARCHIVE"
ob_cmp "--file-headers" --file-headers "$EXEC"
ob_cmp "--section-headers" --section-headers "$EXEC"
ob_cmp "--headers" --headers "$EXEC"
ob_cmp "--private-headers" --private-headers "$SHARED"
ob_cmp "--all-headers" --all-headers "$OBJ"
ob_cmp "--syms" --syms "$OBJ"
ob_cmp "--dynamic-syms" --dynamic-syms "$SHARED"
ob_cmp "--reloc" --reloc "$OBJ"
ob_cmp "--dynamic-reloc" --dynamic-reloc "$EXEC"
ob_cmp "--full-contents" --full-contents -j .rodata "$EXEC"
ob_cmp "--section" --section=.text -h "$EXEC"
ob_cmp "--wide" --wide -h "$KERNEL"
ob_cmp "-j missing" -h -j .missing "$EXEC"
ob_cmp "text file" -f "$AWORK/rd_plain.txt"
ob_cmp "missing file" -f "$AWORK/rd_missing"
ob_cmp "archive with a text member" -f "$RD_AR"

# ---- objdump on the files that the script builds ----

for f in $EXTRA; do
    base=$(basename "$f")
    ob_cmp "-x $base" -x "$f"
    ob_cmp "-T -R $base" -T -R "$f"
    ob_cmp "-s $base" -s "$f"
done
ob_cmp "-s rd_strings.o" -s "$RD_STRINGS"
ob_cmp "-h rd_group.o" -h "$RD_GROUP"
ob_cmp "-p rd_user.elf" -p "$RD_EXEC"
ob_cmp "-p rd_lib.so" -p "$RD_SO"
ob_cmp "-a rd_mixed.a" -a "$RD_AR"

# ---- messages of the programs ----

check "readelf without option fails" "! $BU/readelf $EXEC > /dev/null 2>&1"
check "readelf without file fails" "! $BU/readelf -h > /dev/null 2>&1"
check "readelf rejects a text file" "! $BU/readelf -h $AWORK/rd_plain.txt > /dev/null 2>&1"
check "objdump without option fails" "! $BU/objdump $EXEC > /dev/null 2>&1"
check "objdump rejects a text file" "! $BU/objdump -f $AWORK/rd_plain.txt > /dev/null 2>&1"
check "objdump rejects disassembly" "! $BU/objdump -d $EXEC > /dev/null 2>&1"
check "objdump -d reports the missing disassembly" "$BU/objdump -d $EXEC 2>&1 | grep -q 'disassembly is not available'"
check "objdump -S rejects disassembly" "! $BU/objdump -S $EXEC > /dev/null 2>&1"
