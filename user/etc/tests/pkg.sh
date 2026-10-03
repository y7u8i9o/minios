# Package installer test, run as: sh /etc/tests/pkg.sh. Every failing check
# prints a line starting with FAIL and the value received. Packages install
# into the root image, and the records are in /var/lib/pkg. The fixtures
# under /etc/tests/pkgfix are a library in two builds and a program calling
# it; pkghello-1.0.mpk was built on the host. The host build of pkg has its
# own test, user/pkg/tests/host.sh.
check() {
    test "$2" = "$3" || echo "FAIL $1: [$2]"
}
PKG=${PKG:-pkg}
FIX=${FIX:-/etc/tests/pkgfix}
ABI=${ABI:-/lib/abi}
HELLO=${HELLO:-/etc/tests/pkghello-1.0.mpk}
DB=/var/lib/pkg
WORK=${WORK:-/pt}
mkdir -p $WORK
cd $WORK
check list-empty "$($PKG list)" ""

# The library package, then the program package that depends on it.
mkdir -p lib/files/usr/lib
printf 'name pkgfix\nversion 1.0\nsummary A library\nprovides libpkgfix.so 1\n' > lib/manifest
cp $FIX/libpkgfix.so lib/files/usr/lib/libpkgfix.so
check build-lib "$($PKG build lib)" "pkgfix-1.0.mpk"
check info-format "$($PKG info pkgfix-1.0.mpk | head -n 1)" "format 2"
check info-needs "$($PKG info pkgfix-1.0.mpk | grep needs)" "needs libc.so 1"
check info-file "$($PKG info pkgfix-1.0.mpk | grep '^file' | cut -d' ' -f2)" "usr/lib/libpkgfix.so"
check info-arch "$($PKG info pkgfix-1.0.mpk | grep '^arch')" "arch $(uname -m)"

# A package for the other machine is refused.
OTHER=aarch64
test "$(uname -m)" = aarch64 && OTHER=x86_64
mkdir -p other/files/usr/share
printf 'data\n' > other/files/usr/share/data
printf 'name pkgother\nversion 1.0\nsummary Another machine\narch %s\n' $OTHER > other/manifest
check build-other "$($PKG build other)" "pkgother-1.0.mpk"
check arch-refused "$($PKG install pkgother-1.0.mpk 2>&1)" "pkg: pkgother: the package is built for $OTHER, and the system is $(uname -m)"

mkdir -p prog/files/usr/bin prog/files/usr/share/pkgprog
printf 'name pkgprog\nversion 1.0\nsummary A program\ndepends pkgfix >= 1.0\nneeds libpkgfix.so 1\nlauncher Prog usr/bin/pkgprog\nmime-type text/x-pkgfix pkf\nmime-handler text/x-pkgfix usr/bin/pkgprog\n' > prog/manifest
cp $FIX/pkgprog prog/files/usr/bin/pkgprog
printf 'read me\n' > prog/files/usr/share/pkgprog/readme
check build-prog "$($PKG build prog)" "pkgprog-1.0.mpk"
check info-needs-prog "$($PKG info pkgprog-1.0.mpk | grep needs | tr '\n' ' ')" "needs libpkgfix.so 1 needs libc.so 1 "

# Alone, the program lacks its dependency.
check dep-missing "$($PKG install pkgprog-1.0.mpk 2>&1)" "pkg: pkgprog: depends on pkgfix, which is not installed"
check dep-missing-status "$($PKG install pkgprog-1.0.mpk 2>/dev/null; echo $?)" "1"
check nothing-installed "$($PKG list)" ""

