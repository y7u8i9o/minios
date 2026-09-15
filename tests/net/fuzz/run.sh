#!/bin/sh
# Host fuzzing of the parsers and validators linked into the kernel
# (docs/design/network.md, N09). usage: run.sh [out dir]
# FUZZ_SECONDS bounds each seed, FUZZ_CC selects the compiler.
set -eu
TOP="$(cd "$(dirname "$0")/../../.." && pwd)"
OUT="${1:-$TOP/build/network-fuzz}"
mkdir -p "$OUT"
set --
if [ "$(uname -s)" = Darwin ]; then
    set -- -isysroot "$(xcrun --show-sdk-path)"
fi
# Homebrew LLVM is preferred over Apple clang: on macOS 26/27 betas the
# AddressSanitizer runtime of Apple clang recurses in its own startup and
# never reaches main. CC is not consulted because it usually names the
# project compiler.
if [ -z "${FUZZ_CC:-}" ]; then
    if [ -x /opt/homebrew/opt/llvm/bin/clang ]; then
        FUZZ_CC=/opt/homebrew/opt/llvm/bin/clang
    else
        FUZZ_CC=clang
    fi
fi
SOURCES="$TOP/tests/net/fuzz/fuzz.c $TOP/kernel/net/wire.c $TOP/kernel/net/checksum.c
         $TOP/kernel/ipc/socket_validate.c $TOP/kernel/lib/chacha.c"

build() {
    # build <sanitizers> [compiler flags]
    SAN="$1"
    shift
    # shellcheck disable=SC2086
    "$FUZZ_CC" "$@" -Wno-macro-redefined -std=c17 -O1 -g "-fsanitize=$SAN" \
        -fno-omit-frame-pointer -I"$TOP/kernel/include" $SOURCES -o "$OUT/fuzz"
}

# A sanitizer runtime that hangs before main would make the run look like
# fuzzing forever; probe a one second run with a bounded wait and fall
# back to UBSan alone when the address sanitizer does not start.
SANITIZERS=address,undefined
build "$SANITIZERS" "$@"
"$OUT/fuzz" 1 1 > "$OUT/probe.txt" 2>&1 &
PROBE=$!
i=0
while kill -0 "$PROBE" 2>/dev/null && [ "$i" -lt 20 ]; do
    sleep 1
    i=$((i + 1))
done
if kill -0 "$PROBE" 2>/dev/null; then
    kill "$PROBE" 2>/dev/null || true
    wait "$PROBE" 2>/dev/null || true
    echo "fuzz: address sanitizer of $FUZZ_CC does not start on this host, using undefined only"
    SANITIZERS=undefined
    build "$SANITIZERS" "$@"
else
    wait "$PROBE"
fi
echo "fuzz: $FUZZ_CC -fsanitize=$SANITIZERS"
for seed in 0x7090809 0xffffffff 0x123456789abcdef; do
    "$OUT/fuzz" "$seed" "${FUZZ_SECONDS:-30}"
done
