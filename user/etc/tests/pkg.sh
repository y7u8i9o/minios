# Package installer test, run as: sh /etc/tests/pkg.sh. Every failing check
# prints a line starting with FAIL and the value received. The prefix is
# /home/.local on the root image, since the test boots without a data
# volume. The fixtures under /etc/tests/pkgfix are a library in two builds
# and a program calling it; pkghello-1.0.mpk was built on the host. The
# variables let the same script drive a host build of pkg.
check() {
    test "$2" = "$3" || echo "FAIL $1: [$2]"
}
P=${P:-/home/.local}
PKG=${PKG:-pkg}
FIX=${FIX:-/etc/tests/pkgfix}
ABI=${ABI:-/lib/abi}
HELLO=${HELLO:-/etc/tests/pkghello-1.0.mpk}
WORK=${WORK:-/pt}
mkdir -p $WORK
cd $WORK
check list-empty "$($PKG list)" ""

# The library package, then the program package that depends on it.
mkdir -p lib/files/lib
printf 'name pkgfix\nversion 1.0\nsummary A library\nprovides libpkgfix.so 1\n' > lib/manifest
cp $FIX/libpkgfix.so lib/files/lib/libpkgfix.so
check build-lib "$($PKG build lib)" "pkgfix-1.0.mpk"
check info-needs "$($PKG info pkgfix-1.0.mpk | grep needs)" "needs libc.so 1"
check info-file "$($PKG info pkgfix-1.0.mpk | grep '^file' | cut -d' ' -f2)" "lib/libpkgfix.so"
check info-arch "$($PKG info pkgfix-1.0.mpk | grep '^arch')" "arch $(uname -m)"

# A package for the other machine is refused.
OTHER=aarch64
test "$(uname -m)" = aarch64 && OTHER=x86_64
mkdir -p other/files/share
printf 'data\n' > other/files/share/data
printf 'name pkgother\nversion 1.0\nsummary Another machine\narch %s\n' $OTHER > other/manifest
check build-other "$($PKG build other)" "pkgother-1.0.mpk"
check arch-refused "$($PKG install pkgother-1.0.mpk 2>&1)" "pkg: pkgother: the package is built for $OTHER, and the system is $(uname -m)"

mkdir -p prog/files/bin prog/files/share/pkgprog
printf 'name pkgprog\nversion 1.0\nsummary A program\ndepends pkgfix >= 1.0\nneeds libpkgfix.so 1\nlauncher Prog bin/pkgprog\nmime-type text/x-pkgfix pkf\nmime-handler text/x-pkgfix bin/pkgprog\n' > prog/manifest
cp $FIX/pkgprog prog/files/bin/pkgprog
printf 'read me\n' > prog/files/share/pkgprog/readme
check build-prog "$($PKG build prog)" "pkgprog-1.0.mpk"
check info-needs-prog "$($PKG info pkgprog-1.0.mpk | grep needs | tr '\n' ' ')" "needs libpkgfix.so 1 needs libc.so 1 "

# Alone, the program lacks its dependency.
check dep-missing "$($PKG install pkgprog-1.0.mpk 2>&1)" "pkg: pkgprog: depends on pkgfix, which is not installed"
check dep-missing-status "$($PKG install pkgprog-1.0.mpk 2>/dev/null; echo $?)" "1"
check nothing-installed "$($PKG list)" ""

# Together, in dependency order regardless of the command line order.
check install-both "$($PKG install pkgprog-1.0.mpk pkgfix-1.0.mpk | tr '\n' ' ')" "installed pkgfix 1.0 installed pkgprog 1.0 "
check list-two "$($PKG list | tr '\n' ' ')" "pkgfix 1.0 A library pkgprog 1.0 A program "
check runs "$($P/bin/pkgprog)" "pkgfix 42"
check launcher-table "$(grep -v '^#' $P/share/launcher)" "Prog=$P/bin/pkgprog"
check types-table "$(grep -v '^#' $P/share/mime.types)" "text/x-pkgfix pkf"
check apps-table "$(grep -v '^#' $P/share/mime.apps)" "text/x-pkgfix $P/bin/pkgprog"
check record "$(cut -d' ' -f1 $P/lib/pkg/pkgprog/files | tr '\n' ' ')" "bin/pkgprog share/pkgprog/readme "
check dirs "$(cat $P/lib/pkg/pkgprog/dirs | tr '\n' ' ')" "bin share share/pkgprog "
check again "$($PKG install pkgfix-1.0.mpk)" "pkgfix 1.0 is installed already"
check verify-ok "$($PKG verify; echo $?)" "0"

# A conflict.
mkdir -p conf/files
printf 'name pkgconf\nversion 1.0\nsummary Conflicts\nconflicts pkgprog\n' > conf/manifest
$PKG build conf > /dev/null
check conflict "$($PKG install pkgconf-1.0.mpk 2>&1)" "pkg: pkgconf: conflicts with pkgprog"

