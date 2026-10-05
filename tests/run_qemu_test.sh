#!/bin/sh
# Boot one test case headless and check its serial output. The script is
# run as LIMINE=<tool> run_qemu_test.sh <kernel.elf> <build dir> <case dir>.
#
# A case directory contains the following files.
#   cmdline   kernel command line (optional, typically test=<name>)
#   arches    the architectures the case runs on, one per line (optional,
#             default all). On another architecture the case is reported
#             as SKIP and the script exits with status 2.
#   expect    one extended regex per line, every line must match the serial log
#   reject    one extended regex per line, no line may match (optional)
#   timeout   seconds to wait before declaring failure (optional, default 30)
#   mem       QEMU memory size in MiB (optional, default 512), replaced by
#             mem.ARCH on that architecture
#   cpus      number of CPUs (optional, default $CPUS or 4)
#   gic       GIC version of the aarch64 virt machine, 2 or 3 (optional,
#             default 3)
#   acpi      boots the aarch64 virt machine with ACPI tables (optional).
#             The machine is then given without acpi=off, and edk2 passes
#             the ACPI tables and no device tree (docs/design/acpi.md)
#   swap      size in MiB of a zero filled swap image attached as vdb (optional)
#   mfs2      size in MiB of an empty mfs image attached as the next virtio-blk
#             device (optional), which the post script sees as DISK2
#   fat       one line per FAT image to attach, "<size_mb> <12|16|32> [dir]"
#             built by mkfat from the directory under the case (optional),
#             and the post script sees the first as FATIMG and all as FATIMGS
#   audio     QEMU audio backend for a virtio-sound device, none or wav
#   vga       std (default) or virtio (virtio-vga, the virtio-gpu driver),
#             on aarch64 ramfb (default) or ramfb with virtio-gpu-pci
#   tablet    attaches a virtio-tablet-pci device when present
#   keyboard  attaches a virtio-keyboard-pci device when present
#   diskif    the controller of the root disk (optional, default virtio):
#             nvme, optionally followed by properties of the QEMU nvme
#             device such as ",mdts=2", ahci for the SATA controller (the one of q35,
#             an ich9-ahci on aarch64), usb for usb-storage on its own
#             xHCI controller, or usb@PORT for usb-storage at PORT (such as
#             1.2, port 2 of a hub on port 1) of the controller of the usb
#             file
#   cd        further CD drives on the AHCI controller (optional), one per
#             line: "empty" for a drive without a medium, or "iso DIR
#             [VOLUME_ID]" for an ISO 9660 image with Rock Ridge that
#             xorriso builds from the directory DIR of the case. A line
#             that begins with "usb" attaches the drive as a USB CD drive
#             (usb-bot with scsi-cd) on an xHCI controller of its own. The
#             images are cd0.iso, cd1.iso and so on in the output
#             directory, and the post script sees the first as CDIMG. On
#             x86_64 the drives use the ports 3 to 5 of the q35 controller,
#             whose port 2 has the boot CD. On aarch64 they use an
#             ich9-ahci controller of their own
#   usb       an xHCI controller and USB devices on it (optional): one line,
#             the controller with its properties followed by the devices,
#             for example "qemu-xhci,msix=off usb-kbd usb-tablet". A device
#             written DEVICE@PORT is attached at PORT, for example
#             "usb-hub@1 usb-kbd@1.1"
#   qmp       a script of tests/qmp_input.py (optional), run against the QMP
#             socket of QEMU while the case boots. It sends keys and pointer
#             events after the serial log shows a line, and its output is
#             written to qmp.log in the case's output directory
#   disk.img  a private root image instead of the shared one (optional)
#   mkdisk    executable that writes the case's disk instead (optional),
#             run with DISK (the shared root image), OUT (the disk to
#             write), OUT2, MKGPT, MKFAT, MKFS, ARCH, CASE and TOP in the
#             environment. A disk it writes to OUT2 is attached as the
#             second disk, which the post script sees as DISK2
#   nic       network backend of a virtio-net-pci device (optional), where
#             dgram exchanges raw Ethernet frames with the case's peer program
#             over UDP on 127.0.0.1, user attaches QEMU's user mode stack,
#             none attaches nothing. Frames are captured to <out>/capture.pcap
#             (docs/design/network.md)
#   peer      executable started before QEMU (required for dgram, optional
#             for user, where a nonzero guest port in PEER_READY is
#             forwarded to guest port 9100), with NETPEER
#             (the host tool tools/netpeer), PEER_READY, PEER_LOG, PEER_PID,
#             OUTDIR, TOP and BUILD in the environment. It writes
#             "<peer port> <guest port>" to PEER_READY once it listens and
#             is terminated when QEMU has exited, and the post script sees
#             PEER_LOG and PEER_READY
#   post      executable run after QEMU exits with DISK, CDIMG, SERIAL, EXITCODE,
#             TOP and BUILD in the environment (optional)
#   diskboot  boots the case's disk instead of the CD (optional), whose
#             boot loader the firmware loads
#   boot2     boots a second time, without the CD, when the first boot
#             passed (optional). The firmware loads the boot loader of the
#             case's disk, or of the mfs2 disk alone when boot2 contains
#             the word disk2. expect2 (or expect2.ARCH) contains the patterns
#             of the second serial log, serial2.txt, which must not
#             contain TEST FAIL either
#   stop      an extended regex (optional): the first boot ends as soon
#             as its serial log matches it, for a system that does not
#             power off by itself, and stop2 does the same for the second
KERNEL="$1"
BUILD="$2"
CASE="$3"
NAME="$(basename "$CASE")"
TOP="$(cd "$(dirname "$0")/.." && pwd)"
QEMU="${QEMU:-qemu-system-${ARCH:-x86_64}}"
XORRISO="${XORRISO:-xorriso}"
OUTDIR="$BUILD/$NAME"
if [ -f "$CASE/arches" ] && ! grep -qx "${ARCH:-x86_64}" "$CASE/arches"; then
    echo "SKIP $NAME (runs on $(tr '\n' ' ' < "$CASE/arches" | sed 's/ *$//') only)"
    exit 2
