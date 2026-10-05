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
# the sounds, which the boot firmware would have to load into memory.
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

# The script records a key of its inputs in DISK.key after a build: the
# checksums of the archives, of user/perms, of this script and of the host
# programs PKG and MKFS, the arguments, the directories of the homes, and
# the sizes and times of the files of the homes. The times of directories
# are left out, because the build recreates the directories. DISK.key also
# records the modification time of DISK. When the key and the time are
# unchanged and DISK and INITRD exist, the script ends without work. make
# run writes into DISK, and a changed time of DISK therefore causes a new
# build.
checksum() {
    if command -v sha256sum > /dev/null 2>&1; then sha256sum; else shasum -a 256; fi
}
mtime() {
    stat -f %m "$1" 2> /dev/null || stat -c %Y "$1"
}
KEY=$({
    echo "$ARCH $DISK_MB $TREE"
    cat "$@" "$TOP/user/perms" "$0" "$PKG" "$MKFS" | checksum
    for d in home root; do
        [ -d "$ROOT/$d" ] || continue
        find "$ROOT/$d" -type d
        find "$ROOT/$d" ! -type d -exec ls -ln {} +
    done
} | checksum | cut -d' ' -f1)
if [ -f "$DISK" ] && [ -f "$INITRD" ] && [ -f "$DISK.key" ] &&
   [ "$(cat "$DISK.key")" = "$KEY $(mtime "$DISK")" ]; then
    echo "mkimage.sh: $DISK and $INITRD are up to date"
    exit 0
fi
rm -f "$DISK.key"

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
echo "$KEY $(mtime "$DISK")" > "$DISK.key"
