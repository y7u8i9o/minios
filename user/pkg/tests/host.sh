# The test of the host build of pkg, run by make check-pkg as
# PKG=build/host/pkg WORK=DIR sh user/pkg/tests/host.sh. It installs
# packages without ELF files into an installation root below WORK and
# checks the records, the owners that pkg perms prints, configuration
# files, a failed transaction and the refusals of format 1 and of a file
# that belongs to no package. With PKGSIGN and MKREPO it also installs from
# a signed file repository into a second root. Every failing check prints a line starting
# with FAIL, and the exit status is 1 when any check failed.
PKG=${PKG:?}
WORK=${WORK:?}
status=0
check() {
    if test "$2" != "$3"; then
        echo "FAIL $1: [$2]"
        status=1
    fi
}
rm -rf "$WORK"
mkdir -p "$WORK/root"
cd "$WORK" || exit 1
R="$WORK/root"
P="$PKG --root $R --arch x86_64"

# A package with a setuid program, a data file and a configuration file.
mkdir -p one/files/usr/bin one/files/usr/share/one one/files/etc
printf 'name one\nversion 1.0\nsummary The first package\nconfig etc/one.conf\nlauncher One usr/bin/one\n' > one/manifest
printf '#!/bin/sh\necho one\n' > one/files/usr/bin/one
chmod 4755 one/files/usr/bin/one
printf 'data 1\n' > one/files/usr/share/one/data
printf 'setting=1\n' > one/files/etc/one.conf
chmod 644 one/files/usr/share/one/data one/files/etc/one.conf
check build "$($P build one one-1.0.mpk)" "one-1.0.mpk"
check format "$($P info one-1.0.mpk | head -n 1)" "format 2"
check install "$($P install one-1.0.mpk)" "installed one 1.0"
check list "$($P list)" "one 1.0 The first package"
check file "$(cat $R/usr/share/one/data)" "data 1"
check no-temporary "$(find $R -name '*.pkgtmp' | wc -l | tr -d ' ')" "0"
check record "$(cut -d' ' -f1-4,6 $R/var/lib/pkg/one/files | tr '\n' ',')" \
    "0644 0 0 10 etc/one.conf,4755 0 0 19 usr/bin/one,0644 0 0 7 usr/share/one/data,"
check perms "$($P perms | grep -v '^/etc \|^/usr \|^/usr/bin \|^/usr/share ' | tr '\n' ',')" \
    "/usr/share/one 0755 0 0,/etc/one.conf 0644 0 0,/usr/bin/one 4755 0 0,/usr/share/one/data 0644 0 0,"
check setuid-dropped "$(ls -l $R/usr/bin/one | cut -c1-10)" "-rwxr-xr-x"
check launcher "$(grep -v '^#' $R/var/lib/pkg/launcher)" "One=/usr/bin/one"
check verify "$($P verify; echo $?)" "0"

# A file of the root that no package owns is refused.
mkdir -p two/files/usr/share
printf 'name two\nversion 1.0\nsummary The second package\n' > two/manifest
printf 'mine\n' > two/files/usr/share/stray
$P build two two-1.0.mpk > /dev/null
printf 'stray\n' > $R/usr/share/stray
check unowned "$($P install two-1.0.mpk 2>&1)" "pkg: two: usr/share/stray exists in the filesystem and belongs to no package"
rm $R/usr/share/stray
check unowned-removed "$($P install two-1.0.mpk)" "installed two 1.0"

# An upgrade replaces an unmodified configuration file.
printf 'setting=2\n' > one/files/etc/one.conf
printf 'name one\nversion 1.1\nsummary The first package\nconfig etc/one.conf\n' > one/manifest
$P build one one-1.1.mpk > /dev/null
check upgrade "$($P install one-1.1.mpk)" "upgraded one 1.1"
check config-replaced "$(cat $R/etc/one.conf)" "setting=2"

# A modified configuration file is left in place, the new version is
# written beside it, and verify reports it without failing.
printf 'setting=local\n' > $R/etc/one.conf
printf 'setting=3\n' > one/files/etc/one.conf
printf 'name one\nversion 1.2\nsummary The first package\nconfig etc/one.conf\n' > one/manifest
$P build one one-1.2.mpk > /dev/null
check upgrade-modified "$($P install one-1.2.mpk 2>&1 | tr '\n' ',')" \
    "pkg: one: etc/one.conf was modified, and the new version is etc/one.conf.pkgnew,upgraded one 1.2,"
check config-retained "$(cat $R/etc/one.conf)" "setting=local"
check config-new "$(cat $R/etc/one.conf.pkgnew)" "setting=3"
check verify-config "$($P verify one; echo $?)" "one: etc/one.conf: modified configuration file
0"

