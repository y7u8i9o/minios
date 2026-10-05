#!/bin/sh
# Download pfetch, the system information script of Dylan Araps (MIT
# licence), into third_party/pfetch. Run from the repository root:
#
#     tools/fetch_pfetch.sh
#
# pfetch is one POSIX sh script. user/Makefile installs it unchanged as
# /usr/bin/pfetch, and docs/design/pfetch.md describes the support for
# minios that it needed.
set -eu

TOP="$(cd "$(dirname "$0")/.." && pwd)"
DIR="$TOP/third_party/pfetch"
URL="https://raw.githubusercontent.com/dylanaraps/pfetch/master"

mkdir -p "$DIR"
curl -fL --max-time 60 -o "$DIR/pfetch" "$URL/pfetch"
curl -fL --max-time 60 -o "$DIR/LICENSE.md" "$URL/LICENSE.md"
curl -fsL --max-time 60 "https://api.github.com/repos/dylanaraps/pfetch/commits/master" \
    | sed -n 's/^  "sha": "\(.*\)",/\1/p' > "$DIR/ORIGIN" || true