# Together, in dependency order regardless of the command line order.
check install-both "$($PKG install pkgprog-1.0.mpk pkgfix-1.0.mpk | tr '\n' ' ')" "installed pkgfix 1.0 installed pkgprog 1.0 "
check list-two "$($PKG list | tr '\n' ' ')" "pkgfix 1.0 A library pkgprog 1.0 A program "
check runs "$(/usr/bin/pkgprog)" "pkgfix 42"
check launcher-table "$(grep -v '^#' $DB/launcher)" "Prog=/usr/bin/pkgprog"
check types-table "$(grep -v '^#' $DB/mime.types)" "text/x-pkgfix pkf"
check apps-table "$(grep -v '^#' $DB/mime.apps)" "text/x-pkgfix /usr/bin/pkgprog"
check record "$(cut -d' ' -f6 $DB/pkgprog/files | tr '\n' ' ')" "usr/bin/pkgprog usr/share/pkgprog/readme "
check record-owner "$(cut -d' ' -f1-3 $DB/pkgprog/files | head -n 1)" "0755 0 0"
check dirs "$(grep -c ' usr/share/pkgprog$' $DB/pkgprog/dirs)" "1"
check no-temporary "$(find /usr -name '*.pkgtmp' | wc -l | tr -d ' ')" "0"
check again "$($PKG install pkgfix-1.0.mpk)" "pkgfix 1.0 is installed already"
# The same version replaces an installed package for the other machine, as
# on a data volume moved from x86_64 to aarch64.
sed "s/^arch .*/arch $OTHER/" $DB/pkgfix/manifest > manifest.other
cp manifest.other $DB/pkgfix/manifest
check foreign-replaced "$($PKG install pkgfix-1.0.mpk)" "upgraded pkgfix 1.0"
check foreign-arch "$(grep '^arch' $DB/pkgfix/manifest)" "arch $(uname -m)"
check foreign-runs "$(/usr/bin/pkgprog)" "pkgfix 42"
# A rebuild with other files and the same version replaces the installed
# package, and an identical archive is skipped again.
printf 'read me again\n' > prog/files/usr/share/pkgprog/readme
check build-rebuilt "$($PKG build prog)" "pkgprog-1.0.mpk"
check rebuilt-replaced "$($PKG install pkgprog-1.0.mpk)" "upgraded pkgprog 1.0"
check rebuilt-file "$(cat /usr/share/pkgprog/readme)" "read me again"
check rebuilt-again "$($PKG install pkgprog-1.0.mpk)" "pkgprog 1.0 is installed already"
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
cp $FIX/bad/libpkgfix.so lib/files/usr/lib/libpkgfix.so
$PKG build lib > /dev/null
check symbol-refused "$($PKG install pkgfix-1.1.mpk 2>&1)" "pkg: pkgprog: usr/bin/pkgprog: undefined symbol pkgfix_value"
check symbol-kept "$(/usr/bin/pkgprog)" "pkgfix 42"

# A wrong ABI number of a system library.
sed 's/^libc.so 1$/libc.so 9/' $ABI > abi.new
cp $ABI abi.old
cp abi.new $ABI
mkdir -p sys/files/usr/bin
printf 'name pkgsys\nversion 1.0\nsummary Built against another system\n' > sys/manifest
cp $FIX/pkgprog sys/files/usr/bin/pkgsys
cp abi.old $ABI
$PKG build sys > /dev/null
cp abi.new $ABI
check system-abi "$($PKG install pkgsys-1.0.mpk 2>&1)" "pkg: pkgsys: needs libc.so ABI 1, the system has 9"
cp abi.old $ABI

# A file owned by another package, and a file of the root image that no
# package owns.
mkdir -p dup/files/usr/bin
printf 'name pkgdup\nversion 1.0\nsummary Duplicate\n' > dup/manifest
cp $FIX/pkgprog dup/files/usr/bin/pkgprog
$PKG build dup > /dev/null
check owned "$($PKG install pkgdup-1.0.mpk 2>&1)" "pkg: pkgdup: usr/bin/pkgprog belongs to pkgprog"
mkdir -p stray/files/etc
printf 'name pkgstray\nversion 1.0\nsummary Replaces a file of the image\n' > stray/manifest
printf 'x\n' > stray/files/etc/fstab
$PKG build stray > /dev/null
check unowned "$($PKG install pkgstray-1.0.mpk 2>&1)" "pkg: pkgstray: etc/fstab exists in the filesystem and belongs to no package"

# A member outside files/.
mkdir bad
printf 'format 2\nname pkgbad\nversion 1.0\nsummary Bad\n' > bad/manifest
printf 'x\n' > bad/evil.txt
(cd bad && tar -czf ../pkgbad-1.0.mpk manifest evil.txt)
sleep 1
check outside "$($PKG install pkgbad-1.0.mpk 2>&1)" "pkg: pkgbad: member evil.txt is outside files/"
# An archive without a format line is of format 1.
mkdir old
printf 'name pkgold\nversion 1.0\nsummary Old\n' > old/manifest
mkdir -p old/files/share
printf 'x\n' > old/files/share/x
(cd old && tar -czf ../pkgold-1.0.mpk manifest files)
check format-1 "$($PKG install pkgold-1.0.mpk 2>&1)" "pkg: pkgold: pkgold-1.0.mpk is a package of format 1, and this installer reads format 2; rebuild it"

# An upgrade that cannot write one of its files changes nothing: the
# temporary name of the new file is taken by a directory.
printf 'name pkgprog\nversion 1.1\nsummary A program\ndepends pkgfix >= 1.0\nneeds libpkgfix.so 1\nlauncher Prog usr/bin/pkgprog\n' > prog/manifest
rm prog/files/usr/share/pkgprog/readme
printf 'notes\n' > prog/files/usr/share/pkgprog/notes
$PKG build prog > /dev/null
mkdir /usr/share/pkgprog/notes.pkgtmp
check failed-status "$($PKG install pkgprog-1.1.mpk > /dev/null 2>&1; echo $?)" "1"
rmdir /usr/share/pkgprog/notes.pkgtmp
check failed-version "$($PKG list | tail -n 1)" "pkgprog 1.0 A program"
check failed-file "$(cat /usr/share/pkgprog/readme)" "read me again"
check failed-runs "$(/usr/bin/pkgprog)" "pkgfix 42"
check failed-verify "$($PKG verify; echo $?)" "0"

