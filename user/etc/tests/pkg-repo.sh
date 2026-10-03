# This is the repository test, run as sh /etc/tests/pkg-repo.sh. The boot
# harness sets NETPEER_PORT to the port of the HTTP server of the case's
# peer on the host, reached as 10.0.2.2 (tests/cases/pkg_repo/peer
# describes the repositories there). Every failing check prints a line starting with
# FAIL and the value received. Packages install into the root image,
# since the test boots without a data volume.
check() {
    test "$2" = "$3" || echo "FAIL $1: [$2]"
}
# contains checks that the value contains the text.
contains() {
    case "$2" in
        *"$3"*) ;;
        *) echo "FAIL $1: [$2]" ;;
    esac
}
URL=http://10.0.2.2:$NETPEER_PORT
HOST=10.0.2.2:$NETPEER_PORT
# The packages of this test. The base system of the image is installed as
# packages as well.
mine() {
    pkg list | grep '^repo'
}
conf() {
    printf 'repo main %s\ntimeout %s\n' "$1" "${2:-20}" > /etc/pkg.conf
}

# The SHA-2 and Ed25519 vectors must pass on minios.
check crypto "$(/bin/cryptotest > /tmp/crypto.log; echo $?)" "0"
cat /tmp/crypto.log
check dhcp "$(/bin/dhcpc -1 -t 30 > /dev/null 2>&1; echo $?)" "0"

# An index is fetched, verified and searched.
conf $URL/v1
check search-before "$(pkg search 2>&1; echo $?)" "pkg: main: no index; run pkg update
1"
check update "$(pkg update)" "main: 3 packages from $URL/v1"
check cache "$(ls /var/lib/pkg/_repos/main | tr '\n' ' ')" "index index.sig url "
check search "$(pkg search | cut -d' ' -f1-3 | tr '\n' ' ')" "repohello 1.0 main repolib 1.0 main repoprog 1.0 main "
check search-pattern "$(pkg search Hello)" "repohello 1.0 main Hello from a repository"
check search-summary "$(pkg search library | cut -d' ' -f1)" "repolib"
check search-none "$(pkg search nothing)" ""

# An index entry for another machine is not listed.
conf $URL/otherarch
check otherarch-update "$(pkg update 2>&1)" "main: 2 packages from $URL/otherarch"
check otherarch-search "$(pkg search | cut -d' ' -f1 | tr '\n' ' ')" "repolib repoprog "
conf $URL/v1
pkg update > /dev/null
check list-empty "$(mine)" ""

# repoprog is installed by name. It depends on repohello and needs
# libpkgfix.so, which repolib provides, and both come from the index.
out="$(pkg install repoprog)"
check install-fetched "$(echo "$out" | grep '^fetched' | tr '\n' ' ')" "fetched repoprog 1.0 from main fetched repohello 1.0 from main fetched repolib 1.0 from main "
check install-order "$(echo "$out" | grep -v '^fetched' | tr '\n' ' ')" "installed repohello 1.0 installed repolib 1.0 installed repoprog 1.0 "
check runs "$(/usr/bin/repoprog)" "pkgfix 42"
check news "$(cat /usr/share/repohello/NEWS)" "repohello 1.0"
check no-temporary "$(ls /tmp | grep '^pkg-')" ""
check again "$(pkg install repohello)" "repohello 1.0 is installed already"
check up-to-date "$(pkg upgrade)" "the installed packages are up to date"
check unknown-name "$(pkg install nosuchpkg 2>&1; echo $?)" "pkg: nosuchpkg: no repository offers this package
1"

# A second version appears; the index names the URL it came from.
conf $URL/v2
check stale "$(pkg search hello 2>&1)" "pkg: main: the index was fetched from $URL/v1; run pkg update"
check update-v2 "$(pkg update)" "main: 4 packages from $URL/v2"
check search-v2 "$(pkg search Hello | cut -d' ' -f1-3 | tr '\n' ' ')" "repohello 1.0 main repohello 1.1 main "
check upgrade "$(pkg upgrade | tr '\n' ' ')" "fetched repohello 1.1 from main upgraded repohello 1.1 "
check upgrade-news "$(cat /usr/share/repohello/NEWS)" "repohello 1.1"
check upgrade-done "$(pkg upgrade)" "the installed packages are up to date"
check by-version "$(pkg install repohello-1.1)" "repohello 1.1 is installed already"
check no-version "$(pkg install repohello-2.0 2>&1; echo $?)" "pkg: repohello: no repository offers version 2.0
1"
check verify "$(pkg verify repohello repolib repoprog; echo $?)" "0"