fi
mkdir -p "$OUTDIR"
CMDLINE=""
[ -f "$CASE/cmdline" ] && CMDLINE="$(cat "$CASE/cmdline")"
TIMEOUT=30
[ -f "$CASE/timeout" ] && TIMEOUT="$(cat "$CASE/timeout")"
MEM=512
[ -f "$CASE/mem" ] && MEM="$(cat "$CASE/mem")"
[ -f "$CASE/mem.${ARCH:-x86_64}" ] && MEM="$(cat "$CASE/mem.${ARCH:-x86_64}")"
CPUS="${CPUS:-4}"
[ -f "$CASE/cpus" ] && CPUS="$(cat "$CASE/cpus")"
SERIAL="$OUTDIR/serial.txt"
ISO="$OUTDIR/test.iso"
PEERPID=""
PEER_READY="$OUTDIR/peer.ready"
PEER_LOG="$OUTDIR/peer.log"
PEER_PID="$OUTDIR/peer.pid"

# The peer of a network case is stopped whenever this script ends, after
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
# Every case gets a private copy of the disk image, which prevents writes from
# affecting other cases. A case may provide its own image as <case>/disk.img.
# The copy is a copy-on-write clone where the file system supports it
# (cp -c on APFS) and is deleted when the case ends, and a run therefore
# never stores more than one image per running case.
clone() {
    cp -c "$1" "$2" 2>/dev/null || cp "$1" "$2"
}
cleanup() {
    stop_peer
    rm -f "$OUTDIR/disk.img" "$OUTDIR/swap.img" "$OUTDIR/disk2.img" "$OUTDIR"/fat*.img "$OUTDIR/test.iso" "$OUTDIR"/cd*.iso
    [ -n "$QMPSOCK" ] && rm -f "$QMPSOCK"
}
trap cleanup EXIT

