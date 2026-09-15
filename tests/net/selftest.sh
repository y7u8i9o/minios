#!/bin/sh
# Self test of the network peer lifecycle of tests/run_qemu_test.sh, run
# by `make check-net` without booting anything: a fake QEMU stands in for
# the real one. Checks that the harness starts the peer, waits for its
# readiness, passes the ports to QEMU, stops the peer when QEMU exits, and
# that after a timeout no peer process and no port reservation remain.
#
# usage: NETPEER=<tool> selftest.sh <build dir>
set -u
BUILD="${1:?build dir}"
DIR="$(cd "$(dirname "$0")" && pwd)"
TOP="$(cd "$DIR/../.." && pwd)"
NETPEER="${NETPEER:-$BUILD/host/netpeer}"
OUT="$BUILD/tests/net-selftest"
rm -rf "$OUT"
mkdir -p "$OUT/cases"
status=0
fail() { echo "FAIL net-selftest: $*"; status=1; }

make_case() {
    # make_case <name> <timeout>
    mkdir -p "$OUT/cases/$1"
    echo "test=boot" > "$OUT/cases/$1/cmdline"
    echo "TEST PASS" > "$OUT/cases/$1/expect"
    echo dgram > "$OUT/cases/$1/nic"
    echo "$2" > "$OUT/cases/$1/timeout"
    cp "$DIR/peer-count.sh" "$OUT/cases/$1/peer"
    chmod +x "$OUT/cases/$1/peer"
}

# The harness needs a kernel and an initrd only to build the ISO, which
# mkiso.sh does before QEMU starts; a fake mkiso is not available, so give
# it real files when they exist and otherwise skip the image (the fake
# QEMU ignores it).
KERNEL="$BUILD/kernel.elf"
[ -f "$KERNEL" ] || { echo "SKIP net-selftest: $KERNEL not built"; exit 0; }
export LIMINE="${LIMINE:-$BUILD/host/limine}"
export INITRD="${INITRD:-$BUILD/initrd.tar}"
export NETPEER
export QEMU="$DIR/fake-qemu.sh"

# 1. The success path: the peer starts, QEMU gets its ports, the peer is
#    stopped afterwards and wrote its summary.
make_case pass 10
FAKE_QEMU_BEHAVIOUR=pass FAKE_QEMU_ARGS="$OUT/pass-args.txt" \
    "$DIR/../run_qemu_test.sh" "$KERNEL" "$OUT/run" "$OUT/cases/pass" > "$OUT/pass.out" 2>&1
if ! grep -q '^PASS pass$' "$OUT/pass.out"; then
    fail "success path did not pass: $(cat "$OUT/pass.out")"
fi
if ! grep -q 'dgram,id=net0,local.type=inet,local.host=127.0.0.1,local.port=[0-9]*,remote.type=inet,remote.host=127.0.0.1,remote.port=[0-9]*' "$OUT/pass-args.txt"; then
    fail "QEMU did not receive the dgram backend: $(grep netdev "$OUT/pass-args.txt")"
fi
grep -q 'virtio-net-pci,netdev=net0' "$OUT/pass-args.txt" || fail "no virtio-net device on the QEMU command line"
grep -q 'filter-dump,id=dump0,netdev=net0,file=' "$OUT/pass-args.txt" || fail "no capture filter on the QEMU command line"
grep -q '^summary frames' "$OUT/run/pass/peer.log" || fail "peer summary missing from $OUT/run/pass/peer.log"
read -r PEERPORT GUESTPORT < "$OUT/run/pass/peer.ready"
grep -q "remote.port=$PEERPORT" "$OUT/pass-args.txt" || fail "peer port $PEERPORT not passed to QEMU"
grep -q "local.port=$GUESTPORT" "$OUT/pass-args.txt" || fail "guest port $GUESTPORT not passed to QEMU"
"$NETPEER" --probe "$PEERPORT" || fail "peer port $PEERPORT still bound after the success path"
[ -f "$OUT/run/pass/peer.pid" ] && fail "peer pid file left behind after the success path"

# 2. The timeout path: QEMU never exits, the harness kills it after the
#    timeout and must also stop the peer and free its port.
make_case hang 2
FAKE_QEMU_BEHAVIOUR=hang \
    "$DIR/../run_qemu_test.sh" "$KERNEL" "$OUT/run" "$OUT/cases/hang" > "$OUT/hang.out" 2>&1