# A local archive is checked against the signed index. The archive of the
# repository matches, and one built here with the same name and version
# does not.
check http-get "$(http -o /tmp/rh.mpk $URL/v2/repohello-1.1.mpk; echo $?)" "0"
check local-matches "$(pkg check /tmp/rh.mpk | tail -n 1)" "repohello 1.1 matches the index of main"
mkdir -p /tmp/rb/files/usr/bin
printf 'name repohello\nversion 1.1\nsummary Not the one of the repository\n' > /tmp/rb/manifest
cp /bin/hello /tmp/rb/files/usr/bin/repohello
(cd /tmp && pkg build rb > /dev/null)
out="$(pkg check /tmp/repohello-1.1.mpk 2>&1; echo $?)"
contains local-differs "$out" "pkg: repohello: /tmp/repohello-1.1.mpk"
contains local-differs-index "$out" "the index of main"
check local-differs-status "$(echo "$out" | tail -n 1)" "1"
check remove "$(pkg remove repoprog repolib repohello | tr '\n' ' ')" "removed repoprog 1.0 removed repolib 1.0 removed repohello 1.1 "

# An archive changed after signing is refused by its digest, one longer
# than the index says by its size; nothing is installed.
conf $URL/badarchive
check update-badarchive "$(pkg update)" "main: 4 packages from $URL/badarchive"
check tampered "$(pkg install repohello 2>&1; echo $?)" "pkg: repohello: the SHA-256 digest of repohello-1.1.mpk differs from the index of main
1"
check tampered-list "$(mine)" ""
check tampered-temporary "$(ls /tmp | grep '^pkg-')" ""
conf $URL/badsize
check update-badsize "$(pkg update)" "main: 4 packages from $URL/badsize"
out="$(pkg install repohello 2>&1; echo $?)"
contains oversized "$out" "pkg: repohello: $HOST announces"
check oversized-status "$(echo "$out" | tail -n 1)" "1"
check oversized-list "$(mine)" ""

# An index changed after signing and an index signed by an unknown key
# are refused, and the last verified index stays in place.
conf $URL/badindex
check bad-signature "$(pkg update 2>&1; echo $?)" "pkg: main: the index signature does not verify with /etc/pkg/keys/build.pub
1"
check bad-signature-retained "$(cat /var/lib/pkg/_repos/main/url)" "$URL/badsize"
check bad-signature-files "$(ls /var/lib/pkg/_repos/main | tr '\n' ' ')" "index index.sig url "
conf $URL/otherkey
out="$(pkg update 2>&1; echo $?)"
contains unknown-key "$out" "pkg: main: the index is signed by key "
contains unknown-key-dir "$out" ", which is not in /etc/pkg/keys"
check unknown-key-status "$(echo "$out" | tail -n 1)" "1"
check unknown-key-retained "$(cat /var/lib/pkg/_repos/main/url)" "$URL/badsize"
check unknown-key-install "$(pkg install repohello 2>&1 | head -n 1)" "pkg: main: the index was fetched from $URL/badsize; run pkg update"

# The transfers fail with a refused connection, a missing index, a short
# body and a server that stops answering. A second repository is updated
# although the first one fails.
printf 'repo down http://10.0.2.2:1/v1\nrepo main %s\ntimeout 20\n' "$URL/v1" > /etc/pkg.conf
check unreachable "$(pkg update 2>&1; echo $?)" "pkg: down: connect to 10.0.2.2:1: Connection refused
main: 3 packages from $URL/v1
1"
conf $URL/nowhere
check not-found "$(pkg update 2>&1; echo $?)" "pkg: main: $URL/nowhere/index: the server returned status 404
1"
conf $URL/short/v1
out="$(pkg update 2>&1; echo $?)"
contains short "$out" "pkg: main: $HOST closed the connection after "
check short-status "$(echo "$out" | tail -n 1)" "1"
conf $URL/stall/v1 2
check stall "$(pkg update 2>&1; echo $?)" "pkg: main: $HOST sent nothing for 2 seconds
1"
out="$(http -o /tmp/short $URL/short/v1/index 2>&1; echo $?)"
contains short-http "$out" "http: $HOST closed the connection after "
check short-http-status "$(echo "$out" | tail -n 1)" "1"
check short-http-removed "$(test -e /tmp/short && echo present || echo absent)" "absent"
check cache-intact "$(ls /var/lib/pkg/_repos/main | tr '\n' ' ')" "index index.sig url "
echo "pkg-repo: done"
