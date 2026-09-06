#!/bin/sh
# Merge every bleeding-edge branch into develop when one of them received a
# commit in the last 24 hours. Runs daily from launchd (see docs/design/build.md).
#
# usage: tools/fold-develop.sh [--force] [--push]
#   --force  merge even without a recent commit
#   --push   push develop to origin after a successful merge (also PUSH=1)
#
# The merge happens in a temporary worktree, so the checkout the user is
# working in is not touched. A conflict aborts the merge and leaves develop
# unchanged; the exit status is then 1.
set -eu

REPO="$(cd "$(dirname "$0")/.." && pwd)"
LOG="${FOLD_LOG:-$HOME/Library/Logs/minios-fold-develop.log}"
FORCE=0
PUSH="${PUSH:-0}"
for arg in "$@"; do
    case "$arg" in
        --force) FORCE=1 ;;
        --push) PUSH=1 ;;
        *) echo "unknown option $arg" >&2; exit 2 ;;
    esac
done

mkdir -p "$(dirname "$LOG")"
log() { printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" | tee -a "$LOG"; }

cd "$REPO"
BRANCHES="$(git for-each-ref --format='%(refname:short)' refs/heads/bleeding-edge refs/heads/bleeding-edge-*)"
if [ -z "$BRANCHES" ]; then
    log "no bleeding-edge branches"
    exit 0
fi

RECENT=""
for b in $BRANCHES; do
    if [ -n "$(git log -1 --since='24 hours ago' --format=%h "$b")" ]; then
        RECENT="$RECENT $b"
    fi
done
if [ -z "$RECENT" ] && [ "$FORCE" = 0 ]; then
    log "no commit in the last 24 hours on:$(printf ' %s' $BRANCHES)"
    exit 0
fi

WT="$(mktemp -d "${TMPDIR:-/tmp}/minios-fold.XXXXXX")"
cleanup() { cd "$REPO"; git worktree remove --force "$WT" 2>/dev/null || true; rmdir "$WT" 2>/dev/null || true; }
trap cleanup EXIT

git worktree add --quiet "$WT" develop
cd "$WT"
BEFORE="$(git rev-parse --short develop)"
for b in $BRANCHES; do
    if git merge-base --is-ancestor "$b" develop; then
        log "$b is already in develop"
        continue
    fi
    if git merge --no-edit -m "Merge $b into develop" "$b" >/dev/null 2>&1; then
        log "merged $b ($(git rev-parse --short "$b")) into develop"
    else
        git merge --abort || true
        log "conflict merging $b into develop; develop left at $BEFORE"
        exit 1
    fi
done
AFTER="$(git rev-parse --short develop)"
log "develop $BEFORE -> $AFTER"

if [ "$PUSH" = 1 ] && [ "$BEFORE" != "$AFTER" ]; then
    if git push --quiet origin develop; then
        log "pushed develop"
    else
        log "push of develop failed"
        exit 1
    fi
fi