# A library upgrade with another ABI number is refused because pkgprog
# was built against ABI 1.
printf 'name pkgfix\nversion 2.0\nsummary A library\nprovides libpkgfix.so 2\n' > lib/manifest
$PKG build lib > /dev/null
check abi-refused "$($PKG install pkgfix-2.0.mpk 2>&1)" "pkg: pkgprog: needs libpkgfix.so ABI 1, pkgfix has 2"
check abi-kept "$($PKG list | head -n 1)" "pkgfix 1.0 A library"

# The same ABI number, but the build without the symbol.
printf 'name pkgfix\nversion 1.1\nsummary A library\nprovides libpkgfix.so 1\n' > lib/manifest
cp $FIX/bad/libpkgfix.so lib/files/lib/libpkgfix.so
$PKG build lib > /dev/null
check symbol-refused "$($PKG install pkgfix-1.1.mpk 2>&1)" "pkg: pkgprog: bin/pkgprog: undefined symbol pkgfix_value"
check symbol-kept "$($P/bin/pkgprog)" "pkgfix 42"

# A wrong ABI number of a system library.
sed 's/^libc.so 1$/libc.so 9/' $ABI > abi.new
cp $ABI abi.old
cp abi.new $ABI
mkdir -p sys/files/bin
printf 'name pkgsys\nversion 1.0\nsummary Built against another system\n' > sys/manifest
cp $FIX/pkgprog sys/files/bin/pkgsys
cp abi.old $ABI
$PKG build sys > /dev/null
cp abi.new $ABI
check system-abi "$($PKG install pkgsys-1.0.mpk 2>&1)" "pkg: pkgsys: needs libc.so ABI 1, the system has 9"
cp abi.old $ABI

# A file owned by another package.
mkdir -p dup/files/bin
printf 'name pkgdup\nversion 1.0\nsummary Duplicate\n' > dup/manifest
cp $FIX/pkgprog dup/files/bin/pkgprog
$PKG build dup > /dev/null
check owned "$($PKG install pkgdup-1.0.mpk 2>&1)" "pkg: pkgdup: bin/pkgprog belongs to pkgprog"

# A member outside files/.
mkdir bad
printf 'name pkgbad\nversion 1.0\nsummary Bad\n' > bad/manifest
printf 'x\n' > bad/evil.txt
(cd bad && tar -czf ../pkgbad-1.0.mpk manifest evil.txt)
sleep 1
check outside "$($PKG install pkgbad-1.0.mpk 2>&1)" "pkg: pkgbad: member evil.txt is outside files/"

# An upgrade replaces the files and removes the ones no longer shipped.
printf 'name pkgprog\nversion 1.1\nsummary A program\ndepends pkgfix >= 1.0\nneeds libpkgfix.so 1\nlauncher Prog bin/pkgprog\n' > prog/manifest
rm prog/files/share/pkgprog/readme
printf 'notes\n' > prog/files/share/pkgprog/notes
$PKG build prog > /dev/null
check upgrade "$($PKG install pkgprog-1.1.mpk)" "upgraded pkgprog 1.1"
check upgrade-old-gone "$(test -e $P/share/pkgprog/readme && echo present || echo absent)" "absent"
check upgrade-new "$(cat $P/share/pkgprog/notes)" "notes"
check upgrade-list "$($PKG list | tail -n 1)" "pkgprog 1.1 A program"
check upgrade-apps "$(grep -v '^#' $P/share/mime.apps)" ""

# Verification after a change.
printf 'more\n' >> $P/share/pkgprog/notes
check verify-changed "$($PKG verify pkgprog)" "pkgprog: share/pkgprog/notes: changed"
check verify-status "$($PKG verify > /dev/null; echo $?)" "1"
rm $P/share/pkgprog/notes
check verify-missing "$($PKG verify pkgprog)" "pkgprog: share/pkgprog/notes: missing"

# Removal: a dependency stays while something depends on it.
check remove-refused "$($PKG remove pkgfix 2>&1)" "pkg: pkgfix: pkgprog depends on it (--force removes it anyway)"
check remove-prog "$($PKG remove pkgprog)" "removed pkgprog 1.1"
check remove-prog-gone "$(test -e $P/bin/pkgprog && echo present || echo absent)" "absent"
check remove-dir-gone "$(test -e $P/bin && echo present || echo absent)" "absent"
check remove-launcher "$(grep -v '^#' $P/share/launcher)" ""
check remove-lib "$($PKG remove pkgfix)" "removed pkgfix 1.0"
check remove-all "$($PKG list)" ""
check remove-missing "$($PKG remove pkgfix 2>&1)" "pkg: pkgfix: not installed"

# The package built on the host.
check host-install "$($PKG install $HELLO)" "installed pkghello 1.0"
check host-runs "$($P/bin/pkghello | head -n 1 | cut -d, -f1)" "hello from user mode"
check host-launcher "$(grep -v '^#' $P/share/launcher)" "Hello=$P/bin/pkghello"
check host-remove "$($PKG remove pkghello)" "removed pkghello 1.0"

echo "pkg: done"
