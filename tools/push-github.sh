#!/bin/sh
# Push main, develop and the release tags to the GitHub copy of minios.
#
#   tools/push-github.sh [--force]
#
# The GitHub copy is the remote github. The script pushes main, develop and
# the tags v* of this repository to that remote. Other branches are not
# published.
#
# --force is needed only when the history on GitHub must be replaced.
#
# The script also configures the push refspecs of the remote github. A plain
# `git push github` then pushes the same refs as the script.
set -eu

TOP="$(cd "$(dirname "$0")/.." && pwd)"
FORCE=""
case "${1:-}" in
    --force) FORCE="--force" ;;
    "") ;;
    *) echo "usage: tools/push-github.sh [--force]" >&2; exit 1 ;;
esac

git -C "$TOP" config remote.github.url >/dev/null || {
    echo "push-github: the remote github does not exist" >&2
    exit 1
}

# Older checkouts set a push URL that is not a repository. Remove it.
git -C "$TOP" config --unset remote.github.pushurl || true
git -C "$TOP" config --unset-all remote.github.push || true
git -C "$TOP" config --add remote.github.push refs/heads/main:refs/heads/main
git -C "$TOP" config --add remote.github.push refs/heads/develop:refs/heads/develop
git -C "$TOP" config --add remote.github.push 'refs/tags/v*:refs/tags/v*'

git -C "$TOP" push $FORCE github