# The network device and its peer are prepared before the image, which
# lets a peer that fails to start cost nothing else. The backend named in
# the nic file must be one this QEMU offers.
NETFLAGS=""
if [ -f "$CASE/nic" ]; then
    NIC="$(cat "$CASE/nic")"
    case "$NIC" in
        none) ;;
        dgram|user)
            "$QEMU" -M none -netdev help 2>/dev/null | grep -qx "$NIC" || fail "nic backend $NIC not offered by $QEMU"
            ;;
        *) fail "unknown nic backend $NIC" ;;
    esac
    if [ "$NIC" = dgram ] || { [ "$NIC" = user ] && [ -x "$CASE/peer" ]; }; then
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
        if [ "$NIC" = dgram ]; then
            NETFLAGS="-netdev dgram,id=net0,local.type=inet,local.host=127.0.0.1,local.port=$GUESTPORT,remote.type=inet,remote.host=127.0.0.1,remote.port=$PEERPORT"
        else
            NETFLAGS="-netdev user,id=net0"
            # A nonzero guest port forwards that host port to guest port
            # 9100, the xfer(1) server, which lets the peer reach the guest.
            [ "$GUESTPORT" != 0 ] && NETFLAGS="$NETFLAGS,hostfwd=tcp:127.0.0.1:$GUESTPORT-:9100"
            CMDLINE="$CMDLINE netpeer_port=$PEERPORT"
        fi
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
LATEFLAGS=""
if [ -x "$CASE/mkdisk" ]; then
    rm -f "$OUTDIR/disk2.img"
    DISK="$DISK" OUT="$OUTDIR/disk.img" OUT2="$OUTDIR/disk2.img" MKGPT="$MKGPT" MKFAT="$MKFAT" MKFS="$MKFS" ARCH="${ARCH:-x86_64}" \
        CASE="$CASE" TOP="$TOP" "$CASE/mkdisk" > "$OUTDIR/mkdisk.log" 2>&1 || fail "mkdisk, see $OUTDIR/mkdisk.log"
elif [ -f "$CASE/disk.img" ]; then
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
# The USB controller and its devices. The controller is named usb0, and
# the devices are attached to its bus usb0.0.
USBFLAGS=""
if [ -f "$CASE/usb" ]; then
    for word in $(cat "$CASE/usb"); do
        if [ -z "$USBFLAGS" ]; then
            USBFLAGS="-device $word,id=usb0"
        else
            case "$word" in
                *@*) USBFLAGS="$USBFLAGS -device ${word%@*},bus=usb0.0,port=${word#*@}" ;;
                *) USBFLAGS="$USBFLAGS -device $word,bus=usb0.0" ;;
            esac
        fi
    done
fi
# The QMP socket of a case with a qmp script. A Unix socket path has at
# most 104 bytes on macOS, and a longer output directory uses /tmp.
QMPFLAGS=""
QMPSOCK=""
if [ -f "$CASE/qmp" ]; then
    QMPSOCK="$OUTDIR/qmp.sock"
    [ "${#QMPSOCK}" -ge 100 ] && QMPSOCK="/tmp/minios-qmp-$$.sock"
    rm -f "$QMPSOCK"
    QMPFLAGS="-qmp unix:$QMPSOCK,server=on,wait=off"
fi
[ -f "$CASE/tablet" ] && VGAFLAGS="$VGAFLAGS -device virtio-tablet-pci"
[ -f "$CASE/keyboard" ] && VGAFLAGS="$VGAFLAGS -device virtio-keyboard-pci"
if [ -f "$OUTDIR/disk.img" ]; then
    DISKIF=virtio
    [ -f "$CASE/diskif" ] && DISKIF="$(cat "$CASE/diskif")"
    case "$DISKIF" in
        virtio) ROOTDEV="-device virtio-blk-pci,drive=vd0" ;;
        nvme*) ROOTDEV="-device $DISKIF,drive=vd0,serial=minios-root" ;;
        ahci)
            if [ "${ARCH:-x86_64}" = x86_64 ]; then
                ROOTDEV="-device ide-hd,drive=vd0,bus=ide.0"
            else
                ROOTDEV="-device ich9-ahci,id=ahci -device ide-hd,drive=vd0,bus=ahci.0"
            fi
            ;;
        usb) ROOTDEV="-device qemu-xhci,id=usbdisk -device usb-storage,drive=vd0,bus=usbdisk.0" ;;
        usb@*)
            ROOTDEV=""
            LATEFLAGS="$LATEFLAGS -device usb-storage,drive=vd0,bus=usb0.0,port=${DISKIF#usb@}"
            ;;
        *) fail "unknown diskif $DISKIF" ;;
    esac
    DISKFLAGS="-drive file=$OUTDIR/disk.img,if=none,id=vd0,format=raw $ROOTDEV"
