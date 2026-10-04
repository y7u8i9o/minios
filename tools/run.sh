#!/bin/sh
# Boot the built image under QEMU with a configurable machine and audio
# setup. Used by `make run` and `make gdb`, and usable directly:
#
#   tools/run.sh [options] [-- extra qemu arguments]
#
# Settings are taken, in increasing order of precedence, from the defaults
# below, from $TOP/qemu.conf (a shell fragment, see qemu.conf.example),
# from QEMU_* environment variables and from the command line. Every
# option has an environment variable of the same meaning:
#
# ARCH (x86_64 or aarch64, as make passes it) selects the machine and the
# QEMU binary qemu-system-$ARCH.
#
#   -a, --audio BACKEND    QEMU_AUDIO        audio backend for virtio-snd
#                                            (coreaudio, none, wav, pa, alsa,
#                                            pipewire, sdl, dbus, ...);
#                                            default coreaudio on macOS, on
#                                            Linux the first of pipewire, pa,
#                                            alsa and sdl that QEMU offers
#       --audio-opts OPTS  QEMU_AUDIO_OPTS   extra -audiodev properties, for
#                                            example out.frequency=48000
#       --wav FILE         QEMU_WAV          output file of the wav backend
#                                            (default build/audio.wav)
#       --no-sound         QEMU_SOUND=0      do not attach virtio-snd at all
#   -m, --mem SIZE         QEMU_MEM          guest memory (default 512M)
#   -s, --smp N            QEMU_SMP          number of CPUs (default 4)
#       --gic N            QEMU_GIC          GIC version of the aarch64 virt
#                                            machine, 2 or 3 (default 3), a
#                                            GICv2 serves at most 8 CPUs
#       --accel NAME       QEMU_ACCEL        hvf, tcg, kvm (default: hvf on
#                                            macOS and kvm on Linux when
#                                            offered and /dev/kvm is
#                                            writable, else tcg)
#   -d, --display SPEC     QEMU_DISPLAY      -display argument (default: sdl
#                                            on Linux when QEMU offers it,
#                                            else the QEMU default window)
#   -f, --full-screen      QEMU_FULLSCREEN=1 full screen, guest scaled to fit
#       --vga TYPE         QEMU_VGA          virtio (default: virtio-vga, the
#                                            kernel's virtio-gpu driver sets any
#                                            mode up to 2560x1600 at run time),
#                                            std (VGA BIOS modes only) or none
#       --no-keyboard      QEMU_KEYBOARD=0   no virtio keyboard; keys go to the PS/2 port.
#                                            On aarch64 a USB keyboard is added
#                                            before the virtio keyboard for the
#                                            firmware and the Limine menu
#       --nic BACKEND      QEMU_NIC          network backend of a virtio-net
#                                            (user forwards host port 9100 to
#                                            the guest for xfer(1))
#                                            device: none (default), user
#                                            (QEMU's user mode stack) or a
#                                            complete -netdev argument such as
#                                            "dgram,local.type=inet,..." (the
#                                            id net0 is added)
#       --no-tablet        QEMU_TABLET=0     no virtio tablet; the window grabs
#                                            the mouse and moves it relatively
#       --video MODE       QEMU_VIDEO        framebuffer mode WxH[xBPP][@SCALE]
#                                            put on the kernel command line
#                                            when the image is built (make run,
#                                            --build). @2 doubles every pixel
#                                            for high density displays. Default
#                                            on macOS with a Retina display and
#                                            the cocoa window: 2560x1600@2; on
#                                            Linux with an X display: the
#                                            largest mode within 90 percent of
#                                            the primary screen and the 16 MiB
#                                            framebuffer, @2 when the screen is
#                                            150 dpi or more, 3000 pixels wide
#                                            or more, or GDK_SCALE is 2;
#                                            elsewhere the image default,
#                                            1024x768. With --vga std only the
#                                            VGA BIOS modes work (1600x1200,
#                                            1920x1080, 1920x1200, 2560x1440,
#                                            2560x1600, ...).
#       --serial SPEC      QEMU_SERIAL       -serial argument (default stdio)
#       --iso FILE         ISO               boot image (default build/minios.iso)
#       --disk FILE        DISK              root image (default build/disk.img)
#       --swap FILE        SWAP              swap image (default build/swap.img)
#       --data FILE        DATA              data volume (default data.img in the repository)
#       --no-data          DATA=             boot without the data volume
#       --devdisk FILE     DEVDISK           boot the development disk FILE, an
#                                            installed system (make run), in
#                                            place of the CD with --disk and
#                                            --swap, with the data volume as vdc
#       --update FILE      UPDATE            the update medium attached as vdb
#                                            beside the development disk
#       --boot-kernel      RUN_BOOT=kernel   boot the kernel of the CD with the
#                                            development disk as the root
#       --qemu BINARY      QEMU              qemu-system-x86_64 to run
#       --extra ARGS       QEMU_EXTRA        arguments appended to the command
#   -g, --gdb                                start halted with the gdbstub
#                                            on port 1234 (-s -S)
#   -B, --build                              run `make image` first (with
#                                            VIDEO=$QEMU_VIDEO), or with
#                                            --devdisk `make devprep`, which
#                                            creates the development disk
#                                            once and writes the update medium
#   -n, --dry-run                            print the command, do not run it
#   -v, --verbose                            print the command before running
#   -c, --config FILE      QEMU_CONF         configuration file to read
#   -h, --help
#
# Arguments after `--` are passed to QEMU unchanged.
set -e

