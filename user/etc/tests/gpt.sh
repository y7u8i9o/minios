# The GPT test, run as sh /etc/tests/gpt.sh by tests/cases/gpt_boot. Its
# disk contains an EFI system partition, a swap partition and the root
# partition, which the kernel found by its type. Every failing check prints
# a line starting with FAIL and the value received.
check() {
    test "$2" = "$3" || echo "FAIL $1: [$2]"
}
G=c0ffee00-0000-4000-8000-00000000000
check partitions "$(cut -d' ' -f1-3 /dev/partitions | tr '\n' ' ')" "vda1 vda ${G}1 vda2 vda ${G}2 vda3 vda ${G}3 "
case "$(uname -m)" in
    x86_64) ROOT=4f68bce3-e8cd-4db1-96e7-fbcaf984b709 ;;
    *) ROOT=b921b045-1df0-41c3-af44-4c6f280d3fae ;;
esac
check root-type "$(grep '^vda3 ' /dev/partitions | cut -d' ' -f4)" "$ROOT"
check esp-type "$(grep '^vda1 ' /dev/partitions | cut -d' ' -f4)" "c12a7328-f81f-11d2-ba4b-00a0c93ec93b"
check esp-size "$(grep '^vda1 ' /dev/partitions | cut -d' ' -f5)" "16777216"

# fsinit mounts a partition named by its GUID, in capitals as well, and
# two filesystems in memory, one of 1 MiB.
mkdir -p /boot /mnt/small
printf 'PARTUUID=C0FFEE00-0000-4000-8000-000000000001 /boot fat\ntmpfs /tmp tmpfs mode=1777\ntmpfs /mnt/small tmpfs size=1\n' > /gpt.fstab
check fsinit "$(fsinit -f /gpt.fstab > /dev/null; echo $?)" "0"
check esp-file "$(cat /boot/hello.txt)" "hello from the esp"
check mounted "$(mount | grep -c '^/tmp type tmpfs$\|^/boot type fat$\|^/mnt/small type tmpfs$')" "3"
check tmp-mode "$(ls -ld /tmp | cut -c1-10)" "drwxrwxrwt"
check tmp-empty "$(ls -a /tmp | tr '\n' ' ')" ". .. "

# Files, directories, links and renames in memory.
echo one > /tmp/f
check write "$(cat /tmp/f)" "one"
mkdir /tmp/d
mv /tmp/f /tmp/d/g
ln -s d/g /tmp/l
check link "$(cat /tmp/l)" "one"
check list "$(ls /tmp | tr '\n' ' ')" "d l "
ln /tmp/d/g /tmp/hard
check hard "$(ls -l /tmp/hard | tr -s ' ' | cut -d' ' -f2)" "2"
echo two >> /tmp/hard
check append "$(cat /tmp/d/g | tr '\n' ' ')" "one two "
check rmdir-full "$(rmdir /tmp/d 2>/dev/null; echo $?)" "1"
rm /tmp/l /tmp/d/g /tmp/hard
rmdir /tmp/d
check removed "$(ls /tmp)" ""
cp /bin/sh /tmp/sh
check copy "$(cmp /bin/sh /tmp/sh; echo $?)" "0"
check exec "$(/tmp/sh -c 'echo run')" "run"
rm /tmp/sh

# The size limit, and an unmount that frees the files.
F=/usr/share/fonts/DejaVuSans.ttf
check first-copy "$(cp $F /mnt/small/a 2>/dev/null; echo $?)" "0"
check full "$(cp $F /mnt/small/b 2>/dev/null; echo $?)" "1"
check umount "$(mount -u /mnt/small; echo $?)" "0"
check umounted "$(mount | grep -c '^/mnt/small ')" "0"
# The run harness counts the pages that the mounts would occupy.
check umount-tmp "$(mount -u /tmp; echo $?)" "0"
check umount-boot "$(mount -u /boot; echo $?)" "0"
echo "gpt: done"