fi
CDIMG=""
if [ -f "$CASE/cd" ]; then
    NCD=0
    NAHCI=0
    NUSB=0
    while read -r w1 w2 w3 w4; do
        [ -z "$w1" ] && continue
        if [ "$w1" = usb ]; then
            kind="$w2" dir="$w3" volid="$w4"
            [ "$NUSB" = 0 ] && DISKFLAGS="$DISKFLAGS -device qemu-xhci,id=usbcd"
            CDDEV="-device usb-bot,id=bot$NCD,bus=usbcd.0 -device scsi-cd,bus=bot$NCD.0"
            NUSB=$((NUSB + 1))
        else
            kind="$w1" dir="$w2" volid="$w3"
            if [ "${ARCH:-x86_64}" = x86_64 ]; then
                CDBUS="ide.$((NAHCI + 3))"
            else
                [ "$NAHCI" = 0 ] && DISKFLAGS="$DISKFLAGS -device ich9-ahci,id=cdahci"
                CDBUS="cdahci.$NAHCI"
            fi
            CDDEV="-device ide-cd,bus=$CDBUS"
            NAHCI=$((NAHCI + 1))
        fi
        case "$kind" in
            empty) DISKFLAGS="$DISKFLAGS $CDDEV" ;;
            iso)
                IMG="$OUTDIR/cd$NCD.iso"
                "$XORRISO" -as mkisofs -R -V "${volid:-MINIOS_TEST}" -o "$IMG" "$CASE/$dir" > "$OUTDIR/cd$NCD.log" 2>&1 ||
                    fail "cd image, see $OUTDIR/cd$NCD.log"
                [ -z "$CDIMG" ] && CDIMG="$IMG"
                DISKFLAGS="$DISKFLAGS -drive file=$IMG,if=none,id=cd$NCD,media=cdrom,readonly=on $CDDEV,drive=cd$NCD"
                ;;
            *) fail "unknown cd kind $kind" ;;
        esac
        NCD=$((NCD + 1))
    done < "$CASE/cd"
fi
# Devices on the controller of the usb file follow it on the command line.
USBFLAGS="$USBFLAGS $LATEFLAGS"
NDISK=1
if [ -f "$CASE/swap" ]; then
    dd if=/dev/zero of="$OUTDIR/swap.img" bs=1048576 count="$(cat "$CASE/swap")" status=none
    DISKFLAGS="$DISKFLAGS -drive file=$OUTDIR/swap.img,if=none,id=vd$NDISK,format=raw -device virtio-blk-pci,drive=vd$NDISK"
    NDISK=$((NDISK + 1))
fi
DISK2=""
if [ -x "$CASE/mkdisk" ] && [ -f "$OUTDIR/disk2.img" ]; then
    DISK2="$OUTDIR/disk2.img"
    DISKFLAGS="$DISKFLAGS -drive file=$DISK2,if=none,id=vd$NDISK,format=raw -device virtio-blk-pci,drive=vd$NDISK"
    NDISK=$((NDISK + 1))
elif [ -f "$CASE/mfs2" ]; then
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
# behavior, and ACCEL can explicitly select another accelerator.
if [ -z "$ACCEL" ]; then
    ACCELS="$("$QEMU" -accel help 2>/dev/null)"
    if [ "$(uname -s)" = Darwin ] && echo "$ACCELS" | grep -q '^hvf$'; then
        ACCEL=hvf
    else
        ACCEL=tcg
    fi