TOP="$(cd "$(dirname "$0")/.." && pwd)"
# ARCH selects the machine: the q35 PC for x86_64, virt for aarch64. make
# passes ARCH and BUILD.
ARCH="${ARCH:-x86_64}"
if [ "$ARCH" = x86_64 ]; then
    BUILD="${BUILD:-$TOP/build}"
else
    BUILD="${BUILD:-$TOP/build/$ARCH}"
fi

usage() {
    sed -n '2,/^set -e/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'
}

die() {
    echo "run.sh: $*" >&2
    exit 1
}

# --- configuration file ------------------------------------------------
# Environment variables must beat the file, so remember them, source the
# file, then put them back.
VARS="QEMU QEMU_AUDIO QEMU_AUDIO_OPTS QEMU_WAV QEMU_SOUND QEMU_MEM QEMU_SMP QEMU_GIC \
      QEMU_ACCEL QEMU_DISPLAY QEMU_FULLSCREEN QEMU_VGA QEMU_TABLET QEMU_KEYBOARD QEMU_NIC QEMU_VIDEO \
      QEMU_SERIAL QEMU_EXTRA ISO DISK SWAP DATA"

CONF="${QEMU_CONF:-$TOP/qemu.conf}"
# --config must be found before the file is read; other options are parsed
# afterwards so that they override it.
prev=""
for arg in "$@"; do
    case "$prev" in
        -c|--config) CONF="$arg" ;;
    esac
    case "$arg" in
        --config=*) CONF="${arg#--config=}" ;;
    esac
    prev="$arg"
done

if [ -f "$CONF" ]; then
    saved=""
    for v in $VARS; do
        if eval "[ -n \"\${$v+set}\" ]"; then
            saved="$saved $v"
            eval "env_$v=\"\$$v\""
        fi
    done
    # shellcheck disable=SC1090
    . "$CONF"
    for v in $saved; do
        eval "$v=\"\$env_$v\""
    done
elif [ -n "$QEMU_CONF" ] || [ "$CONF" != "$TOP/qemu.conf" ]; then
    die "configuration file not found: $CONF"
fi

# --- defaults -----------------------------------------------------------
QEMU="${QEMU:-qemu-system-$ARCH}"
ISO="${ISO:-$BUILD/minios.iso}"
DISK="${DISK:-$BUILD/disk.img}"
SWAP="${SWAP:-$BUILD/swap.img}"
DATA="${DATA-$TOP/data.img}"
QEMU_MEM="${QEMU_MEM:-512M}"
QEMU_SMP="${QEMU_SMP:-4}"
QEMU_GIC="${QEMU_GIC:-3}"
QEMU_SERIAL="${QEMU_SERIAL:-stdio}"
QEMU_SOUND="${QEMU_SOUND:-1}"
QEMU_VGA="${QEMU_VGA:-virtio}"
QEMU_TABLET="${QEMU_TABLET:-1}"
QEMU_KEYBOARD="${QEMU_KEYBOARD:-1}"
QEMU_NIC="${QEMU_NIC:-none}"
QEMU_WAV="${QEMU_WAV:-$BUILD/audio.wav}"
if [ -z "$QEMU_AUDIO" ]; then
    case "$(uname -s)" in
        Darwin) QEMU_AUDIO=coreaudio ;;
        *)
            # The first backend this QEMU offers, in order of preference.
            for b in pipewire pa alsa sdl; do
                if "$QEMU" -audiodev help 2>/dev/null | grep -qx "$b"; then
                    QEMU_AUDIO=$b
                    break
                fi
            done
            [ -n "$QEMU_AUDIO" ] || QEMU_AUDIO=none
            ;;
    esac
