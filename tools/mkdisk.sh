#!/bin/sh
# Build an installed disk on the host (docs/plan/packaging.md, P6, and
# docs/design/packages.md): a GPT disk whose EFI system partition holds
# /boot of an installation tree, with the kernel, the boot loader and its
# configuration, followed by a swap partition and the root partition with
# the rest of the tree.
# usage: mkdisk.sh PKG ARCH TREE PERMS OUT SIZE_MB MKFS MKFAT MKGPT LIMINE [CMDLINE]
#
# PKG is the host build of pkg and TREE an installation tree that it
# filled, such as build/sysroot. The tree is copied, the copy receives
# /etc/kernel/cmdline with root=PARTUUID and CMDLINE, the entry of /boot in
# /etc/fstab and, on x86_64, /etc/kernel/bios-disk, and pkg bootconfig
# writes the boot loader configuration from it. PERMS is user/perms, which
# is applied with the output of pkg perms. On x86_64 the disk begins with a
# BIOS boot partition, into which limine bios-install writes the BIOS
# stage, and UEFI firmware loads EFI/BOOT/BOOT*.EFI of the EFI system
# partition. DISK_UUID, BIOS_UUID, ESP_UUID, SWAP_UUID and ROOT_UUID fix
# the GUIDs, which are random otherwise, and ESP_MB and SWAP_MB the sizes
# (64 each).
set -e
PKG="$1"; ARCH="$2"; TREE="$3"; PERMS="$4"; OUT="$5"; SIZE_MB="$6"
MKFS="$7"; MKFAT="$8"; MKGPT="$9"
shift 9
LIMINE="$1"; CMDLINE="$2"
[ -n "$LIMINE" ] || { echo "usage: mkdisk.sh PKG ARCH TREE PERMS OUT SIZE_MB MKFS MKFAT MKGPT LIMINE [CMDLINE]" >&2; exit 2; }
guid() { python3 -c 'import uuid; print(uuid.uuid4())'; }
: "${DISK_UUID:=$(guid)}" "${ESP_UUID:=$(guid)}" "${SWAP_UUID:=$(guid)}" "${ROOT_UUID:=$(guid)}"
: "${BIOS_UUID:=$(guid)}"
: "${ESP_MB:=64}" "${SWAP_MB:=64}"

WORK="$OUT.d"
rm -rf "$WORK"
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT
# A clone where the file system offers one (APFS), a copy otherwise.
cp -Rpc "$TREE" "$WORK/root" 2>/dev/null || { rm -rf "$WORK/root"; cp -Rp "$TREE" "$WORK/root"; }
R="$WORK/root"

mkdir -p "$R/etc/kernel"
echo "root=PARTUUID=$ROOT_UUID${CMDLINE:+ $CMDLINE}" > "$R/etc/kernel/cmdline"
BIOS=""
BIOS_MB=0
if [ "$ARCH" = x86_64 ]; then
    echo "PARTUUID=$BIOS_UUID" > "$R/etc/kernel/bios-disk"
    BIOS="bios:1::$BIOS_UUID"
    BIOS_MB=1
fi
printf '# The EFI system partition, which holds the kernel and the boot loader.\nPARTUUID=%s /boot fat\n' "$ESP_UUID" >> "$R/etc/fstab"
"$PKG" --root "$R" --arch "$ARCH" bootconfig

# /boot goes to the EFI system partition, and the root holds it as the
# empty mount point. The owners and modes of the root come from pkg perms
# without the paths below /boot, which FAT cannot hold.
mkdir -p "$WORK/esp"
mv "$R/boot"/* "$WORK/esp"/ 2>/dev/null || true
"$PKG" --root "$R" perms | grep -v '^/boot/' > "$WORK/perms"
cat "$PERMS" >> "$WORK/perms"

RESERVED=$((3 + BIOS_MB + ESP_MB + SWAP_MB))
ROOT_MB=$((SIZE_MB - RESERVED))
[ "$ROOT_MB" -ge 64 ] || { echo "mkdisk.sh: $SIZE_MB MiB leave no room for the root" >&2; exit 1; }
"$MKFAT" -t 32 "$WORK/esp.img" "$ESP_MB" "$WORK/esp" > /dev/null
"$MKFS" -p "$WORK/perms" "$WORK/root.img" "$ROOT_MB" "$R" > /dev/null
"$MKGPT" --disk-uuid "$DISK_UUID" "$OUT" "$SIZE_MB" $BIOS \
    "esp:$ESP_MB:$WORK/esp.img:$ESP_UUID" \
    "swap:$SWAP_MB::$SWAP_UUID" \
    "root-$ARCH:rest:$WORK/root.img:$ROOT_UUID"
if [ -n "$BIOS" ]; then
    "$LIMINE" bios-install "$OUT" 1 > /dev/null 2>&1
fi