fi
RNGFLAGS="-object rng-random,id=rng0,filename=/dev/urandom -device virtio-rng-pci,rng=rng0,disable-legacy=on"
[ -f "$CASE/rng-zero" ] && RNGFLAGS="-object rng-random,id=rng0,filename=/dev/zero -device virtio-rng-pci,rng=rng0,disable-legacy=on"
[ -f "$CASE/no-rng" ] && RNGFLAGS=""
# The machine of the architecture. x86_64 is the q35 PC with the
# isa-debug-exit device for the exit status. aarch64 is virt with the edk2
# UEFI firmware that QEMU installs (EDK2_AARCH64 overrides its path). The
# image is a CD on a SCSI controller, and the guest ends a test with a PSCI
# power off, which reports no exit status.
case "${ARCH:-x86_64}" in
    x86_64)
        MACHINE="-M q35 -device isa-debug-exit,iobase=0xf4,iosize=0x4"
        # The CD first: a GPT disk carries a protective MBR with a boot
        # signature, which the BIOS would otherwise try to boot.
        BOOTFLAGS="-cdrom $ISO -boot order=d"
        BOOTFLAGS2="-boot order=c"
        ;;
    aarch64)
        EDK2_AARCH64="${EDK2_AARCH64:-$(dirname "$(command -v "$QEMU")")/../share/qemu/edk2-aarch64-code.fd}"
        [ -f "$EDK2_AARCH64" ] || fail "no edk2 firmware at $EDK2_AARCH64"
        CPU=max
        [ "$ACCEL" = hvf ] && CPU=host
        GIC=3
        [ -f "$CASE/gic" ] && GIC="$(cat "$CASE/gic")"
        ACPIOPT=",acpi=off"
        [ -f "$CASE/acpi" ] && ACPIOPT=""
        # A boot menu wait of 0 ms replaces the five second TianoCore screen.
        MACHINE="-M virt,gic-version=$GIC$ACPIOPT -cpu $CPU -bios $EDK2_AARCH64 -boot menu=on,splash-time=0"
        BOOTFLAGS="-drive file=$ISO,if=none,id=cd0,media=cdrom,readonly=on -device virtio-scsi-pci -device scsi-cd,drive=cd0"
        BOOTFLAGS2=""
        # virt has no VGA. ramfb is the boot framebuffer, like std VGA on
        # the PC. virtio-vga is a boot framebuffer and a virtio GPU. edk2
        # sets up no framebuffer on virtio-gpu-pci, and ramfb is added to it.
        # virt adds a virtio-net device unless -nic none is given. A case
        # without a nic file gets no network device, like the PC, whose
        # e1000 has no driver.
        [ -f "$CASE/nic" ] || NETFLAGS="-nic none"
        VGAFLAGS="-device ramfb"
        [ -f "$CASE/vga" ] && [ "$(cat "$CASE/vga")" = virtio ] && VGAFLAGS="$VGAFLAGS -device virtio-gpu-pci"
        [ -f "$CASE/tablet" ] && VGAFLAGS="$VGAFLAGS -device virtio-tablet-pci"
        [ -f "$CASE/keyboard" ] && VGAFLAGS="$VGAFLAGS -device virtio-keyboard-pci"
        ;;
    *) fail "unknown ARCH ${ARCH}" ;;
