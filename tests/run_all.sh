#!/bin/sh
# usage: run_all.sh <kernel.elf> <build dir> <cases dir>
# Runs the cases in parallel (JOBS, default 4) and prints one PASS or
# FAIL line per case plus a summary.
KERNEL="$1"; BUILD="$2"; CASES="$3"
DIR="$(cd "$(dirname "$0")" && pwd)"
JOBS="${JOBS:-4}"
mkdir -p "$BUILD"
RESULTS="$BUILD/results.txt"
rm -f "$RESULTS"
export KERNEL BUILD DIR RESULTS
/bin/ls -d "$CASES"/*/ | sed 's|/$||' | xargs -P "$JOBS" -I{} sh -c '
    if "$DIR/run_qemu_test.sh" "$KERNEL" "$BUILD" "{}"; then echo pass >> "$RESULTS"; else echo fail >> "$RESULTS"; fi'
pass=$(grep -c pass "$RESULTS" 2>/dev/null); pass=${pass:-0}
fail=$(grep -c fail "$RESULTS" 2>/dev/null); fail=${fail:-0}
echo "tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