fi

# --- command line -------------------------------------------------------
GDB=0
DO_BUILD=0
DRY_RUN=0
VERBOSE=0
while [ $# -gt 0 ]; do
    case "$1" in
        -a|--audio)       QEMU_AUDIO="$2"; shift ;;
        --audio=*)        QEMU_AUDIO="${1#*=}" ;;
        --audio-opts)     QEMU_AUDIO_OPTS="$2"; shift ;;
        --audio-opts=*)   QEMU_AUDIO_OPTS="${1#*=}" ;;
        --wav)            QEMU_WAV="$2"; QEMU_AUDIO=wav; shift ;;
        --wav=*)          QEMU_WAV="${1#*=}"; QEMU_AUDIO=wav ;;
        --no-sound)       QEMU_SOUND=0 ;;
        -m|--mem)         QEMU_MEM="$2"; shift ;;
        --mem=*)          QEMU_MEM="${1#*=}" ;;
        -s|--smp)         QEMU_SMP="$2"; shift ;;
        --smp=*)          QEMU_SMP="${1#*=}" ;;
        --gic)            QEMU_GIC="$2"; shift ;;
        --gic=*)          QEMU_GIC="${1#*=}" ;;
        --accel)          QEMU_ACCEL="$2"; shift ;;
        --accel=*)        QEMU_ACCEL="${1#*=}" ;;
        -d|--display)     QEMU_DISPLAY="$2"; shift ;;
        --display=*)      QEMU_DISPLAY="${1#*=}" ;;
        -f|--full-screen) QEMU_FULLSCREEN=1 ;;
        --vga)            QEMU_VGA="$2"; shift ;;
        --vga=*)          QEMU_VGA="${1#*=}" ;;
        --no-tablet)      QEMU_TABLET=0 ;;
        --no-keyboard)    QEMU_KEYBOARD=0 ;;
        --nic)            QEMU_NIC="$2"; shift ;;
        --nic=*)          QEMU_NIC="${1#*=}" ;;
        --video)          QEMU_VIDEO="$2"; shift ;;
        --video=*)        QEMU_VIDEO="${1#*=}" ;;
        --serial)         QEMU_SERIAL="$2"; shift ;;
        --serial=*)       QEMU_SERIAL="${1#*=}" ;;
        --iso)            ISO="$2"; shift ;;
        --iso=*)          ISO="${1#*=}" ;;
        --disk)           DISK="$2"; shift ;;
        --disk=*)         DISK="${1#*=}" ;;
        --swap)           SWAP="$2"; shift ;;
        --swap=*)         SWAP="${1#*=}" ;;
        --data)           DATA="$2"; shift ;;
        --data=*)         DATA="${1#*=}" ;;
        --no-data)        DATA="" ;;
        --devdisk)        DEVDISK="$2"; shift ;;
        --update)         UPDATE="$2"; shift ;;
        --boot-kernel)    RUN_BOOT=kernel ;;
        --qemu)           QEMU="$2"; shift ;;
        --qemu=*)         QEMU="${1#*=}" ;;
        --extra)          QEMU_EXTRA="$QEMU_EXTRA $2"; shift ;;
        --extra=*)        QEMU_EXTRA="$QEMU_EXTRA ${1#*=}" ;;
        -c|--config)      shift ;;   # handled above
        --config=*)       ;;
        -g|--gdb)         GDB=1 ;;
        -B|--build)       DO_BUILD=1 ;;
        -n|--dry-run)     DRY_RUN=1 ;;
        -v|--verbose)     VERBOSE=1 ;;
        -h|--help)        usage; exit 0 ;;
        --)               shift; break ;;
        -*)               die "unknown option: $1 (see --help)" ;;
        *)                die "unexpected argument: $1 (use -- before QEMU arguments)" ;;
    esac
    shift