esac
# run_qemu SERIAL BOOTFLAGS DISKFLAGS [STOP] boots the machine once and
# waits for it to power off, or until the serial log matches the extended
# regex in the file STOP.
run_qemu() {
    "$QEMU" $MACHINE -m "${MEM}M" -smp "$CPUS" -accel "$ACCEL" -display none -no-reboot \
        -serial "file:$1" \
        $3 $SOUNDFLAGS $VGAFLAGS $USBFLAGS $NETFLAGS $RNGFLAGS $QMPFLAGS \
        $2 >"$OUTDIR/qemu.log" 2>&1 &
    QPID=$!
    QMPPID=""
    if [ -n "$QMPSOCK" ]; then
        python3 "$TOP/tests/qmp_input.py" "$QMPSOCK" "$1" "$CASE/qmp" > "$OUTDIR/qmp.log" 2>&1 &
        QMPPID=$!
    fi
    ELAPSED=0
    while kill -0 $QPID 2>/dev/null; do
        if [ -n "$4" ] && [ -f "$4" ] && grep -E -q -- "$(cat "$4")" "$1" 2>/dev/null; then
            kill $QPID 2>/dev/null
            wait $QPID 2>/dev/null
            [ -n "$QMPPID" ] && kill "$QMPPID" 2>/dev/null
            return 0
        fi
        if [ "$ELAPSED" -ge "$TIMEOUT" ]; then
            kill $QPID 2>/dev/null
            wait $QPID 2>/dev/null
            [ -n "$QMPPID" ] && kill "$QMPPID" 2>/dev/null
            stop_peer
            echo "FAIL $NAME (timeout after ${TIMEOUT}s, log: $1)"
            exit 1
        fi
        sleep 1
        ELAPSED=$((ELAPSED + 1))
    done
    wait $QPID
    if [ -n "$QMPPID" ]; then
        kill "$QMPPID" 2>/dev/null
        wait "$QMPPID" 2>/dev/null
    fi
}
# check_log SERIAL EXPECT fails the case on TEST FAIL in the log or on a
# pattern of EXPECT that it lacks.
check_log() {
    if grep -q "TEST FAIL" "$1"; then
        echo "FAIL $NAME: $(grep -m1 'TEST FAIL' "$1")"
        STATUS=1
    fi
    if [ -f "$2" ]; then
        while IFS= read -r pat; do
            [ -z "$pat" ] && continue
            if ! grep -E -q -- "$pat" "$1"; then
                echo "FAIL $NAME: missing /$pat/"
                STATUS=1
            fi
        done < "$2"
    fi
}
[ -f "$CASE/diskboot" ] && BOOTFLAGS="$BOOTFLAGS2"
run_qemu "$SERIAL" "$BOOTFLAGS" "$DISKFLAGS" "$CASE/stop"
echo "$?" > "$OUTDIR/exitcode"
# The peer's log is complete once it has been stopped, and the checks
# below and the post script read it afterwards.
stop_peer

STATUS=0
touch "$SERIAL"
# expect.$ARCH replaces expect where the output names architecture state.
EXPECT="$CASE/expect"
[ -f "$CASE/expect.${ARCH:-x86_64}" ] && EXPECT="$CASE/expect.${ARCH:-x86_64}"
check_log "$SERIAL" "$EXPECT"
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
    if ! DISK="$OUTDIR/disk.img" DISK2="$DISK2" FATIMG="$FATIMG" FATIMGS="$FATIMGS" CDIMG="$CDIMG" SERIAL="$SERIAL" \
         PEER_LOG="$PEER_LOG" PEER_READY="$PEER_READY" CAPTURE="$OUTDIR/capture.pcap" \
         EXITCODE="$(cat "$OUTDIR/exitcode")" TOP="$TOP" BUILD="$(dirname "$BUILD")" "$CASE/post"; then
        echo "FAIL $NAME: post check failed"
        STATUS=1
    fi
fi
if [ -f "$CASE/boot2" ] && [ "$STATUS" -eq 0 ]; then
    SERIAL2="$OUTDIR/serial2.txt"
    rm -f "$SERIAL2"
    DISKFLAGS2="$DISKFLAGS"
    if grep -qw disk2 "$CASE/boot2" && [ -n "$DISK2" ]; then
        DISKFLAGS2="-drive file=$DISK2,if=none,id=vd0,format=raw -device virtio-blk-pci,drive=vd0"
    fi
    run_qemu "$SERIAL2" "$BOOTFLAGS2" "$DISKFLAGS2" "$CASE/stop2"
    touch "$SERIAL2"
    EXPECT2="$CASE/expect2"
    [ -f "$CASE/expect2.${ARCH:-x86_64}" ] && EXPECT2="$CASE/expect2.${ARCH:-x86_64}"
    check_log "$SERIAL2" "$EXPECT2"
    [ "$STATUS" -ne 0 ] && echo "  second serial log: $SERIAL2"
fi
[ "$STATUS" -eq 0 ] && echo "PASS $NAME"
[ "$STATUS" -ne 0 ] && echo "  serial log: $SERIAL"
exit $STATUS
