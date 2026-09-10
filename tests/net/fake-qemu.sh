#!/bin/sh
# A stand-in for qemu-system-x86_64 used by the harness self test. It
# takes the arguments the harness builds, finds the serial log among them
# and behaves as FAKE_QEMU_BEHAVIOUR says: "pass" writes TEST PASS to the
# serial log and exits, "hang" sleeps until it is killed (the timeout
# path). It records the arguments in FAKE_QEMU_ARGS when set.
# The harness asks QEMU what it offers before starting it.
if [ "$2" = help ]; then
    case "$1" in
        -netdev) printf 'Available netdev backend types:\nsocket\nstream\ndgram\nuser\n' ;;
        -accel)  printf 'Accelerators supported in QEMU binary:\ntcg\n' ;;
        *)       printf '\n' ;;
    esac
    exit 0
fi
SERIAL=""
prev=""
for a in "$@"; do
    case "$prev" in
        -serial) SERIAL="${a#file:}" ;;
    esac
    prev="$a"
done
[ -n "$FAKE_QEMU_ARGS" ] && printf '%s\n' "$@" > "$FAKE_QEMU_ARGS"
case "${FAKE_QEMU_BEHAVIOUR:-pass}" in
    pass)
        [ -n "$SERIAL" ] && printf 'minios booting\nTEST PASS\n' > "$SERIAL"
        exit 0
        ;;
    hang)
        [ -n "$SERIAL" ] && printf 'minios booting\n' > "$SERIAL"
        while :; do sleep 1; done
        ;;
esac
exit 3
