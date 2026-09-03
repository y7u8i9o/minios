#!/bin/sh
# Boot one test case headless and check its serial output.
# usage: LIMINE=<tool> run_qemu_test.sh <kernel.elf> <build dir> <case dir>
#
# A case directory contains:
#   cmdline   kernel command line (optional, typically test=<name>)
#   expect    one extended regex per line, every line must match the serial log
#   reject    one extended regex per line, no line may match (optional)
#   timeout   seconds to wait before declaring failure (optional, default 30)
#   mem       QEMU memory size in MiB (optional, default 512)
#   cpus      number of CPUs (optional, default $CPUS or 4)
#   swap      size in MiB of a zero filled swap image attached as vdb (optional)
#   mfs2      size in MiB of an empty mfs image attached as the next virtio-blk
#             device (optional); the post script sees it as DISK2
#   fat       one line per FAT image to attach, "<size_mb> <12|16|32> [dir]"
#             built by mkfat from the directory under the case (optional);
#             the post script sees the first as FATIMG, all as FATIMGS
#   audio     QEMU audio backend for a virtio-sound device: none or wav
#   vga       std (default) or virtio (virtio-vga, the virtio-gpu driver)
#   tablet    present: attach a virtio-tablet-pci device
#   disk.img  a private root image instead of the shared one (optional)
#   post      executable run after QEMU exits with DISK, SERIAL, EXITCODE,
#             TOP and BUILD in the environment (optional)
KERNEL="$1"
BUILD="$2"
CASE="$3"
NAME="$(basename "$CASE")"
TOP="$(cd "$(dirname "$0")/.." && pwd)"
QEMU="${QEMU:-qemu-system-x86_64}"
OUTDIR="$BUILD/$NAME"
mkdir -p "$OUTDIR"
CMDLINE=""
[ -f "$CASE/cmdline" ] && CMDLINE="$(cat "$CASE/cmdline")"
TIMEOUT=30
[ -f "$CASE/timeout" ] && TIMEOUT="$(cat "$CASE/timeout")"
MEM=512
[ -f "$CASE/mem" ] && MEM="$(cat "$CASE/mem")"
CPUS="${CPUS:-4}"
[ -f "$CASE/cpus" ] && CPUS="$(cat "$CASE/cpus")"
SERIAL="$OUTDIR/serial.txt"
ISO="$OUTDIR/test.iso"

"$TOP/tools/mkiso.sh" "$KERNEL" "$ISO" "$CMDLINE" || { echo "FAIL $NAME (image build)"; exit 1; }
rm -f "$SERIAL"
# Every case gets a private copy of the disk image so writes do not leak
# between cases. A case may provide its own image as <case>/disk.img.
# The copy is a copy-on-write clone where the file system supports it
# (APFS: cp -c) and is deleted when the case ends, so a run never holds
# more than one image per running case.
clone() {
    cp -c "$1" "$2" 2>/dev/null || cp "$1" "$2"
}
cleanup_images() {
    rm -f "$OUTDIR/disk.img" "$OUTDIR/swap.img" "$OUTDIR/disk2.img" "$OUTDIR"/fat*.img "$OUTDIR/test.iso"
}
trap cleanup_images EXIT
DISKFLAGS=""
if [ -f "$CASE/disk.img" ]; then
    clone "$CASE/disk.img" "$OUTDIR/disk.img"
elif [ -n "$DISK" ] && [ -f "$DISK" ]; then
    clone "$DISK" "$OUTDIR/disk.img"
fi
SOUNDFLAGS=""
if [ -f "$CASE/audio" ]; then
    AUDIO_BACKEND="$(cat "$CASE/audio")"
    if [ "$AUDIO_BACKEND" = wav ]; then
        SOUNDFLAGS="-audiodev wav,id=minios_audio,path=$OUTDIR/audio.wav -device virtio-sound-pci,audiodev=minios_audio"
    elif [ "$AUDIO_BACKEND" = none ]; then
        SOUNDFLAGS="-audiodev none,id=minios_audio -device virtio-sound-pci,audiodev=minios_audio"
    else
        echo "FAIL $NAME (unknown audio backend: $AUDIO_BACKEND)"
        exit 1
    fi
fi
VGAFLAGS="-vga std"
[ -f "$CASE/vga" ] && VGAFLAGS="-vga $(cat "$CASE/vga")"
[ -f "$CASE/tablet" ] && VGAFLAGS="$VGAFLAGS -device virtio-tablet-pci"
if [ -f "$OUTDIR/disk.img" ]; then
    DISKFLAGS="-drive file=$OUTDIR/disk.img,if=none,id=vd0,format=raw -device virtio-blk-pci,drive=vd0"
