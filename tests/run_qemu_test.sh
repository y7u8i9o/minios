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
#   keyboard  present: attach a virtio-keyboard-pci device
#   disk.img  a private root image instead of the shared one (optional)
#   nic       network backend of a virtio-net-pci device (optional): dgram
#             exchanges raw Ethernet frames with the case's peer program
#             over UDP on 127.0.0.1, user attaches QEMU's user mode stack,
#             none attaches nothing. Frames are captured to <out>/capture.pcap
#             (docs/design/network.md)
#   peer      executable started before QEMU for nic dgram, with NETPEER
#             (the host tool tools/netpeer), PEER_READY, PEER_LOG, PEER_PID,
#             OUTDIR, TOP and BUILD in the environment. It writes
#             "<peer port> <guest port>" to PEER_READY once it listens and
#             is terminated when QEMU has exited; the post script sees
#             PEER_LOG and PEER_READY
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
PEERPID=""
PEER_READY="$OUTDIR/peer.ready"
PEER_LOG="$OUTDIR/peer.log"
PEER_PID="$OUTDIR/peer.pid"

# The peer of a network case is stopped whenever this script ends: after
# QEMU exited, after the timeout killed it, and on every early failure.
# A peer that ignores SIGTERM for five seconds is killed.
stop_peer() {
    [ -n "$PEERPID" ] || return 0
    if kill -0 "$PEERPID" 2>/dev/null; then
        kill "$PEERPID" 2>/dev/null
        i=0
        while kill -0 "$PEERPID" 2>/dev/null && [ "$i" -lt 50 ]; do
            sleep 0.1
            i=$((i + 1))
        done
        kill -0 "$PEERPID" 2>/dev/null && kill -9 "$PEERPID" 2>/dev/null
    fi
    wait "$PEERPID" 2>/dev/null
    PEERPID=""
}
fail() {
    echo "FAIL $NAME ($1)"
    exit 1
}
# Every case gets a private copy of the disk image so writes do not leak
# between cases. A case may provide its own image as <case>/disk.img.
# The copy is a copy-on-write clone where the file system supports it
# (APFS: cp -c) and is deleted when the case ends, so a run never holds
# more than one image per running case.
clone() {
    cp -c "$1" "$2" 2>/dev/null || cp "$1" "$2"
}
cleanup() {
    stop_peer
    rm -f "$OUTDIR/disk.img" "$OUTDIR/swap.img" "$OUTDIR/disk2.img" "$OUTDIR"/fat*.img "$OUTDIR/test.iso"
}
trap cleanup EXIT

# The network device and its peer are prepared before the image so that
# a peer that fails to start costs nothing else. The backend named in
# the nic file must be one this QEMU offers.
NETFLAGS=""
if [ -f "$CASE/nic" ]; then
    NIC="$(cat "$CASE/nic")"
    case "$NIC" in
        none) ;;
        dgram|user)
            "$QEMU" -netdev help 2>/dev/null | grep -qx "$NIC" || fail "nic backend $NIC not offered by $QEMU"
            ;;
        *) fail "unknown nic backend $NIC" ;;
    esac
    if [ "$NIC" = dgram ]; then
        [ -x "$CASE/peer" ] || fail "nic dgram needs an executable peer"
        rm -f "$PEER_READY" "$PEER_LOG" "$PEER_PID"
        NETPEER="${NETPEER:-$(dirname "$BUILD")/host/netpeer}" PEER_READY="$PEER_READY" PEER_LOG="$PEER_LOG" \
            PEER_PID="$PEER_PID" OUTDIR="$OUTDIR" TOP="$TOP" BUILD="$(dirname "$BUILD")" \
            "$CASE/peer" > "$OUTDIR/peer.out" 2>&1 &
        PEERPID=$!
        i=0
        while [ ! -s "$PEER_READY" ]; do
            kill -0 "$PEERPID" 2>/dev/null || fail "peer exited before it was ready: $(cat "$OUTDIR/peer.out")"
            [ "$i" -ge 100 ] && fail "peer not ready after 10 s"
            sleep 0.1
            i=$((i + 1))
        done
        read -r PEERPORT GUESTPORT < "$PEER_READY"
        NETFLAGS="-netdev dgram,id=net0,local.type=inet,local.host=127.0.0.1,local.port=$GUESTPORT,remote.type=inet,remote.host=127.0.0.1,remote.port=$PEERPORT"
    elif [ "$NIC" = user ]; then
        NETFLAGS="-netdev user,id=net0"
    fi
    if [ -n "$NETFLAGS" ]; then
        rm -f "$OUTDIR/capture.pcap"
        NETFLAGS="$NETFLAGS -device virtio-net-pci,netdev=net0,mac=52:54:00:4d:49:4f -object filter-dump,id=dump0,netdev=net0,file=$OUTDIR/capture.pcap"
    fi
fi

"$TOP/tools/mkiso.sh" "$KERNEL" "$ISO" "$CMDLINE" || fail "image build"
rm -f "$SERIAL"
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
[ -f "$CASE/keyboard" ] && VGAFLAGS="$VGAFLAGS -device virtio-keyboard-pci"
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
# Use HVF on macOS when offered. Tests use TCG elsewhere for deterministic
# behavior; ACCEL can explicitly select another accelerator.
if [ -z "$ACCEL" ]; then
    ACCELS="$("$QEMU" -accel help 2>/dev/null)"
    if [ "$(uname -s)" = Darwin ] && echo "$ACCELS" | grep -q '^hvf$'; then
        ACCEL=hvf
    else
        ACCEL=tcg
    fi
fi
"$QEMU" -M q35 -m "${MEM}M" -smp "$CPUS" -accel "$ACCEL" -display none -no-reboot \
    -serial "file:$SERIAL" \
    -device isa-debug-exit,iobase=0xf4,iosize=0x4 \
    $DISKFLAGS $SOUNDFLAGS $VGAFLAGS $NETFLAGS \
    -cdrom "$ISO" >"$OUTDIR/qemu.log" 2>&1 &
QPID=$!
ELAPSED=0
while kill -0 $QPID 2>/dev/null; do
    if [ "$ELAPSED" -ge "$TIMEOUT" ]; then
        kill $QPID 2>/dev/null
        wait $QPID 2>/dev/null
        stop_peer
        echo "FAIL $NAME (timeout after ${TIMEOUT}s, log: $SERIAL)"
        exit 1
    fi
    sleep 1
    ELAPSED=$((ELAPSED + 1))
done
wait $QPID
echo "$?" > "$OUTDIR/exitcode"
# The peer's log is complete once it has been stopped, so the checks
# below and the post script can read it.
stop_peer

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
         PEER_LOG="$PEER_LOG" PEER_READY="$PEER_READY" CAPTURE="$OUTDIR/capture.pcap" \
         EXITCODE="$(cat "$OUTDIR/exitcode")" TOP="$TOP" BUILD="$(dirname "$BUILD")" "$CASE/post"; then
        echo "FAIL $NAME: post check failed"
        STATUS=1
    fi
fi
[ "$STATUS" -eq 0 ] && echo "PASS $NAME"
[ "$STATUS" -ne 0 ] && echo "  serial log: $SERIAL"
exit $STATUS
