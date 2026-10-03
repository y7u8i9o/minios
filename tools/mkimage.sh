#!/bin/sh
# Build the root image and the initrd by installing packages
# (docs/plan/packaging.md, P3, and docs/design/packages.md).
# usage: mkimage.sh PKG ARCH ROOT TREE DISK DISK_MB MKFS INITRD ARCHIVE...
#
# PKG is the host build of pkg, ARCH the machine of the packages and ROOT
# the build tree, from which only home/ and root/ are taken: the homes of
# the accounts of the image, which belong to no package. TREE is emptied
# and receives the installation. DISK is written with MKFS as an mfs image
# of DISK_MB MiB, and INITRD as a ustar archive of the same tree without
# the sounds, which the boot firmware would have to hold in memory.
#
# The host build of pkg runs without root, which means the tree has
# neither the owners nor the setuid bits of the packages. pkg perms lists
# them, with user/perms for the homes, and mkfs applies the list. The
# modes are applied to the tree as well, which gives the initrd its setuid
# bits, while tar names root as the owner of every member.
set -e
PKG="$1"; ARCH="$2"; ROOT="$3"; TREE="$4"; DISK="$5"; DISK_MB="$6"; MKFS="$7"; INITRD="$8"
[ $# -ge 9 ] || { echo "usage: mkimage.sh PKG ARCH ROOT TREE DISK DISK_MB MKFS INITRD ARCHIVE..." >&2; exit 2; }
shift 8
TOP="$(cd "$(dirname "$0")/.." && pwd)"

rm -rf "$TREE"
mkdir -p "$TREE"
"$PKG" --root "$TREE" --arch "$ARCH" install "$@" > "$TREE.log" ||
    { cat "$TREE.log" >&2; echo "mkimage.sh: the installation into $TREE failed" >&2; exit 1; }

# The homes of the image, as user/Makefile installs them into ROOT.
for d in home root; do
    mkdir -p "$TREE/$d"
    cp -Rp "$ROOT/$d/." "$TREE/$d/"
done

PERMS="$TREE.perms"
"$PKG" --root "$TREE" perms > "$PERMS"
cat "$TOP/user/perms" >> "$PERMS"
grep -v '^#' "$PERMS" | while read -r path mode uid gid; do
    case "$path" in */) continue ;; esac
    [ "$mode" = "-" ] || chmod "$mode" "$TREE$path"
done

"$MKFS" -p "$PERMS" "$DISK" "$DISK_MB" "$TREE"
(cd "$TREE" && tar --format ustar --owner=0 --group=0 --exclude ./usr/share/sounds -cf "$INITRD" .)
