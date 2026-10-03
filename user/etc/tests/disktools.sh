# The disk tool test, run as sh /etc/tests/disktools.sh by
# tests/cases/disk_tools. vdb is swap and vdc an empty disk of 256 MiB,
# which part divides into an EFI system partition, a swap partition and a
# root partition. mkfat and mkfs format them, both are mounted and
# written, and the post script of the case checks the disk on the host.
# Every failing check prints a line starting with FAIL and the value
# received.
check() {
    test "$2" = "$3" || echo "FAIL $1: [$2]"
}
D=/dev/vdc
G=c0ffee00-0000-4000-8000-0000000000a
ARCH=$(uname -m)
check part "$(part $D 0 esp:32::${G}1 swap:16::${G}2 root-$ARCH:rest::${G}3 > /dev/null; echo $?)" "0"
check list "$(part -l $D | cut -d' ' -f1,2 | tr '\n' ' ')" "1 esp 2 swap 3 root-$ARCH "
check rescanned "$(grep '^vdc' /dev/partitions | cut -d' ' -f1,3 | tr '\n' ' ')" "vdc1 ${G}1 vdc2 ${G}2 vdc3 ${G}3 "
check esp-size "$(grep '^vdc1 ' /dev/partitions | cut -d' ' -f5)" "33554432"
check esp-stat "$(ls -l /dev/vdc1 | tr -s ' ' | cut -d' ' -f5)" "33554432"

mkdir -p /dt/tree /dt/esp /dt/root
echo "hello from the tree" > /dt/tree/hello.txt
check mkfat "$(mkfat -t 16 /dev/vdc1 0 /dt/tree > /dev/null; echo $?)" "0"
check mkfs "$(mkfs /dev/vdc3 0 /dt/tree > /dev/null; echo $?)" "0"
check mount-esp "$(mount fat vdc1 /dt/esp; echo $?)" "0"
check esp-file "$(cat /dt/esp/hello.txt)" "hello from the tree"
check umount-esp "$(mount -u /dt/esp; echo $?)" "0"
check mount-root "$(mount mfs vdc3 /dt/root; echo $?)" "0"
check root-file "$(cat /dt/root/hello.txt)" "hello from the tree"
echo "written on minios" > /dt/root/written.txt
check umount-root "$(mount -u /dt/root; echo $?)" "0"

# A table written again replaces the partitions: two entries remain, the
# third partition has no sectors and leaves the listing.
check part-again "$(part $D 0 esp:32::${G}1 linux:rest::${G}4 > /dev/null; echo $?)" "0"
check relisted "$(grep '^vdc' /dev/partitions | cut -d' ' -f1,3 | tr '\n' ' ')" "vdc1 ${G}1 vdc2 ${G}4 "
check vdc3-empty "$(ls -l /dev/vdc3 | tr -s ' ' | cut -d' ' -f5)" "0"
# The post script expects the first layout.
check part-restore "$(part $D 0 esp:32::${G}1 swap:16::${G}2 root-$ARCH:rest::${G}3 > /dev/null; echo $?)" "0"
check limine "$(limine version > /dev/null; echo $?)" "0"
sync
echo "disktools: done"