fi
NDISK=1
if [ -f "$CASE/swap" ]; then
    dd if=/dev/zero of="$OUTDIR/swap.img" bs=1048576 count="$(cat "$CASE/swap")" status=none
    DISKFLAGS="$DISKFLAGS -drive file=$OUTDIR/swap.img,if=none,id=vd$NDISK,format=raw -device virtio-blk-pci,drive=vd$NDISK"
    NDISK=$((NDISK + 1))
fi
DISK2=""
if [ -f "$CASE/mfs2" ]; then
    mkdir -p "$OUTDIR/empty"
    "${MKFS:-$(dirname "$BUILD")/host/mkfs}" "$OUTDIR/disk2.img" "$(cat "$CASE/mfs2")" "$OUTDIR/empty" >/dev/null || { echo "FAIL $NAME (mfs2 image)"; exit 1; }
    DISK2="$OUTDIR/disk2.img"
    DISKFLAGS="$DISKFLAGS -drive file=$DISK2,if=none,id=vd$NDISK,format=raw -device virtio-blk-pci,drive=vd$NDISK"
    NDISK=$((NDISK + 1))
fi
FATIMG=""
FATIMGS=""
if [ -f "$CASE/fat" ]; then
    NFAT=0
    while read -r size type dir; do
        [ -z "$size" ] && continue
        IMG="$OUTDIR/fat$NFAT.img"
        if [ -n "$dir" ]; then
            "${MKFAT:-$(dirname "$BUILD")/host/mkfat}" -t "$type" "$IMG" "$size" "$CASE/$dir" >/dev/null || { echo "FAIL $NAME (fat image)"; exit 1; }
        else
            "${MKFAT:-$(dirname "$BUILD")/host/mkfat}" -t "$type" "$IMG" "$size" >/dev/null || { echo "FAIL $NAME (fat image)"; exit 1; }
        fi
        [ -z "$FATIMG" ] && FATIMG="$IMG"
        FATIMGS="$FATIMGS $IMG"
        DISKFLAGS="$DISKFLAGS -drive file=$IMG,if=none,id=vd$NDISK,format=raw -device virtio-blk-pci,drive=vd$NDISK"
        NDISK=$((NDISK + 1))
        NFAT=$((NFAT + 1))
    done < "$CASE/fat"
fi
# The hypervisor framework when this QEMU offers it (ACCEL=tcg forces
# binary translation).
if [ -z "$ACCEL" ]; then
    if "$QEMU" -accel help 2>/dev/null | grep -q '^hvf$'; then ACCEL=hvf; else ACCEL=tcg; fi
fi
"$QEMU" -M q35 -m "${MEM}M" -smp "$CPUS" -accel "$ACCEL" -display none -no-reboot \
    -serial "file:$SERIAL" \
    -device isa-debug-exit,iobase=0xf4,iosize=0x4 \
    $DISKFLAGS $SOUNDFLAGS $VGAFLAGS \
    -cdrom "$ISO" >"$OUTDIR/qemu.log" 2>&1 &
QPID=$!
ELAPSED=0
while kill -0 $QPID 2>/dev/null; do
    if [ "$ELAPSED" -ge "$TIMEOUT" ]; then
        kill $QPID 2>/dev/null
        wait $QPID 2>/dev/null
        echo "FAIL $NAME (timeout after ${TIMEOUT}s, log: $SERIAL)"
        exit 1
    fi
    sleep 1
    ELAPSED=$((ELAPSED + 1))
done
wait $QPID
echo "$?" > "$OUTDIR/exitcode"

STATUS=0
touch "$SERIAL"
if grep -q "TEST FAIL" "$SERIAL"; then
    echo "FAIL $NAME: $(grep -m1 'TEST FAIL' "$SERIAL")"
    STATUS=1
fi
if [ -f "$CASE/expect" ]; then
    while IFS= read -r pat; do
        [ -z "$pat" ] && continue
        if ! grep -E -q -- "$pat" "$SERIAL"; then
            echo "FAIL $NAME: missing /$pat/"
            STATUS=1
        fi
    done < "$CASE/expect"
fi
if [ -f "$CASE/reject" ]; then
    while IFS= read -r pat; do
        [ -z "$pat" ] && continue
        if grep -E -q -- "$pat" "$SERIAL"; then
            echo "FAIL $NAME: found /$pat/"
            STATUS=1
        fi
    done < "$CASE/reject"
fi
if [ -x "$CASE/post" ]; then
    if ! DISK="$OUTDIR/disk.img" DISK2="$DISK2" FATIMG="$FATIMG" FATIMGS="$FATIMGS" SERIAL="$SERIAL" \
         EXITCODE="$(cat "$OUTDIR/exitcode")" TOP="$TOP" BUILD="$(dirname "$BUILD")" "$CASE/post"; then
        echo "FAIL $NAME: post check failed"
        STATUS=1
    fi
fi
[ "$STATUS" -eq 0 ] && echo "PASS $NAME"
[ "$STATUS" -ne 0 ] && echo "  serial log: $SERIAL"
exit $STATUS