# The upgrade replaces the files and removes the ones no longer shipped.
check upgrade "$($PKG install pkgprog-1.1.mpk)" "upgraded pkgprog 1.1"
check upgrade-old-gone "$(test -e /usr/share/pkgprog/readme && echo present || echo absent)" "absent"
check upgrade-new "$(cat /usr/share/pkgprog/notes)" "notes"
check upgrade-list "$($PKG list | tail -n 1)" "pkgprog 1.1 A program"
check upgrade-apps "$(grep -v '^#' $DB/mime.apps)" ""

# Verification after a change.
printf 'more\n' >> /usr/share/pkgprog/notes
check verify-changed "$($PKG verify pkgprog)" "pkgprog: usr/share/pkgprog/notes: changed"
check verify-status "$($PKG verify > /dev/null; echo $?)" "1"
rm /usr/share/pkgprog/notes
check verify-missing "$($PKG verify pkgprog)" "pkgprog: usr/share/pkgprog/notes: missing"

# Removal: a dependency stays while something depends on it.
check remove-refused "$($PKG remove pkgfix 2>&1)" "pkg: pkgfix: pkgprog depends on it (--force removes it anyway)"
check remove-prog "$($PKG remove pkgprog)" "removed pkgprog 1.1"
check remove-prog-gone "$(test -e /usr/bin/pkgprog && echo present || echo absent)" "absent"
check remove-dir-gone "$(test -e /usr/share/pkgprog && echo present || echo absent)" "absent"
check remove-launcher "$(grep -v '^#' $DB/launcher)" ""
check remove-lib "$($PKG remove pkgfix)" "removed pkgfix 1.0"
check remove-all "$($PKG list)" ""
check remove-missing "$($PKG remove pkgfix 2>&1)" "pkg: pkgfix: not installed"

# Owners, modes and configuration files. A setuid program installed by
# root keeps its bit, and a configuration file changed by the
# administrator survives an upgrade and the removal.
mkdir -p cfg/files/usr/bin cfg/files/etc
printf '#!/bin/sh\nid -u\n' > cfg/files/usr/bin/pkgsetuid
chmod 4755 cfg/files/usr/bin/pkgsetuid
printf 'value=1\n' > cfg/files/etc/pkgcfg.conf
printf 'name pkgcfg\nversion 1.0\nsummary Configuration\nconfig etc/pkgcfg.conf\n' > cfg/manifest
$PKG build cfg > /dev/null
check cfg-install "$($PKG install pkgcfg-1.0.mpk 2>&1)" "installed pkgcfg 1.0"
check cfg-setuid "$(ls -l /usr/bin/pkgsetuid | cut -c1-10)" "-rwsr-xr-x"
check cfg-perms "$($PKG perms | grep pkgsetuid)" "/usr/bin/pkgsetuid 4755 0 0"
check cfg-verify-ok "$($PKG verify pkgcfg; echo $?)" "0"
printf 'value=local\n' > /etc/pkgcfg.conf
printf 'value=2\n' > cfg/files/etc/pkgcfg.conf
printf 'name pkgcfg\nversion 1.1\nsummary Configuration\nconfig etc/pkgcfg.conf\n' > cfg/manifest
$PKG build cfg > /dev/null
check cfg-upgrade "$($PKG install pkgcfg-1.1.mpk 2>&1 | tr '\n' ' ')" "pkg: pkgcfg: etc/pkgcfg.conf was modified, and the new version is etc/pkgcfg.conf.pkgnew upgraded pkgcfg 1.1 "
check cfg-kept "$(cat /etc/pkgcfg.conf)" "value=local"
check cfg-new "$(cat /etc/pkgcfg.conf.pkgnew)" "value=2"
check cfg-verify "$($PKG verify pkgcfg; echo $?)" "pkgcfg: etc/pkgcfg.conf: modified configuration file
0"
check cfg-remove "$($PKG remove pkgcfg 2>&1 | tr '\n' ' ')" "pkg: pkgcfg: etc/pkgcfg.conf was modified and is saved as etc/pkgcfg.conf.pkgsave removed pkgcfg 1.1 "
check cfg-saved "$(cat /etc/pkgcfg.conf.pkgsave)" "value=local"
rm -f /etc/pkgcfg.conf.pkgsave /etc/pkgcfg.conf.pkgnew

# The package built on the host.
check host-install "$($PKG install $HELLO)" "installed pkghello 1.0"
check host-runs "$(/usr/bin/pkghello | head -n 1 | cut -d, -f1)" "hello from user mode"
check host-launcher "$(grep -v '^#' $DB/launcher)" "Hello=/usr/bin/pkghello"
check host-owner "$(cut -d' ' -f1-3 $DB/pkghello/files)" "0755 0 0"
check host-remove "$($PKG remove pkghello)" "removed pkghello 1.0"
check all-removed "$($PKG list)" ""

echo "pkg: done"
