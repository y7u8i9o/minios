# Persistent storage test, run as: sh /etc/tests/persist.sh. The runner
# attaches an empty mfs image as vdb. Every failing check prints a line
# starting with FAIL and the value received; the post script checks the
# image after QEMU exits.
check() {
    test "$2" = "$3" || echo "FAIL $1: [$2]"
}
mkdir /mnt
mkdir /mnt/data
mkdir /mnt/none
printf 'vdb /mnt/data mfs seed=/usr/share/skel/home\n# a comment\nvdz /mnt/none mfs nofail\n' > /tmp/fstab
fsinit -f /tmp/fstab -v > /tmp/out.txt; check fsinit-status "$?" "0"
grep 'seeded /mnt/data from /usr/share/skel/home' /tmp/out.txt > /dev/null || echo "FAIL fsinit-seed-message"
grep '/mnt/none: .*skipped' /tmp/out.txt > /dev/null || echo "FAIL fsinit-nofail-message"
check seeded-files "$(ls /mnt/data/desktop | sort | tr "\n" " ")" "Clock.app Files.app Terminal.app readme.txt "
check seeded-dotfile "$(head -n 1 /mnt/data/.shrc | cut -c1-10)" "# Personal"
check mounted "$(mount | grep -c '^/mnt/data type mfs')" "1"
echo persisted > /mnt/data/persisted.txt
rm /mnt/data/desktop/readme.txt
mount -u /mnt/data || echo "FAIL umount"
check unmounted "$(mount | grep -c '^/mnt/data ')" "0"
fsinit -f /tmp/fstab; check fsinit-again "$?" "0"
check file-kept "$(cat /mnt/data/persisted.txt)" "persisted"
ls /mnt/data/desktop/readme.txt > /dev/null 2>&1 && echo "FAIL reseeded-a-used-volume"
fsinit -f /tmp/fstab -v > /tmp/out.txt; check fsinit-idempotent "$?" "0"
grep 'is mounted already' /tmp/out.txt > /dev/null || echo "FAIL fsinit-already-mounted-message"
printf 'vdz /mnt/none mfs\n' > /tmp/fstab2
fsinit -f /tmp/fstab2 2> /dev/null; check fsinit-required-failure "$?" "1"
printf 'vdb /mnt/data\n' > /tmp/fstab3
fsinit -f /tmp/fstab3 2> /dev/null; check fsinit-invalid-entry "$?" "1"
fsinit -f /tmp/nosuch 2> /dev/null; check fsinit-missing-table "$?" "1"
mount -u /mnt/data || echo "FAIL umount-final"
sync
rm /tmp/fstab /tmp/fstab2 /tmp/fstab3 /tmp/out.txt
rmdir /mnt/data /mnt/none /mnt
echo "persist: done"