done

# --- checks -------------------------------------------------------------
command -v "$QEMU" >/dev/null 2>&1 || [ -x "$QEMU" ] || die "QEMU not found: $QEMU"

if [ -z "$QEMU_ACCEL" ]; then
    accels="$("$QEMU" -accel help 2>/dev/null)"
    if [ "$(uname -s)" = Darwin ] && echo "$accels" | grep -q '^hvf$'; then
        QEMU_ACCEL=hvf
    elif [ "$(uname -s)" = Linux ] && echo "$accels" | grep -q '^kvm$' && [ -w /dev/kvm ]; then
        QEMU_ACCEL=kvm
    else
        QEMU_ACCEL=tcg
    fi
fi

# Linux: the sdl window follows the guest resolution on every session
# type; the gtk window does not on a native Wayland session.
if [ -z "$QEMU_DISPLAY" ] && [ "$(uname -s)" = Linux ] &&
   "$QEMU" -display help 2>/dev/null | grep -qx sdl; then
    QEMU_DISPLAY=sdl
fi

if [ "$QEMU_SOUND" != 0 ]; then
    backends="$("$QEMU" -audiodev help 2>/dev/null | sed '1d')"
    if [ -n "$backends" ] && ! echo "$backends" | grep -q "^$QEMU_AUDIO\$"; then
        echo "run.sh: audio backend '$QEMU_AUDIO' is not available in $QEMU" >&2
        echo "run.sh: available backends: $(echo "$backends" | tr '\n' ' ')" >&2
        exit 1
    fi
fi

