#!/bin/sh
# usage: run_all.sh <kernel.elf> <build dir> <cases dir> [case...]
# Runs the cases in parallel (JOBS, default 4) and prints one PASS, FAIL or
# SKIP line per case plus a summary. A case skipped for the architecture
# (the arches file) counts neither as passed nor as failed. With case names only those cases
# run (the whole suite is too large to run for every change).
KERNEL="$1"; BUILD="$2"; CASES="$3"
shift 3
DIR="$(cd "$(dirname "$0")" && pwd)"
JOBS="${JOBS:-4}"
mkdir -p "$BUILD"
RESULTS="$BUILD/results.txt"
rm -f "$RESULTS"
export KERNEL BUILD DIR RESULTS
if [ $# -gt 0 ]; then
    for c in "$@"; do
        [ -d "$CASES/$c" ] || { echo "run_all.sh: no case $c" >&2; exit 1; }
        echo "$CASES/$c"
    done
else
    /bin/ls -d "$CASES"/*/ | sed 's|/$||'
fi | xargs -P "$JOBS" -I{} sh -c '
    "$DIR/run_qemu_test.sh" "$KERNEL" "$BUILD" "{}"; r=$?
    if [ $r -eq 0 ]; then echo pass; elif [ $r -eq 2 ]; then echo skip; else echo fail; fi >> "$RESULTS"'
pass=$(grep -c pass "$RESULTS" 2>/dev/null); pass=${pass:-0}
fail=$(grep -c fail "$RESULTS" 2>/dev/null); fail=${fail:-0}
skip=$(grep -c skip "$RESULTS" 2>/dev/null); skip=${skip:-0}
if [ "$skip" -gt 0 ]; then
    echo "tests: $pass passed, $fail failed, $skip skipped"
else
    echo "tests: $pass passed, $fail failed"
fi
[ "$fail" -eq 0 ]