grep -q 'FAIL hang (timeout' "$OUT/hang.out" || fail "timeout path did not report a timeout: $(cat "$OUT/hang.out")"
read -r PEERPORT GUESTPORT < "$OUT/run/hang/peer.ready"
# The peer is stopped synchronously by the harness before it exits; the
# pid file is removed by the peer itself on a clean stop.
if [ -f "$OUT/run/hang/peer.pid" ]; then
    pid="$(cat "$OUT/run/hang/peer.pid")"
    kill -0 "$pid" 2>/dev/null && fail "peer $pid still running after the timeout path"
fi
"$NETPEER" --probe "$PEERPORT" || fail "peer port $PEERPORT still bound after the timeout path"
grep -q '^summary frames' "$OUT/run/hang/peer.log" || fail "peer summary missing after the timeout path"
pgrep -f "netpeer --ready $OUT/" >/dev/null 2>&1 && fail "netpeer processes of this run still exist: $(pgrep -fl "netpeer --ready $OUT/")"

# 3. A case naming a backend the host QEMU does not offer fails before
#    starting anything.
mkdir -p "$OUT/cases/badnic"
echo "test=boot" > "$OUT/cases/badnic/cmdline"
echo nosuchbackend > "$OUT/cases/badnic/nic"
"$DIR/../run_qemu_test.sh" "$KERNEL" "$OUT/run" "$OUT/cases/badnic" > "$OUT/badnic.out" 2>&1
grep -q 'FAIL badnic (unknown nic backend' "$OUT/badnic.out" || fail "unknown backend not rejected: $(cat "$OUT/badnic.out")"

# 4. A user-backend peer uses the same lifecycle, including timeout cleanup.
for behavior in pass hang; do
    name="user-$behavior"
    make_case "$name" 2
    echo user > "$OUT/cases/$name/nic"
    FAKE_QEMU_BEHAVIOUR="$behavior" FAKE_QEMU_ARGS="$OUT/$name-args.txt" \
        "$DIR/../run_qemu_test.sh" "$KERNEL" "$OUT/run" "$OUT/cases/$name" \
        > "$OUT/$name.out" 2>&1
    if [ "$behavior" = pass ]; then
        grep -q "^PASS $name$" "$OUT/$name.out" || fail "user backend success path"
    else
        grep -q "FAIL $name (timeout" "$OUT/$name.out" || fail "user backend timeout path"
    fi
    grep -q 'user,id=net0' "$OUT/$name-args.txt" || fail "user backend not passed to QEMU"
    read -r PEERPORT GUESTPORT < "$OUT/run/$name/peer.ready"
    "$NETPEER" --probe "$PEERPORT" || fail "user peer port remains bound"
    [ -f "$OUT/run/$name/peer.pid" ] && fail "user peer pid remains after $behavior"
    grep -q '^summary frames' "$OUT/run/$name/peer.log" || fail "user peer summary missing"
done

# 5. Native TCP peers must also release a blocking accept on termination.
for behavior in pass hang; do
    name="tcp-$behavior"
    make_case "$name" 2
    echo user > "$OUT/cases/$name/nic"
    cp "$TOP/tests/cases/net_tcp_peer/peer" "$OUT/cases/$name/peer"
    FAKE_QEMU_BEHAVIOUR="$behavior" \
        "$DIR/../run_qemu_test.sh" "$KERNEL" "$OUT/run" "$OUT/cases/$name" \
        > "$OUT/$name.out" 2>&1
    if [ "$behavior" = pass ]; then
        grep -q "^PASS $name$" "$OUT/$name.out" || fail "TCP peer success path"
    else
        grep -q "FAIL $name (timeout" "$OUT/$name.out" || fail "TCP peer timeout path"
    fi
    read -r PEERPORT GUESTPORT < "$OUT/run/$name/peer.ready"
    "$NETPEER" --probe-tcp "$PEERPORT" || fail "TCP peer port remains bound"
    [ -f "$OUT/run/$name/peer.pid" ] && fail "TCP peer pid remains after $behavior"
    grep -q '^tcp summary connections' "$OUT/run/$name/peer.log" || fail "TCP summary missing"
done

[ "$status" -eq 0 ] && echo "PASS net-selftest"
exit $status
