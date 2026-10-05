# Tests of objcopy and strip, sourced by run.sh once per architecture
# (docs/design/binutils.md). Each command line runs with the minios program
# and with the GNU program on a file of the corpus. The results must agree
# in the section table without the file offsets, the symbol tables, the
# program headers, the relocations and the loaded image. A relocatable
# object must remain an input of the GNU linker, and the symbol index of an
# archive must remain equal.

OC=$AWORK/oc
mkdir -p "$OC/ours" "$OC/gnu"
OC_ASPECT=$OC/aspect.sh
cat > "$OC_ASPECT" << 'EOS'
# aspect.sh GNU-PREFIX ASPECT DIRECTORY prints one dump of the file x.
G=$1
cd "$3" || exit 1
case $2 in
sections)
    "${G}readelf" -S -W x 2>&1 | sed -E 's/, starting at offset .*//; s/(^ +\[ *[0-9]+\] .* [0-9a-f]{16}) [0-9a-f]{6,} /\1 /' ;;
symbols)
    "${G}readelf" -s -W x 2>&1 ;;
segments)
    "${G}readelf" -l -W x 2>&1 ;;
relocs)
    "${G}readelf" -r -W x 2>&1 | sed -E 's/ at offset 0x[0-9a-f]+//' ;;
nm)
    "${G}nm" -s x 2>&1 ;;
esac
EOS

# oc_aspect NAME ASPECT compares one dump of both results.
oc_aspect() {
    check "$ARCH $1 $2" "sh '$OC_ASPECT' '$GNU' $2 '$OC/ours' > '$OC/a.txt' && sh '$OC_ASPECT' '$GNU' $2 '$OC/gnu' > '$OC/b.txt' && diff -u '$OC/b.txt' '$OC/a.txt'"
}

# oc_run TOOL OUTPUT FILE OPTION... runs a tool of the prefix GNU or of BU.
oc_run() {
    tool=$1; out=$2; file=$3; shift 3
    case $tool in
    strip) "$@" -o "$out" "$file" ;;
    *) "$@" "$file" "$out" ;;
    esac
}

# oc_verify NAME TOOL KIND FILE OPTION... runs TOOL with both programs.
# KIND is obj, archive or exec.
oc_verify() {
    name=$1; tool=$2; kind=$3; file=$4; shift 4
    rm -f "$OC/ours/x" "$OC/gnu/x"
    if ! oc_run "$tool" "$OC/ours/x" "$file" "$BU/$tool" "$@" 2> "$OC/ours.err"; then
        check "$ARCH $name ours" "cat '$OC/ours.err'; false"
        return
    fi
    if ! oc_run "$tool" "$OC/gnu/x" "$file" "${GNU}$tool" "$@" 2> "$OC/gnu.err"; then
        check "$ARCH $name gnu" "cat '$OC/gnu.err'; false"
        return
    fi
    oc_aspect "$name" sections
    oc_aspect "$name" symbols
    oc_aspect "$name" relocs
    case $kind in
    obj)
        check "$ARCH $name ld -r" "${GNU}ld -r -o '$OC/ld.o' '$OC/ours/x'" ;;
    archive)
        oc_aspect "$name" nm ;;
    exec)
        oc_aspect "$name" segments
        check "$ARCH $name image" "${GNU}objcopy -O binary '$OC/ours/x' '$OC/ours.bin' && ${GNU}objcopy -O binary '$OC/gnu/x' '$OC/gnu.bin' && cmp '$OC/gnu.bin' '$OC/ours.bin'" ;;
    esac
}

# oc_binary NAME FILE OPTION... compares objcopy -O binary.
oc_binary() {
    name=$1; file=$2; shift 2
    check "$ARCH $name" "'$BU/objcopy' -O binary $* '$file' '$OC/ours.bin' && ${GNU}objcopy -O binary $* '$file' '$OC/gnu.bin' && cmp '$OC/gnu.bin' '$OC/ours.bin'"
}

echo test > "$OC/added.txt"