# --- video mode -----------------------------------------------------------
# QEMU's cocoa window maps one guest pixel to one screen pixel, so on a
# Retina display the default 1024x768 mode is a quarter of the screen. A
# doubled mode with @2 retains the desktop at its size and makes it sharp.
# Only the built image carries the mode; without --build it is a no-op.
hidpi_display() {
    [ "$(uname -s)" = Darwin ] || return 1
    case "${QEMU_DISPLAY:-cocoa}" in
        cocoa*) ;;
        *) return 1 ;;
    esac
    system_profiler SPDisplaysDataType 2>/dev/null | grep -q -i 'retina\|UI Looks like'
}
# Linux: the gtk and sdl windows show one guest pixel per screen pixel as
# well, so the mode follows the primary screen: its pixel size, doubled
# (@2) when the screen is high density (150 dpi or more, or GDK_SCALE=2).
# Prints "WIDTH HEIGHT DPI" from xrandr; fails without an X display.
linux_screen() {
    [ "$(uname -s)" = Linux ] || return 1
    case "${QEMU_DISPLAY:-gtk}" in
        gtk*|sdl*) ;;
        *) return 1 ;;
    esac
    if command -v xrandr >/dev/null 2>&1 && out="$(xrandr --current 2>/dev/null)"; then
        # set -e must not end the substitution when a grep finds nothing.
        line="$(echo "$out" | grep ' connected primary' || true)"
        [ -n "$line" ] || line="$(echo "$out" | grep ' connected' | head -1 || true)"
        if [ -n "$line" ]; then
            echo "$line" | awk '{
                for (i = 1; i <= NF; i++) {
                    if ($i ~ /^[0-9]+x[0-9]+\+[0-9]+\+[0-9]+$/) geom = $i
                    if ($i ~ /^[0-9]+mm$/ && mm == "") mm = $i
                }
                if (geom == "") exit 1
                split(geom, a, /[x+]/)
                sub(/mm/, "", mm)
                dpi = (mm + 0 > 0) ? int(a[1] * 25.4 / mm) : 96
                print a[1], a[2], dpi
            }' && return 0
        fi
    fi
    # Without xrandr (or with no output marked connected, as under some
    # XWayland versions) the root window size and its physical size.
    command -v xdpyinfo >/dev/null 2>&1 || return 1
    xdpyinfo 2>/dev/null | awk '/dimensions:/ {
        split($2, a, "x")
        mm = $3; gsub(/[()]/, "", mm); split(mm, m, "x")
        dpi = (m[1] + 0 > 0) ? int(a[1] * 25.4 / m[1]) : 96
        print a[1], a[2], dpi; found = 1; exit
    } END { exit !found }'
}
if [ -z "${QEMU_VIDEO+set}" ]; then
    if hidpi_display; then
        QEMU_VIDEO=2560x1600@2
    elif screen="$(linux_screen)"; then
        set -- $screen "$@"
        w=$1; h=$2; dpi=$3; shift 3
        scale=1
        # A screen reported 3000 pixels wide or more is a high density
        # screen even when no physical size is known: XWayland reports a
        # 2560x1440 panel with 150 percent scaling as 3840x2160, 0 mm.
        if [ "${GDK_SCALE:-1}" -ge 2 ] || [ "$dpi" -ge 150 ] || [ "$w" -ge 3000 ]; then
            scale=2
        fi
        # The gtk window follows the guest resolution (zoom-to-fit is off),
        # so the mode must leave room for panels and the title bar: at
        # most 90 percent of the screen in each direction. The frame must
        # also fit the 16 MiB virtio-gpu buffer. The size is reduced with
        # the aspect ratio preserved until both conditions are met.
        screen="${w}x${h}"
        max=$((16 * 1024 * 1024))
        limw=$((w * 9 / 10)); limh=$((h * 9 / 10))
        for r in "9 10" "4 5" "3 4" "2 3" "1 2" "1 3" "1 4"; do
            num=${r% *}; den=${r#* }
            mw=$((w * num / den / 8 * 8)); mh=$((h * num / den / 8 * 8))
            [ $((mw * mh * 4)) -le $max ] && [ $mw -le $limw ] && [ $mh -le $limh ] && break
        done
        if [ $((mw * mh * 4)) -le $max ]; then
            QEMU_VIDEO="${mw}x${mh}@${scale}"
            # On a native Wayland session a gtk window does not follow a
            # guest resolution change (QEMU issue 1876), and an sdl window
            # is sized in logical points, so a 2560x1440 guest fills a
            # 2560x1440 screen with 150 percent scaling. Through XWayland
            # both windows are sized in the pixels xrandr reported.
            if [ "${XDG_SESSION_TYPE:-}" = wayland ]; then
                [ -n "${GDK_BACKEND:-}" ] || export GDK_BACKEND=x11
                [ -n "${SDL_VIDEODRIVER:-}" ] || export SDL_VIDEODRIVER=x11
            fi
            echo "run.sh: primary screen $screen, $dpi dpi: video $QEMU_VIDEO, display ${QEMU_DISPLAY:-gtk}${SDL_VIDEODRIVER:+ through $SDL_VIDEODRIVER}" >&2
        else
            echo "run.sh: primary screen $screen exceeds the 16 MiB framebuffer, using the image default" >&2
        fi
    elif [ "$(uname -s)" = Linux ]; then
        echo "run.sh: no screen size from xrandr or xdpyinfo (session ${XDG_SESSION_TYPE:-unknown}, display ${QEMU_DISPLAY:-gtk}), using the image default video mode" >&2
    fi
fi

if [ "$DO_BUILD" = 1 ] && [ -n "$DEVDISK" ]; then
    # The video mode reaches the development disk through the update
    # medium on every run, where pkg-update writes it into the kernel
    # command line, and the CD of RUN_BOOT=kernel on every build.
    "${MAKE:-make}" -C "$TOP" devprep ${QEMU_VIDEO:+"VIDEO=$QEMU_VIDEO"} ${RUN_BOOT:+"BOOT=$RUN_BOOT"}
elif [ "$DO_BUILD" = 1 ]; then
    if [ -n "$QEMU_VIDEO" ]; then
        "${MAKE:-make}" -C "$TOP" image "VIDEO=$QEMU_VIDEO"
    else
        "${MAKE:-make}" -C "$TOP" image
    fi
elif [ -n "$QEMU_VIDEO" ] && [ "$VERBOSE" = 1 ]; then
    echo "run.sh: --video $QEMU_VIDEO applies when the image is built (make run or --build)" >&2
fi
if [ "$DRY_RUN" = 0 ] && [ -n "$DEVDISK" ]; then
    [ -f "$DEVDISK" ] || die "$DEVDISK not found, run 'make devdisk' or pass --build"
    [ "$RUN_BOOT" != kernel ] || [ -f "$ISO" ] || die "$ISO not found, run 'make devprep BOOT=kernel'"
    [ -z "$DATA" ] || [ -f "$DATA" ] || die "$DATA not found, run 'make image' or pass --build"
elif [ "$DRY_RUN" = 0 ]; then
    [ -f "$ISO" ]  || die "$ISO not found, run 'make image' or pass --build"
    [ -f "$DISK" ] || die "$DISK not found, run 'make image' or pass --build"
    [ -f "$SWAP" ] || die "$SWAP not found, run 'make image' or pass --build"
    [ -z "$DATA" ] || [ -f "$DATA" ] || die "$DATA not found, run 'make image' or pass --build"
fi

# --- command ------------------------------------------------------------
set -- "$@"     # the arguments after `--`
# The machine. virt has no VGA and no PS/2 devices. The edk2 firmware that
# QEMU installs boots the CD, and without ACPI it installs the device tree
# that the kernel reads. virtio-gpu-pci is the first display, so that the
# window shows it. edk2 sets up no framebuffer on it, so ramfb is the boot
# framebuffer. virt adds a virtio-net device unless -nic none is given.
case "$ARCH" in
    x86_64)
        set -- -M q35 -vga "$QEMU_VGA" "$@"
        BOOT="-cdrom"
        ;;
    aarch64)
        EDK2="${EDK2_AARCH64:-$(dirname "$(command -v "$QEMU")")/../share/qemu/edk2-aarch64-code.fd}"
        [ "$DRY_RUN" = 1 ] || [ -f "$EDK2" ] || die "no edk2 firmware at $EDK2, set EDK2_AARCH64"
        cpu=max
        [ "$QEMU_ACCEL" = hvf ] || [ "$QEMU_ACCEL" = kvm ] && cpu=host
        case "$QEMU_VGA" in
            virtio) gpu="-device virtio-gpu-pci -device ramfb" ;;
            none)   gpu="" ;;
            *)      gpu="-device ramfb" ;;
        esac
        # shellcheck disable=SC2086
        # A boot menu wait of 0 ms, which edk2 reads from etc/boot-menu-wait,
        # replaces the five second TianoCore screen of its boot manager.
        set -- -M "virt,gic-version=$QEMU_GIC,acpi=off" -cpu "$cpu" -bios "$EDK2" -boot menu=on,splash-time=0 \
            $gpu "$@"
        [ "$QEMU_NIC" = none ] || [ -z "$QEMU_NIC" ] && set -- "$@" -nic none
        BOOT="aarch64"
        ;;
    *) die "unknown ARCH $ARCH" ;;
