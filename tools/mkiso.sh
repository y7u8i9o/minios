#!/bin/sh
# Build a bootable Limine ISO.
# usage: LIMINE=<limine host tool> mkiso.sh <kernel.elf> <out.iso> [cmdline]
set -e
KERNEL="$1"
OUT="$2"
CMDLINE="$3"
TOP="$(cd "$(dirname "$0")/.." && pwd)"
LIMINE_DIR="$TOP/third_party/limine"
: "${LIMINE:=$TOP/build/host/limine}"
: "${INITRD:=$TOP/build/initrd.tar}"

ROOT="$(dirname "$OUT")/iso_root_$(basename "$OUT" .iso)"
rm -rf "$ROOT"
mkdir -p "$ROOT/boot/limine" "$ROOT/EFI/BOOT"
cp "$KERNEL" "$ROOT/boot/kernel.elf"
cp "$INITRD" "$ROOT/boot/initrd.tar"
sed "s|^    cmdline: .*|    cmdline: $CMDLINE|" "$TOP/limine.conf" > "$ROOT/boot/limine/limine.conf"
# video=WxH on the command line selects the framebuffer mode.
VIDEO="$(printf '%s' "$CMDLINE" | tr ' ' '\n' | sed -n 's/^video=//p' | head -1)"
[ -n "$VIDEO" ] && sed -i.bak "s|^    resolution: .*|    resolution: $VIDEO|" "$ROOT/boot/limine/limine.conf" && rm -f "$ROOT/boot/limine/limine.conf.bak"
true
cp "$LIMINE_DIR/limine-bios.sys" "$LIMINE_DIR/limine-bios-cd.bin" \
   "$LIMINE_DIR/limine-uefi-cd.bin" "$ROOT/boot/limine/"
cp "$LIMINE_DIR/BOOTX64.EFI" "$ROOT/EFI/BOOT/"

xorriso -as mkisofs -quiet -R -r -J \
    -b boot/limine/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 -boot-info-table \
    -hfsplus -apm-block-size 2048 \
    --efi-boot boot/limine/limine-uefi-cd.bin -efi-boot-part --efi-boot-image \
    --protective-msdos-label "$ROOT" -o "$OUT" 2>/dev/null
"$LIMINE" bios-install "$OUT" >/dev/null 2>&1
rm -rf "$ROOT"
