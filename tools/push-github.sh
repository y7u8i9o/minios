#!/bin/sh
# Push main, develop and the release tags to the GitHub copy of minios.
#
#   tools/push-github.sh [--force]
#
# The GitHub copy is the URL of the remote github. The script pushes main,
# develop and the tags v* of this repository to that URL. Other branches
# are not published.
#
# --force is needed only when the history on GitHub must be replaced.
#
# The remote github has a push URL that is not a repository. A plain
# `git push github` therefore fails and cannot publish other branches.
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

git -C "$TOP" push $FORCE "$URL" refs/heads/main:refs/heads/main \
    refs/heads/develop:refs/heads/develop 'refs/tags/v*:refs/tags/v*'
