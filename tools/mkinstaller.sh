#!/bin/sh
# Build the installation medium (docs/design/installer.md, P7 of
# docs/plan/packaging.md).
# usage: mkinstaller.sh ARCH BASE APPS OUT SIZE_MB [ANSWERS]
#
# The tools come from the environment as the top level Makefile exports
# them: PKGHOST, PKGSIGN, PKG_KEY_FILE, PKG_PUB, MKFS, MKFAT, MKGPT, LIMINE
# and KERNEL. BASE is the directory of the base packages (build/base) and
# APPS the one of the application packages (build/packages), of which the
# archives of the current version are taken. The medium is a GPT disk.
# Its EFI system partition holds Limine, the kernel and the initrd of the
# installer environment, with the command line root=initrd swap=off, and a
# second partition, of the type linux, holds the signed repository of all
# packages but the tests below repo/ARCH and, when ANSWERS is given, that
# file as installer.conf, which makes the installer run without questions.
# On x86_64 the medium also boots through the BIOS. The installer
# environment is the minimal group with the disk tools and the installer,
# whose init mounts the tmpfs of /tmp and /run and starts the installer on
# the console.
set -e
ARCH="$1"; BASE="$2"; APPS="$3"; OUT="$4"; SIZE_MB="$5"; ANSWERS="$6"
[ -n "$SIZE_MB" ] || { echo "usage: mkinstaller.sh ARCH BASE APPS OUT SIZE_MB [ANSWERS]" >&2; exit 2; }
TOP="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="$(cat "$TOP/VERSION")"
WORK="$OUT.d"
rm -rf "$WORK"
mkdir -p "$WORK/medium/repo" "$WORK/esp/EFI/BOOT" "$WORK/esp/minios" "$WORK/keys"
trap 'rm -rf "$WORK"' EXIT

# The repository of the medium: the base packages but tests, and the
# applications of this version.
ARCHIVES="$(ls "$BASE"/*.mpk | grep -v '/tests-[0-9.]*\.mpk$') $(ls "$APPS"/*-"$VERSION".mpk "$APPS"/luasynth-*.mpk 2>/dev/null | sort -u)"
"$TOP/tools/mkrepo.sh" "$PKGSIGN" "$PKG_KEY_FILE" "$WORK/medium/repo/$ARCH" $ARCHIVES > /dev/null
[ -n "$ANSWERS" ] && cp "$ANSWERS" "$WORK/medium/installer.conf"

# The installer environment, installed from that repository.
ENV="$WORK/env"
mkdir -p "$ENV"
cp "$PKG_PUB" "$WORK/keys/build.pub"
printf 'repo medium file://%s/medium/repo/$arch\n' "$WORK" > "$WORK/pkg.conf"
"$PKGHOST" --root "$ENV" --arch "$ARCH" --config "$WORK/pkg.conf" --keys "$WORK/keys" update > /dev/null
"$PKGHOST" --root "$ENV" --arch "$ARCH" --config "$WORK/pkg.conf" --keys "$WORK/keys" \
    install minimal disktools installer > "$WORK/env.log" ||
    { cat "$WORK/env.log" >&2; exit 1; }
rm -rf "$ENV/var/lib/pkg/_repos"
"$PKGHOST" --root "$ENV" perms | grep -v '^#' | while read -r path mode uid gid; do
    case "$path" in */) continue ;; esac
    chmod "$mode" "$ENV$path"
done
# The environment runs from memory. Its init mounts the tmpfs of /tmp and
# /run and runs the installer on the console, and the account databases
# exist for the programs that look accounts up.
cat > "$ENV/etc/init.conf" <<'CONF'
# The init of the installer environment (tools/mkinstaller.sh).
env PATH=/usr/bin TERM=minios
task fsinit fsinit
task keymap loadkeys -c
console installer installer
CONF
cat > "$ENV/etc/fstab" <<'CONF'
# The installer environment runs from the initrd, with /tmp and /run in memory.
tmpfs /tmp tmpfs mode=1777
tmpfs /run tmpfs mode=755
CONF
cp -Rp "$ENV/usr/share/skel/home/." "$ENV/home/"
(cd "$ENV" && tar --format ustar --owner=0 --group=0 -cf "$WORK/esp/minios/initrd.tar" .)

# The EFI system partition.
cp "$KERNEL" "$WORK/esp/minios/kernel.elf"
if [ "$ARCH" = x86_64 ]; then
    cp "$TOP/third_party/limine/BOOTX64.EFI" "$WORK/esp/EFI/BOOT/"
    mkdir -p "$WORK/esp/limine"
    cp "$TOP/third_party/limine/limine-bios.sys" "$WORK/esp/limine/"
else
    cp "$TOP/third_party/limine/BOOTAA64.EFI" "$WORK/esp/EFI/BOOT/"
fi
cat > "$WORK/esp/limine.conf" <<CONF
# The boot loader of the installation medium (tools/mkinstaller.sh).
timeout: 3
serial: yes

/minios $VERSION installer
    protocol: limine
    path: boot():/minios/kernel.elf
    module_path: boot():/minios/initrd.tar
    cmdline: root=initrd swap=off
CONF

ESP_MB=$(( $(du -sk "$WORK/esp" | cut -f1) / 1024 + 16 ))
[ "$ESP_MB" -ge 40 ] || ESP_MB=40
MED_MB=$(( $(du -sk "$WORK/medium" | cut -f1) / 1024 + 32 ))
BIOS=""
[ "$ARCH" = x86_64 ] && BIOS="bios:1"
BIOS_MB=0
[ -n "$BIOS" ] && BIOS_MB=1
NEED=$((3 + BIOS_MB + ESP_MB + MED_MB))
[ "$SIZE_MB" -ge "$NEED" ] || { echo "mkinstaller.sh: the medium needs $NEED MiB" >&2; exit 1; }
"$MKFAT" -t 32 "$WORK/esp.img" "$ESP_MB" "$WORK/esp" > /dev/null
"$MKFS" "$WORK/medium.img" "$MED_MB" "$WORK/medium" > /dev/null
"$MKGPT" "$OUT" "$SIZE_MB" $BIOS "esp:$ESP_MB:$WORK/esp.img" "linux:rest:$WORK/medium.img" > /dev/null
if [ -n "$BIOS" ]; then
    "$LIMINE" bios-install "$OUT" 1 > /dev/null 2>&1
fi
echo "mkinstaller.sh: $OUT, $ESP_MB MiB of boot files and $MED_MB MiB of packages"
