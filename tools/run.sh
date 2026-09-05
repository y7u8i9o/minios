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
#   -a, --audio BACKEND    QEMU_AUDIO        audio backend for virtio-snd
#                                            (coreaudio, none, wav, pa, alsa,
#                                            pipewire, sdl, dbus, ...);
#                                            default coreaudio on macOS, none
#                                            elsewhere
#       --audio-opts OPTS  QEMU_AUDIO_OPTS   extra -audiodev properties, for
#                                            example out.frequency=48000
#       --wav FILE         QEMU_WAV          output file of the wav backend
#                                            (default build/audio.wav)
#       --no-sound         QEMU_SOUND=0      do not attach virtio-snd at all
#   -m, --mem SIZE         QEMU_MEM          guest memory (default 512M)
#   -s, --smp N            QEMU_SMP          number of CPUs (default 4)
#       --accel NAME       QEMU_ACCEL        hvf, tcg, kvm (default: hvf on
#                                            macOS when offered, else tcg;
#                                            select kvm explicitly on Linux)
#   -d, --display SPEC     QEMU_DISPLAY      -display argument (default: the
#                                            QEMU default window)
#   -f, --full-screen      QEMU_FULLSCREEN=1 full screen, guest scaled to fit
#       --vga TYPE         QEMU_VGA          virtio (default: virtio-vga, the
#                                            kernel's virtio-gpu driver sets any
#                                            mode up to 2560x1600 at run time),
#                                            std (VGA BIOS modes only) or none
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
#                                            primary screen's size, @2 when it
#                                            is 150 dpi or more or GDK_SCALE is
#                                            2, in a gtk window with
#                                            zoom-to-fit; elsewhere the image
#                                            default, 1024x768. With --vga std only the
#                                            VGA BIOS modes work (1600x1200,
#                                            1920x1080, 1920x1200, 2560x1440,
#                                            2560x1600, ...).
#       --serial SPEC      QEMU_SERIAL       -serial argument (default stdio)
#       --iso FILE         ISO               boot image (default build/minios.iso)
#       --disk FILE        DISK              root image (default build/disk.img)
#       --swap FILE        SWAP              swap image (default build/swap.img)
#       --qemu BINARY      QEMU              qemu-system-x86_64 to run
#       --extra ARGS       QEMU_EXTRA        arguments appended to the command
#   -g, --gdb                                start halted with the gdbstub
#                                            on port 1234 (-s -S)
#   -B, --build                              run `make image` first (with
#                                            VIDEO=$QEMU_VIDEO)
#   -n, --dry-run                            print the command, do not run it
#   -v, --verbose                            print the command before running
#   -c, --config FILE      QEMU_CONF         configuration file to read
#   -h, --help
#
# Arguments after `--` are passed to QEMU unchanged.
set -e

TOP="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$TOP/build"

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
VARS="QEMU QEMU_AUDIO QEMU_AUDIO_OPTS QEMU_WAV QEMU_SOUND QEMU_MEM QEMU_SMP \
      QEMU_ACCEL QEMU_DISPLAY QEMU_FULLSCREEN QEMU_VGA QEMU_TABLET QEMU_VIDEO \
      QEMU_SERIAL QEMU_EXTRA ISO DISK SWAP"

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
QEMU="${QEMU:-qemu-system-x86_64}"
ISO="${ISO:-$BUILD/minios.iso}"
DISK="${DISK:-$BUILD/disk.img}"
SWAP="${SWAP:-$BUILD/swap.img}"
QEMU_MEM="${QEMU_MEM:-512M}"
QEMU_SMP="${QEMU_SMP:-4}"
QEMU_SERIAL="${QEMU_SERIAL:-stdio}"
QEMU_SOUND="${QEMU_SOUND:-1}"
QEMU_VGA="${QEMU_VGA:-virtio}"
QEMU_TABLET="${QEMU_TABLET:-1}"
QEMU_WAV="${QEMU_WAV:-$BUILD/audio.wav}"
if [ -z "$QEMU_AUDIO" ]; then
    case "$(uname -s)" in
        Darwin) QEMU_AUDIO=coreaudio ;;
        *)      QEMU_AUDIO=none ;;
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
        --accel)          QEMU_ACCEL="$2"; shift ;;
        --accel=*)        QEMU_ACCEL="${1#*=}" ;;
        -d|--display)     QEMU_DISPLAY="$2"; shift ;;
        --display=*)      QEMU_DISPLAY="${1#*=}" ;;
        -f|--full-screen) QEMU_FULLSCREEN=1 ;;
        --vga)            QEMU_VGA="$2"; shift ;;
        --vga=*)          QEMU_VGA="${1#*=}" ;;
        --no-tablet)      QEMU_TABLET=0 ;;
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
    else
        QEMU_ACCEL=tcg
    fi
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
# doubled mode with @2 keeps the desktop at its size and makes it sharp.
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
    command -v xrandr >/dev/null 2>&1 || return 1
    out="$(xrandr --current 2>/dev/null)" || return 1
    line="$(echo "$out" | grep ' connected primary')"
    [ -n "$line" ] || line="$(echo "$out" | grep ' connected' | head -1)"
    [ -n "$line" ] || return 1
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
    }'
}
if [ -z "${QEMU_VIDEO+set}" ]; then
    if hidpi_display; then
        QEMU_VIDEO=2560x1600@2
    elif screen="$(linux_screen)"; then
        set -- $screen "$@"
        w=$1; h=$2; dpi=$3; shift 3
        scale=1
        if [ "${GDK_SCALE:-1}" -ge 2 ] || [ "$dpi" -ge 150 ]; then
            scale=2
        fi
        # The frame must fit the 16 MiB virtio-gpu buffer.
        if [ $((w * h * 4)) -le $((16 * 1024 * 1024)) ]; then
            QEMU_VIDEO="${w}x${h}@${scale}"
            # A window of the screen's size does not fit next to panels;
            # let gtk scale it to the space it gets.
            [ -z "$QEMU_DISPLAY" ] && QEMU_DISPLAY=gtk,zoom-to-fit=on
        fi
    fi
fi

if [ "$DO_BUILD" = 1 ]; then
    if [ -n "$QEMU_VIDEO" ]; then
        "${MAKE:-make}" -C "$TOP" image "VIDEO=$QEMU_VIDEO"
    else
        "${MAKE:-make}" -C "$TOP" image
    fi
elif [ -n "$QEMU_VIDEO" ] && [ "$VERBOSE" = 1 ]; then
    echo "run.sh: --video $QEMU_VIDEO applies when the image is built (make run or --build)" >&2
fi
if [ "$DRY_RUN" = 0 ]; then
    [ -f "$ISO" ]  || die "$ISO not found, run 'make image' or pass --build"
    [ -f "$DISK" ] || die "$DISK not found, run 'make image' or pass --build"
    [ -f "$SWAP" ] || die "$SWAP not found, run 'make image' or pass --build"
fi

# --- command ------------------------------------------------------------
set -- "$@"     # the arguments after `--`
set -- -M q35 -accel "$QEMU_ACCEL" -m "$QEMU_MEM" -smp "$QEMU_SMP" \
       -serial "$QEMU_SERIAL" -no-reboot -vga "$QEMU_VGA" \
       -drive "file=$DISK,if=none,id=vd0,format=raw" -device virtio-blk-pci,drive=vd0 \
       -drive "file=$SWAP,if=none,id=vd1,format=raw" -device virtio-blk-pci,drive=vd1 \
       "$@"
[ "$QEMU_TABLET" != 0 ] && set -- "$@" -device virtio-tablet-pci
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
set -- "$@" -cdrom "$ISO"
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