esac
set -- "$@" -accel "$QEMU_ACCEL" -m "$QEMU_MEM" -smp "$QEMU_SMP" \
       -object rng-random,id=rng0,filename=/dev/urandom \
       -device virtio-rng-pci,rng=rng0,disable-legacy=on \
       -serial "$QEMU_SERIAL"
# A restart ends QEMU, except on the development disk, where pkg-update
# restarts the machine after it has upgraded the kernel, the boot loader
# or libc.
[ -z "$DEVDISK" ] && set -- "$@" -no-reboot
# The development disk replaces the root image and swap, which it contains
# itself, and the update medium takes the place of swap as vdb. The medium
# is writable, as a USB stick is, since mounting an mfs updates its
# superblock, and make writes it anew before every run. A medium that make
# has not built is replaced by an empty disk, and the data volume therefore
# remains vdc.
if [ -n "$DEVDISK" ]; then
    set -- "$@" -drive "file=$DEVDISK,if=none,id=vd0,format=raw" -device virtio-blk-pci,drive=vd0
    if [ -n "$UPDATE" ] && [ -f "$UPDATE" ]; then
        set -- "$@" -drive "file=$UPDATE,if=none,id=vd1,format=raw" -device virtio-blk-pci,drive=vd1
    else
        set -- "$@" -drive "driver=null-co,if=none,id=vd1" -device virtio-blk-pci,drive=vd1
    fi