# A transaction that cannot write a file changes nothing.
printf 'name one\nversion 1.3\nsummary The first package\nconfig etc/one.conf\n' > one/manifest
printf 'data 3\n' > one/files/usr/share/one/data
printf 'more\n' > one/files/usr/share/one/more
$P build one one-1.3.mpk > /dev/null
chmod 555 $R/usr/share/one
out="$($P install one-1.3.mpk 2>&1; echo $?)"
chmod 755 $R/usr/share/one
check failed-status "$(echo "$out" | tail -n 1)" "1"
check failed-version "$($P list | head -n 1)" "one 1.2 The first package"
check failed-data "$(cat $R/usr/share/one/data)" "data 1"
check failed-no-temporary "$(find $R -name '*.pkgtmp' | wc -l | tr -d ' ')" "0"
check failed-verify "$($P verify one; echo $?)" "one: etc/one.conf: modified configuration file
0"

# Removal saves the modified configuration file and removes the rest.
check remove "$($P remove one 2>&1 | tr '\n' ',')" \
    "pkg: one: etc/one.conf was modified and is saved as etc/one.conf.pkgsave,removed one 1.2,"
check saved "$(cat $R/etc/one.conf.pkgsave)" "setting=local"
check removed-dir "$(test -e $R/usr/share/one && echo present || echo absent)" "absent"
check removed-launcher "$(grep -v '^#' $R/var/lib/pkg/launcher)" ""

# Symbolic links are members of their own. They are recorded with their
# target, verified by it, left out of pkg perms and removed with the
# package.
mkdir -p lnk/files/usr/share/lnk
printf 'name lnk\nversion 1.0\nsummary Links\n' > lnk/manifest
printf 'x\n' > lnk/files/usr/share/lnk/file
ln -s usr/share lnk/files/share
ln -s file lnk/files/usr/share/lnk/alias
$P build lnk lnk-1.0.mpk > /dev/null
check link-install "$($P install lnk-1.0.mpk)" "installed lnk 1.0"
check link-target "$(readlink $R/share)" "usr/share"
check link-through "$(cat $R/share/lnk/alias)" "x"
check link-record "$(grep ' share$' $R/var/lib/pkg/lnk/files | cut -d' ' -f1,4)" "120777 9"
check link-perms "$($P perms | grep -c 'alias\|/share ')" "0"
check link-verify "$($P verify lnk; echo $?)" "0"
rm $R/share && ln -s elsewhere $R/share
check link-changed "$($P verify lnk)" "lnk: share: changed"
check link-remove "$($P remove lnk)" "removed lnk 1.0"
check link-gone "$(test -L $R/share && echo present || echo absent)" "absent"

# An archive of format 1 is refused.
mkdir -p old/files/share
printf 'name old\nversion 1.0\nsummary An archive of format 1\n' > old/manifest
printf 'x\n' > old/files/share/x
(cd old && COPYFILE_DISABLE=1 tar --format ustar -cf - manifest files | gzip > ../old-1.0.mpk)
check format-1 "$($P install old-1.0.mpk 2>&1)" \
    "pkg: old: old-1.0.mpk is a package of format 1, and this installer reads format 2; rebuild it"

