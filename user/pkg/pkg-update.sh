#!/bin/sh
# pkg-update: apply the packages of an update medium at boot, before the
# login (docs/design/packages.md, P8 of docs/plan/packaging.md). init runs
# it as a task after fsinit. An update medium is a partition of the type
# repo, the type of minios for a signed package repository, that contains
# repo/MACHINE/index, as the development disk receives it from `make run`.
# Without one the task ends at once. With one, pkg upgrades every
# installed package that the repository offers in a higher version, and
# when the kernel, the boot loader or the C library changed, the system
# restarts, which loads the new kernel.
REPO_TYPE=6d696e69-6f73-4e70-6b67-7265706f7369
MEDIUM=/run/update
ARCH=$(uname -m)
part=""
while read -r name disk uuid type bytes; do
    if [ "$type" = "$REPO_TYPE" ]; then
        part=$name
        break
    fi
done < /dev/partitions
if [ -z "$part" ]; then
    exit 0
fi
mkdir -p $MEDIUM
if ! mount mfs "$part" $MEDIUM; then
    echo "pkg-update: cannot mount $part"
    exit 1
fi
if [ ! -f "$MEDIUM/repo/$ARCH/index" ]; then
    echo "pkg-update: $part contains no repository for $ARCH"
    mount -u $MEDIUM
    exit 0
fi
echo "repo update file://$MEDIUM/repo/\$arch" > /run/pkg-update.conf
status=0
if ! pkg --config /run/pkg-update.conf update > /run/pkg-update.log 2>&1 ||
   ! pkg --config /run/pkg-update.conf upgrade >> /run/pkg-update.log 2>&1; then
    status=1
fi
sed 's/^/pkg-update: /' /run/pkg-update.log
mount -u $MEDIUM
if [ $status -ne 0 ]; then
    echo "pkg-update: the update failed"
    exit 1
fi
if grep -E '^upgraded (kernel|limine|libc) ' /run/pkg-update.log > /dev/null; then
    echo "pkg-update: restarting to start the new system"
    sync
    # init waits for this task, and answers the request once it ends.
    initctl reboot > /dev/null 2>&1 &
fi
exit 0
