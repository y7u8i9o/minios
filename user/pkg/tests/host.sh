# The test of the host build of pkg, run by make check-pkg as
# PKG=build/host/pkg WORK=DIR sh user/pkg/tests/host.sh. It installs
# packages without ELF files into an installation root below WORK and
# checks the records, the owners that pkg perms prints, configuration
# files, a failed transaction and the refusals of format 1 and of a file
# that belongs to no package. Every failing check prints a line starting
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
check config-kept "$(cat $R/etc/one.conf)" "setting=local"
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

if test $status = 0; then
    echo "pkg host test: done"
fi
exit $status
