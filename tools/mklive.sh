#!/bin/sh
# Build the live medium (R6 of docs/plan/release-0.5.0.md,
# docs/design/live.md).
# usage: mklive.sh ARCH BASE APPS OUT [ANSWERS]
#
# The tools come from the environment as the top level Makefile exports
# them: PKGHOST, PKGSIGN, PKG_KEY_FILE, PKG_PUB, KERNEL and LIMINE, and
# XORRISO names xorriso. CMDLINE adds options to the kernel command line,
# such as the test of a boot case. BASE is the directory of the base
# packages (build/base) and APPS the one of the application packages
# (build/packages), of which the archives of the current version are taken.
#
# The medium is a hybrid ISO image: El Torito for BIOS and UEFI on a CD
# drive, and a protective MBR for a disk, such as a USB stick, to which the
# image is written. Its ISO 9660 file system with Rock Ridge is the root of
# the live system: the group desktop-system with the installer and the
# disk tools, the signed repository of every package but the tests below
# /repo/ARCH, Limine and the kernel below /boot and /EFI, and, when ANSWERS
# is given, that file as /installer.conf. The kernel mounts the file system
# by its volume identifier MINIOS_LIVE. The live configuration mounts tmpfs
# instances on the directories that the system writes, seeds /home with
# the account live, logs live in without a password, and gives the
# launcher menu the entry "Install minios".
set -e
ARCH="$1"; BASE="$2"; APPS="$3"; OUT="$4"; ANSWERS="$5"
[ -n "$OUT" ] || { echo "usage: mklive.sh ARCH BASE APPS OUT [ANSWERS]" >&2; exit 2; }
TOP="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="$(cat "$TOP/VERSION")"
VOLID=MINIOS_LIVE
: "${XORRISO:=xorriso}"
: "${LIMINE:=$TOP/build/host/limine}"
WORK="$OUT.d"
ROOT="$WORK/root"
rm -rf "$WORK"
mkdir -p "$ROOT/repo" "$WORK/keys"
trap 'rm -rf "$WORK"' EXIT

# The repository of the medium: the base packages but tests, and the
# applications of this version, as on the installation medium.
ARCHIVES="$(ls "$BASE"/*.mpk | grep -v '/tests-[0-9.]*\.mpk$') $(ls "$APPS"/*-"$VERSION".mpk "$APPS"/luasynth-*.mpk 2>/dev/null | sort -u)"
"$TOP/tools/mkrepo.sh" "$PKGSIGN" "$PKG_KEY_FILE" "$ROOT/repo/$ARCH" $ARCHIVES > /dev/null

# The live system, installed from that repository.
cp "$PKG_PUB" "$WORK/keys/build.pub"
printf 'repo medium file://%s/repo/$arch\n' "$ROOT" > "$WORK/pkg.conf"
"$PKGHOST" --root "$ROOT" --arch "$ARCH" --config "$WORK/pkg.conf" --keys "$WORK/keys" update > /dev/null
"$PKGHOST" --root "$ROOT" --arch "$ARCH" --config "$WORK/pkg.conf" --keys "$WORK/keys" \
    install desktop-system installer disktools > "$WORK/install.log" ||
    { cat "$WORK/install.log" >&2; exit 1; }
rm -rf "$ROOT/var/lib/pkg/_repos"

# The modes of the packages are applied to the tree, which xorriso
# records. Every file belongs to root, except the owners that pkg perms
# names, which xorriso sets.
OWNERS="$WORK/owners"
: > "$OWNERS"
"$PKGHOST" --root "$ROOT" perms | grep -v '^#' | while read -r path mode uid gid; do
    case "$path" in */) continue ;; esac
    [ "$mode" = "-" ] || chmod "$mode" "$ROOT$path"
    if [ "$uid" != 0 ] || [ "$gid" != 0 ]; then
        printf '%s %s %s\n' "$path" "$uid" "$gid" >> "$OWNERS"
    fi
done