for tool_opts in "strip -s" "strip -g" "strip --strip-unneeded" "strip -R .comment" "strip -s -K main -K theme_px -K .LC0" \
        "strip -N main -N spawn -N ui_fonts" "strip -g -R .comment" "strip --strip-unneeded -K spawn" \
        "strip -R .note* -g" "strip -s -R .comment" "strip --strip-debug --remove-section=.comment" \
        "objcopy -S" "objcopy -g" "objcopy --strip-unneeded" "objcopy --add-section .added=$OC/added.txt" \
        "objcopy -R .comment --add-section .added=$OC/added.txt" "objcopy -R .comment" "objcopy -p"; do
    set -- $tool_opts
    tool=$1; shift
    label=$(echo "$tool_opts" | sed "s|$OC/||g" | tr ' /' '__')
    oc_verify "obj $label" "$tool" obj "$OBJ" "$@"
    oc_verify "archive $label" "$tool" archive "$ARCHIVE" "$@"
    oc_verify "shared $label" "$tool" exec "$SHARED" "$@"
    oc_verify "exec $label" "$tool" exec "$EXEC" "$@"
    oc_verify "kernel $label" "$tool" exec "$KERNEL" "$@"
    oc_verify "dwarf $label" "$tool" exec "$DWARF_EXEC" "$@"
    oc_verify "stripped $label" "$tool" exec "$STRIPPED" "$@"
done

for f in "exec $EXEC" "shared $SHARED" "kernel $KERNEL" "dwarf $DWARF_EXEC" "stripped $STRIPPED"; do
    set -- $f
    oc_binary "binary $1" "$2"
    oc_binary "binary $1 -j .text" "$2" -j .text
    oc_binary "binary $1 -R .text" "$2" -R .text
done
oc_binary "binary obj" "$OBJ"
oc_binary "binary obj -j .text" "$OBJ" -j .text

# Several files, the preserved dates and the diagnostics.
check "$ARCH strip several files" "cp '$EXEC' '$OC/s1' && cp '$SHARED' '$OC/s2' && '$BU/strip' -g '$OC/s1' '$OC/s2' && cp '$EXEC' '$OC/g1' && cp '$SHARED' '$OC/g2' && ${GNU}strip -g '$OC/g1' '$OC/g2' && ${GNU}readelf -S -W '$OC/s1' | grep -c debug | grep -qx 0 && ${GNU}readelf -S -W '$OC/s2' | grep -c debug | grep -qx 0"
check "$ARCH strip in place equals output" "cp '$EXEC' '$OC/p1' && '$BU/strip' -s '$OC/p1' && '$BU/strip' -s -o '$OC/p2' '$EXEC' && cmp '$OC/p1' '$OC/p2'"
check "$ARCH objcopy -p dates" "cp '$EXEC' '$OC/d1' && touch -t 200001010000 '$OC/d1' && '$BU/objcopy' -p -g '$OC/d1' && ls -l '$OC/d1' | grep -q 2000"
check "$ARCH strip -p dates" "cp '$EXEC' '$OC/d2' && touch -t 200001010000 '$OC/d2' && '$BU/strip' -p -g '$OC/d2' && ls -l '$OC/d2' | grep -q 2000"
check "$ARCH strip rejects text" "echo hello > '$OC/t.txt' && ! '$BU/strip' '$OC/t.txt' 2> '$OC/t.err' && grep -q 'file format not recognized' '$OC/t.err'"
check "$ARCH objcopy rejects text" "echo hello > '$OC/t.txt' && ! '$BU/objcopy' '$OC/t.txt' '$OC/t2' 2> '$OC/t.err' && grep -q 'file format not recognized' '$OC/t.err'"
check "$ARCH objcopy missing file" "! '$BU/objcopy' '$OC/none' '$OC/t2' 2> '$OC/t.err' && grep -q 'No such file' '$OC/t.err'"
case $ARCH in
x86_64) OC_BFD=elf64-x86-64 ;;
*) OC_BFD=elf64-littleaarch64 ;;
esac
check "$ARCH objcopy -O elf64 equals copy" "'$BU/objcopy' -O $OC_BFD '$EXEC' '$OC/f1' && ${GNU}objcopy '$EXEC' '$OC/f2' && ${GNU}readelf -l -W '$OC/f1' > '$OC/f1.txt' && ${GNU}readelf -l -W '$OC/f2' > '$OC/f2.txt' && cmp '$OC/f1.txt' '$OC/f2.txt'"