# A signed repository in the file system serves a second, empty root. The
# trusted keys and the configuration are outside that root.
if test -n "$PKGSIGN" && test -n "$MKREPO"; then
    mkdir -p fr/liba/files/usr/share/liba fr/appb/files/usr/share/appb
    printf 'name liba\nversion 1.0\nsummary A library package\n' > fr/liba/manifest
    printf 'a\n' > fr/liba/files/usr/share/liba/data
    printf 'name appb\nversion 2.0\nsummary An application package\ndepends liba\n' > fr/appb/manifest
    printf 'b\n' > fr/appb/files/usr/share/appb/data
    $P build fr/liba fr/liba-1.0.mpk > /dev/null
    $P build fr/appb fr/appb-2.0.mpk > /dev/null
    $PKGSIGN keygen fr/key > /dev/null
    $PKGSIGN keygen fr/other > /dev/null
    mkdir -p fr/keys
    $PKGSIGN public fr/key minios > fr/keys/test.pub
    sh "$MKREPO" "$PKGSIGN" fr/key "$WORK/fr/repo" fr/liba-1.0.mpk fr/appb-2.0.mpk > /dev/null
    printf 'repo local file://%s/fr/repo\n' "$WORK" > fr/pkg.conf
    R2="$WORK/root2"
    mkdir -p "$R2"
    P2="$PKG --root $R2 --arch x86_64 --config $WORK/fr/pkg.conf --keys $WORK/fr/keys"
    check file-update "$($P2 update)" "local: 2 packages from file://$WORK/fr/repo"
    check file-cache "$(cmp $R2/var/lib/pkg/_repos/local/index fr/repo/index && echo same)" "same"
    check file-search "$($P2 search application)" "appb 2.0 local An application package"
    check file-install "$($P2 install appb | tr '\n' ',')" \
        "found appb 2.0 from local,found liba 1.0 from local,installed liba 1.0,installed appb 2.0,"
    check file-list "$($P2 list | tr '\n' ',')" "appb 2.0 An application package,liba 1.0 A library package,"
    check file-data "$(cat $R2/usr/share/appb/data)" "b"
    check file-verify "$($P2 verify; echo $?)" "0"
    check file-retained "$(ls fr/repo | tr '\n' ' ')" "appb-2.0.mpk index index.sig liba-1.0.mpk "
    check file-tmp "$(ls -d /tmp/pkg-* 2>/dev/null | wc -l | tr -d ' ')" "0"

    # --verbose, also between other options, prints a line before each step.
    R4="$WORK/root4"
    mkdir -p "$R4"
    P4="$PKG --root $R4 --verbose --arch x86_64 --config $WORK/fr/pkg.conf --keys $WORK/fr/keys"
    check verbose-update "$($P4 update | tr '\n' ',')" \
        "reading the index of local (1 of 1),local: 2 packages from file://$WORK/fr/repo,"
    check verbose-install "$($P4 install appb | tr '\n' ',')" \
        "verifying appb 2.0 (1 of 2),found appb 2.0 from local,verifying liba 1.0 (2 of 2),found liba 1.0 from local,checking 2 packages,unpacking liba 1.0 (1 of 2),unpacking appb 2.0 (2 of 2),installing the files of 2 packages,installed liba 1.0,installed appb 2.0,"

    # An archive that changed after the signing is refused and not removed.
    R3="$WORK/root3"
    mkdir -p "$R3"
    P3="$PKG --root $R3 --arch x86_64 --config $WORK/fr/pkg.conf --keys $WORK/fr/keys"
    $P3 update > /dev/null
    size=$(wc -c < fr/repo/liba-1.0.mpk | tr -d ' ')
    printf 'x' >> fr/repo/liba-1.0.mpk
    check file-tampered "$($P3 install liba 2>&1)" \
        "pkg: liba: liba-1.0.mpk is $((size + 1)) bytes, the index of local gives $size"
    # Same size, other content, which only the digest can tell.
    head -c "$((size - 1))" fr/repo/liba-1.0.mpk > fr/swap
    printf 'Z' >> fr/swap
    cp fr/swap fr/repo/liba-1.0.mpk
    check file-digest "$($P3 install liba 2>&1)" "pkg: liba: the SHA-256 digest of liba-1.0.mpk differs from the index of local"
    check file-tampered-retained "$(test -f fr/repo/liba-1.0.mpk && echo present)" "present"
    check file-tampered-none "$($P3 list | wc -l | tr -d ' ')" "0"

    # An older index, an expired index and an index of an origin that the
    # key may not sign are refused.
    REPO_SEQUENCE=1 sh "$MKREPO" "$PKGSIGN" fr/key "$WORK/fr/old" fr/appb-2.0.mpk > /dev/null
    printf 'repo local file://%s/fr/old\n' "$WORK" > fr/old.conf
    check file-old "$($PKG --root $R3 --arch x86_64 --config $WORK/fr/old.conf --keys $WORK/fr/keys update 2>&1)" \
        "pkg: local: the index has sequence 1, lower than $(sed -n 's/^sequence //p' $R3/var/lib/pkg/_repos/local/index) of the stored index"
    REPO_EXPIRES=1000000000 sh "$MKREPO" "$PKGSIGN" fr/key "$WORK/fr/expired" fr/appb-2.0.mpk > /dev/null
    printf 'repo local file://%s/fr/expired\n' "$WORK" > fr/expired.conf
    check file-expired "$($PKG --root $R3 --arch x86_64 --config $WORK/fr/expired.conf --keys $WORK/fr/keys update 2>&1)" \
        "pkg: local: the index expired at 2001-09-09 01:46:40 UTC"
    REPO_ORIGIN=elsewhere sh "$MKREPO" "$PKGSIGN" fr/key "$WORK/fr/elsewhere" fr/appb-2.0.mpk > /dev/null
    printf 'repo local file://%s/fr/elsewhere\n' "$WORK" > fr/elsewhere.conf
    check file-origin "$($PKG --root $R3 --arch x86_64 --config $WORK/fr/elsewhere.conf --keys $WORK/fr/keys update 2>&1)" \
        "pkg: local: the index is of the origin elsewhere, which the key $WORK/fr/keys/test.pub may not sign"

    # An index signed by another key is refused.
    $PKGSIGN sign fr/other fr/repo/index
    check file-wrong-key "$($P3 update 2>&1 | grep -c '^pkg: local: the index is signed by key .* which is not in ')" "1"
    check file-tmp-end "$(ls -d /tmp/pkg-* 2>/dev/null | wc -l | tr -d ' ')" "0"
fi

if test $status = 0; then
    echo "pkg host test: done"
fi
exit $status
