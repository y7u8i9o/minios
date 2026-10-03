# The first part of the kernel upgrade test, run as sh /etc/tests/kupgrade.sh
# by tests/cases/kernel_upgrade from its CD. The disk is an installed
# system whose /boot is the EFI system partition. A kernel package of a
# higher version replaces the installed one, which becomes the previous
# kernel, and the boot loader configuration lists both. The second boot
# of the case loads the new kernel from the disk. Every failing check
# prints a line starting with FAIL and the value received.
check() {
    test "$2" = "$3" || echo "FAIL $1: [$2]"
}
check fsinit "$(fsinit > /dev/null; echo $?)" "0"
check boot-mounted "$(mount | grep -c '^/boot type fat$')" "1"
check verify "$(pkg verify kernel limine; echo $?)" "0"
check entries-before "$(grep -c '^/minios' /boot/limine.conf)" "1"
mkdir -p /ku/files/boot/minios
cp /boot/minios/kernel.elf /ku/files/boot/minios/kernel.elf
printf 'name kernel\nversion 99.0\nsummary The kernel\nkernel boot/minios/kernel.elf\n' > /ku/manifest
check build "$(pkg build /ku /ku/kernel-99.0.mpk)" "/ku/kernel-99.0.mpk"
check upgrade "$(pkg install /ku/kernel-99.0.mpk)" "upgraded kernel 99.0"
check previous "$(cmp /boot/minios/kernel.elf.old /boot/minios/kernel.elf; echo $?)" "0"
check entries "$(grep -c '^/minios' /boot/limine.conf)" "2"
check title "$(grep '^/minios' /boot/limine.conf | head -n 1)" "/minios 99.0"
check cmdline "$(grep -c 'cmdline: root=PARTUUID=c0ffee00-0000-4000-8000-0000000000c4 test=run' /boot/limine.conf)" "2"
check verify-after "$(pkg verify kernel; echo $?)" "0"
for m in /boot /tmp /run; do
    mount -u $m
done
sync
echo "kupgrade: done"
