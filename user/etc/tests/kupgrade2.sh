# The second part of the kernel upgrade test, which the kernel that the
# first part installed runs after the boot loader of the disk loaded it.
check() {
    test "$2" = "$3" || echo "FAIL $1: [$2]"
}
check kernel "$(pkg list | grep '^kernel ')" "kernel 99.0 The kernel"
check fsinit "$(fsinit > /dev/null; echo $?)" "0"
check previous-entry "$(grep -c '^/minios, the previous kernel$' /boot/limine.conf)" "1"
for m in /boot /tmp /run; do
    mount -u $m
done
echo "kupgrade2: done"
