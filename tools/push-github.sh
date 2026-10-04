#!/bin/sh
# Push main and the release tags to the GitHub copy of minios without
# CLAUDE.md.
#
#   tools/push-github.sh [--force]
#
# The GitHub copy is the URL of the remote github. Its history contains no
# CLAUDE.md. The script copies main and the tags v* into the bare
# repository github-mirror.git in the git directory. It removes CLAUDE.md
# from every commit of that copy with git filter-branch. It checks that no
# commit of the copy contains CLAUDE.md. Then it pushes main and the tags
# v* from the copy to GitHub.
#
# filter-branch gives the same commit for the same input. Every run
# therefore rebuilds the same history, and a later push adds only the new
# commits. --force is needed only when the history on GitHub must be
# replaced.
#
# The remote github has a push URL that is not a repository. A plain
# `git push github` therefore fails and cannot publish CLAUDE.md.
set -eu

TOP="$(cd "$(dirname "$0")/.." && pwd)"
FORCE=""
case "${1:-}" in
    --force) FORCE="--force" ;;
    "") ;;
    *) echo "usage: tools/push-github.sh [--force]" >&2; exit 1 ;;
esac

URL="$(git -C "$TOP" config remote.github.url)" || {
    echo "push-github: the remote github does not exist" >&2
    exit 1
}
GITDIR="$(cd "$TOP" && cd "$(git rev-parse --git-common-dir)" && pwd)"
MIRROR="$GITDIR/github-mirror.git"
[ -d "$MIRROR" ] || git init --quiet --bare "$MIRROR"

# Copy main and the release tags. Earlier filtered refs are replaced.
git -C "$MIRROR" fetch --quiet --force --no-tags "$TOP" \
    '+refs/heads/main:refs/heads/main' '+refs/tags/v*:refs/tags/v*'

# Remove CLAUDE.md from every commit. The tags are rewritten to the new
# commits with their names unchanged.
FILTER_BRANCH_SQUELCH_WARNING=1 git -C "$MIRROR" filter-branch -f \
    --index-filter 'git rm --cached --quiet --ignore-unmatch CLAUDE.md' \
    --tag-name-filter cat -- --all > /dev/null
git -C "$MIRROR" for-each-ref --format='%(refname)' refs/original |
    while read -r ref; do git -C "$MIRROR" update-ref -d "$ref"; done

if [ -n "$(git -C "$MIRROR" log --all --format=%H -- CLAUDE.md)" ]; then
    echo "push-github: CLAUDE.md is still in the history, nothing was pushed" >&2
    exit 1
fi

git -C "$MIRROR" push $FORCE "$URL" refs/heads/main:refs/heads/main 'refs/tags/v*:refs/tags/v*'
