# tar test, run as: sh /etc/tests/tar.sh. Every failing check prints a line
# starting with FAIL and the value received. tar -z exits before its gzip
# child has finished writing, so the script sleeps before it reads the
# compressed archive.
check() {
    test "$2" = "$3" || echo "FAIL $1: [$2]"
}
mkdir /tt
cd /tt
mkdir src
mkdir src/dir
printf 'one\n' > src/one.txt
printf 'two\n' > src/dir/two.txt
printf 'a name that is longer than one hundred characters needs the ustar prefix field to be stored in the archive\n' > src/dir/a-name-that-is-longer-than-one-hundred-characters-needs-the-ustar-prefix-field-to-be-stored-in-the-archive.txt
tar -cf all.tar src || echo "FAIL tar-create"
check tar-list "$(tar -tf all.tar | sort | tr '\n' ' ')" "src src/dir src/dir/a-name-that-is-longer-than-one-hundred-characters-needs-the-ustar-prefix-field-to-be-stored-in-the-archive.txt src/dir/two.txt src/one.txt "
check tar-magic "$(head -c 263 all.tar | tail -c 6 | head -c 5)" "ustar"
size=$(cat all.tar | wc -c | tr -d ' ')
check tar-size-multiple "$((size % 512))" "0"
mkdir out
tar -C out -xf all.tar || echo "FAIL tar-extract"
check tar-extracted "$(cat out/src/one.txt out/src/dir/two.txt | tr '\n' ' ')" "one two "
check tar-extracted-long "$(cat out/src/dir/a-name-that-is-longer-than-one-hundred-characters-needs-the-ustar-prefix-field-to-be-stored-in-the-archive.txt | head -c 6)" "a name"
check tar-verbose-create "$(tar -cvf v.tar src/one.txt)" "src/one.txt"
check tar-verbose-extract "$(cd out && tar -xvf ../v.tar)" "src/one.txt"
check tar-single-member "$(tar -tf v.tar)" "src/one.txt"
mkdir out2
tar -C out2 -xf all.tar src/dir/two.txt || echo "FAIL tar-extract-member"
check tar-extract-only-member "$(cd out2 && find . -type f | sort | tr '\n' ' ')" "./src/dir/two.txt "
tar -czf all.tgz src || echo "FAIL tar-create-gzip"
sleep 1
gzip -t all.tgz || echo "FAIL tar-gzip-format"
mkdir out3
tar -C out3 -xzf all.tgz || echo "FAIL tar-extract-gzip"
check tar-gzip-roundtrip "$(cat out3/src/dir/two.txt)" "two"
check tar-list-gzip "$(tar -tzf all.tgz | sort | head -n 1)" "src"
sleep 1
mkdir out4
tar -C out4 -xf all.tar
printf 'out4/src/one.txt: src/one.txt\n\t@echo rebuild\n' > kept.mk
make -q -f kept.mk; check tar-mtime-kept "$?" "1"
mkdir out5
tar -C out5 -xmf all.tar
printf 'out5/src/one.txt: src/one.txt\n\t@echo rebuild\n' > now.mk
make -q -f now.mk; check tar-mtime-now "$?" "0"
check tar-host-list "$(tar -tf /etc/tests/fixture.tar | sort | tr '\n' ' ')" "host/ host/note.txt host/sub/ host/sub/inner.txt "
mkdir out6
tar -C out6 -xf /etc/tests/fixture.tar || echo "FAIL tar-host-extract"
check tar-host-content "$(cat out6/host/note.txt out6/host/sub/inner.txt | tr '\n' ' ')" "from the host nested "
tar -tf nosuch.tar 2> /dev/null; test $? != 0 || echo "FAIL tar-missing"
tar 2> /dev/null; test $? != 0 || echo "FAIL tar-usage"
check tar-stdout "$(tar -c src/one.txt | tar -t)" "src/one.txt"
cd /
find /tt -type f | xargs rm
find /tt -type d | sort -r | xargs rmdir
echo "tar: done"
