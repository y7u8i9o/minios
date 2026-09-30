# Symbolic link test, run as: sh /etc/tests/symlink.sh (case symlink). The
# system calls are checked by /bin/symlinktest, which leaves the mfs volume
# of vdb mounted on /mnt; this script checks the utilities and a tar round
# trip, then unmounts /mnt and syncs the root so that the post script can
# check both images with fsck. Every failing check prints a line starting
# with FAIL and the value received.
check() {
    test "$2" = "$3" || echo "FAIL $1: [$2]"
}
/bin/symlinktest || echo "FAIL symlinktest"
mkdir /su
cd /su
mkdir d
printf 'hi\n' > d/f
ln -s d/f lf; check ln-s "$?" "0"
check readlink "$(readlink lf)" "d/f"
check readlink-n "$(readlink -n lf | wc -c | tr -d ' ')" "3"
check read-through "$(cat lf)" "hi"
readlink d/f > /dev/null 2>&1; check readlink-not-a-link "$?" "1"
check readlink-f "$(readlink -f lf)" "/su/d/f"
ln -s d ld
ln -s lf lf 2> /dev/null; check ln-existing "$?" "1"
ln -s lf lx
ln -sf ld lx; check ln-sf "$(readlink lx)" "ld"
mkdir in
ln -s /su/d/f in; check ln-into-directory "$(readlink in/f)" "/su/d/f"
ln d/f hard; check ln-hard "$(stat hard | grep -c 'Links: 2')" "1"
ln /su/d/f /mnt/cross 2> /dev/null; check ln-hard-exdev "$?" "1"

check ls-l-type "$(ls -l lf | cut -c1)" "l"
check ls-l-target "$(ls -l | grep -c ' lf -> d/f$')" "1"
check ls-l-operand "$(ls -l ld | grep -c ' ld -> d$')" "1"
check ls-follows-operand "$(ls ld)" "f"
check ls-F "$(ls -F | grep -c '^lf@$')" "1"
check ls-F-dir-link "$(ls -F | grep -c '^ld@$')" "1"
check stat-link "$(stat lf | grep -c 'symbolic link')" "1"
check stat-target "$(stat lf | grep -c 'File: lf -> d/f')" "1"
check stat-L "$(stat -L lf | grep -c 'regular file')" "1"
check find-type-l "$(find /su -type l | sort | tr '\n' ' ')" "/su/in/f /su/ld /su/lf /su/lx "
check find-type-f "$(find /su -type f | sort | tr '\n' ' ')" "/su/d/f /su/hard "
check find-no-descent "$(find /su/ld | tr '\n' ' ')" "/su/ld "
check tree-link "$(tree /su | grep -c 'lf -> d/f')" "1"

cp lf copy; check cp-follows "$(find /su/copy -type f)" "/su/copy"
check cp-content "$(cat copy)" "hi"
cp -P lf copyl; check cp-P "$(readlink copyl)" "d/f"
mkdir src
ln -s ../d/f src/l
printf 'plain\n' > src/p
cp -R src dst; check cp-R-link "$(readlink dst/l)" "../d/f"
check cp-R-file "$(cat dst/p)" "plain"
check cp-R-resolves "$(cat dst/l)" "hi"
cp -R -L src dstl; check cp-RL "$(find dstl -type f | sort | tr '\n' ' ')" "dstl/l dstl/p "
cp -R ld ldcopy; check cp-R-top-link "$(readlink ldcopy)" "d"

ln -s loopb loopa
ln -s loopa loopb
cat loopa 2> /tmp/su-err; check loop-status "$?" "1"
check loop-message "$(grep -c 'Too many levels of symbolic links' /tmp/su-err)" "1"
du -s /su > /dev/null; check du-links "$?" "0"

mkdir t
ln -s ../d/f t/rel
ln -s /su/d/f t/abs
printf 'data\n' > t/file
tar -cf t.tar t; check tar-create "$?" "0"
check tar-list "$(tar -tf t.tar | sort | tr '\n' ' ')" "t t/abs t/file t/rel "
mkdir out
tar -C out -xf t.tar; check tar-extract "$?" "0"
check tar-rel "$(readlink out/t/rel)" "../d/f"
check tar-abs "$(readlink out/t/abs)" "/su/d/f"
check tar-abs-content "$(cat out/t/abs)" "hi"
check tar-file "$(cat out/t/file)" "data"
check tar-type "$(find out -type l | sort | tr '\n' ' ')" "out/t/abs out/t/rel "

rm lf; check rm-link "$(find /su/lf /su/d/f 2> /dev/null | tr '\n' ' ')" "/su/d/f "
rm ld; check rm-dir-link "$(ls /su/d)" "f"
rm loopa loopb lx; check rm-loops "$?" "0"
cd /
mount -u /mnt || echo "FAIL umount /mnt"
sync
echo "symlink: done"