# The live configuration.
cat > "$ROOT/etc/init.conf" <<'CONF'
# The init of the live medium (tools/mklive.sh, docs/design/live.md).
env PATH=/usr/bin TERM=minios
# The tmpfs instances of /etc/fstab, /home with the account live.
task fsinit fsinit
task keymap loadkeys -c
service audio if=/dev/pcm0 audiod
task network net apply
service dhcp restart=failure dhcpc -a
CONF
if [ -n "$ANSWERS" ]; then
    # The answer file of the medium installs without questions and powers
    # off. The installer is the console program, as in the installer
    # environment, because init serves initctl only after a task has ended.
    cp "$ANSWERS" "$ROOT/installer.conf"
    cat >> "$ROOT/etc/init.conf" <<'CONF'
console installer installer
CONF
else
    cat >> "$ROOT/etc/init.conf" <<'CONF'
# The session of the account live starts without a login.
console greeter greeter -a live
CONF
fi
cat > "$ROOT/etc/fstab" <<'CONF'
# The live medium is read only. The directories that the system writes
# are in memory, and /home receives the account live (tools/mklive.sh).
tmpfs /home tmpfs mode=755,seed=/usr/share/live/home,homes
tmpfs /root tmpfs mode=700,seed=/etc/skel
tmpfs /tmp tmpfs mode=1777
tmpfs /run tmpfs mode=755
tmpfs /var/log tmpfs mode=755
tmpfs /var/db tmpfs mode=755
tmpfs /var/run tmpfs mode=755
CONF
# The seed of /home: the skeleton of the image with the account live in
# place of the account user. live has no password and belongs to wheel.
SEED="$ROOT/usr/share/live/home"
mkdir -p "$SEED"
cp -Rp "$ROOT/usr/share/skel/home/." "$SEED/"
cat > "$SEED/.local/etc/passwd" <<'CONF'
root:x:0:0:Superuser:/root:/bin/sh
live:x:1000:1000:Live session:/home/live:/bin/sh
CONF
cat > "$SEED/.local/etc/group" <<'CONF'
root:x:0:
wheel:x:10:live
live:x:1000:
CONF
printf 'root::0::::::\nlive::0::::::\n' > "$SEED/.local/etc/shadow"
chmod 600 "$SEED/.local/etc/shadow"
cat >> "$ROOT/etc/doas.conf" <<'CONF'
# The live session starts the installer without a password (tools/mklive.sh).
permit nopass live as root cmd /usr/bin/installer-gui args --session
CONF
printf 'Install minios=/bin/doas /usr/bin/installer-gui --session\n' >> "$ROOT/etc/launcher"
printf '%s\n' "$VOLID" > "$ROOT/etc/live-medium"

# The boot files.
mkdir -p "$ROOT/boot/minios" "$ROOT/boot/limine" "$ROOT/EFI/BOOT"
cp "$KERNEL" "$ROOT/boot/minios/kernel.elf"
cp "$TOP/third_party/limine/limine-bios.sys" "$TOP/third_party/limine/limine-bios-cd.bin" \
   "$TOP/third_party/limine/limine-uefi-cd.bin" "$ROOT/boot/limine/"
cp "$TOP/third_party/limine/BOOTX64.EFI" "$TOP/third_party/limine/BOOTAA64.EFI" "$ROOT/EFI/BOOT/"
cat > "$ROOT/boot/limine/limine.conf" <<CONF
# The boot loader of the live medium (tools/mklive.sh).
timeout: 3
serial: yes

/minios $VERSION live
    protocol: limine
    path: boot():/boot/minios/kernel.elf
    cmdline: root=LABEL=$VOLID swap=off${CMDLINE:+ $CMDLINE}
    resolution: 1024x768
CONF

# The image. The owners are set after the tree is mapped: root for every
# file, then the owners of the packages.
set -- -chown_r 0 / -- -chgrp_r 0 / --
while read -r path uid gid; do
    set -- "$@" -chown "$uid" "$path" -- -chgrp "$gid" "$path" --
done < "$OWNERS"
"$XORRISO" -as mkisofs -quiet -R -V "$VOLID" \
    -b boot/limine/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 -boot-info-table \
    --efi-boot boot/limine/limine-uefi-cd.bin -efi-boot-part --efi-boot-image \
    --protective-msdos-label -o "$OUT" "$ROOT" -- "$@" 2>"$WORK/xorriso.log" ||
    { cat "$WORK/xorriso.log" >&2; exit 1; }
"$LIMINE" bios-install "$OUT" > /dev/null 2>&1
echo "mklive.sh: $OUT, $(( $(wc -c < "$OUT") / 1048576 )) MiB"