else
    set -- "$@" -drive "file=$DISK,if=none,id=vd0,format=raw" -device virtio-blk-pci,drive=vd0 \
        -drive "file=$SWAP,if=none,id=vd1,format=raw" -device virtio-blk-pci,drive=vd1
fi
[ -n "$DATA" ] && set -- "$@" -drive "file=$DATA,if=none,id=vd2,format=raw" -device virtio-blk-pci,drive=vd2
[ "$QEMU_TABLET" != 0 ] && set -- "$@" -device virtio-tablet-pci
# The edk2 firmware of aarch64 has no virtio keyboard driver. A USB
# keyboard on its own xHCI controller gives the firmware and the Limine
# menu a keyboard, and the kernel drives it as well (docs/design/usb.md).
# It comes first, because QEMU sends the keys to the first keyboard until
# a guest driver activates another.
[ "$QEMU_KEYBOARD" != 0 ] && [ "$ARCH" = aarch64 ] && set -- "$@" -device qemu-xhci,id=fwkbd -device usb-kbd,bus=fwkbd.0
[ "$QEMU_KEYBOARD" != 0 ] && set -- "$@" -device virtio-keyboard-pci
case "$QEMU_NIC" in
    none|"") ;;
    user)    set -- "$@" -netdev user,id=net0,hostfwd=tcp:127.0.0.1:9100-:9100 -device virtio-net-pci,netdev=net0 ;;
    *)       set -- "$@" -netdev "$QEMU_NIC,id=net0" -device virtio-net-pci,netdev=net0 ;;
esac
if [ "$QEMU_SOUND" != 0 ]; then
    audiodev="$QEMU_AUDIO,id=minios_audio"
    [ "$QEMU_AUDIO" = wav ] && audiodev="$audiodev,path=$QEMU_WAV"
    [ -n "$QEMU_AUDIO_OPTS" ] && audiodev="$audiodev,$QEMU_AUDIO_OPTS"
    set -- "$@" -audiodev "$audiodev" -device virtio-sound-pci,audiodev=minios_audio
fi
if [ "$QEMU_FULLSCREEN" = 1 ]; then
    if [ -z "$QEMU_DISPLAY" ]; then
        case "$(uname -s)" in
            Darwin) QEMU_DISPLAY=cocoa ;;
            *)      QEMU_DISPLAY=gtk ;;
        esac
    fi
    QEMU_DISPLAY="$QEMU_DISPLAY,full-screen=on,zoom-to-fit=on"
fi
[ -n "$QEMU_DISPLAY" ] && set -- "$@" -display "$QEMU_DISPLAY"
[ "$GDB" = 1 ] && set -- "$@" -s -S
if [ -n "$DEVDISK" ] && [ "$RUN_BOOT" != kernel ]; then
    # The firmware loads the boot loader of the development disk.
    [ "$BOOT" = -cdrom ] && set -- "$@" -boot order=c
elif [ "$BOOT" = -cdrom ]; then
    set -- "$@" -cdrom "$ISO" -boot order=d
else
    set -- "$@" -drive "file=$ISO,if=none,id=cd0,media=cdrom,readonly=on" \
        -device virtio-scsi-pci -device scsi-cd,drive=cd0
fi
# QEMU_EXTRA is a string and is deliberately word split.
# shellcheck disable=SC2086
set -- "$@" $QEMU_EXTRA

if [ "$DRY_RUN" = 1 ] || [ "$VERBOSE" = 1 ]; then
    printf '%s' "$QEMU"
    for a in "$@"; do
        case "$a" in
            *[!A-Za-z0-9_./=:,+-]*) printf " '%s'" "$(printf '%s' "$a" | sed "s/'/'\\\\''/g")" ;;
            *)                      printf ' %s' "$a" ;;
        esac
    done
    echo
fi
[ "$DRY_RUN" = 1 ] && exit 0

if [ "$GDB" = 1 ]; then
    echo "Connect with: ${CROSS:-x86_64-elf-}gdb -iex 'set auto-load safe-path $TOP'" >&2
fi
exec "$QEMU" "$@"
