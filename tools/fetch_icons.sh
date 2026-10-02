#!/bin/sh
# Download the Font Awesome Free (solid) SVG icons the desktop uses into
# third_party/fontawesome/svgs/<minios name>.svg, plus the license.
# Run from any directory; needs curl and network access.
#
#   tools/fetch_icons.sh            downloads what is missing
#   tools/fetch_icons.sh -f         downloads everything again
set -e
TOP="$(cd "$(dirname "$0")/.." && pwd)"
DIR="$TOP/third_party/fontawesome"
BASE="https://raw.githubusercontent.com/FortAwesome/Font-Awesome/6.x"
FORCE=""
[ "$1" = -f ] && FORCE=1
mkdir -p "$DIR/svgs"

# minios icon name : Font Awesome solid icon name
ICONS="
new:file-circle-plus
open:folder-open
save:floppy-disk
cut:scissors
copy:copy
paste:paste
quit:circle-xmark
folder:folder
file:file
terminal:terminal
clock:clock
paint:paintbrush
pong:table-tennis-paddle-ball
search:magnifying-glass
up:arrow-up
edit:pen
back:arrow-left
forward:arrow-right
home:house
refresh:arrows-rotate
zoom-in:magnifying-glass-plus
zoom-out:magnifying-glass-minus
fit:expand
undo:rotate-left
redo:rotate-right
line:slash
rectangle:square
ellipse:circle
fill:fill-drip
eraser:eraser
wallpaper:desktop
"

fetch() {
    url="$1"
    out="$2"
    if [ -z "$FORCE" ] && [ -s "$out" ]; then
        echo "have    $out"
        return
    fi
    echo "fetch   $url"
    curl -fsSL --retry 3 -o "$out.tmp" "$url"
    mv "$out.tmp" "$out"
}

fetch "$BASE/LICENSE.txt" "$DIR/LICENSE.txt"
for pair in $ICONS; do
    name="${pair%%:*}"
    fa="${pair#*:}"
    fetch "$BASE/svgs/solid/$fa.svg" "$DIR/svgs/$name.svg"
done
echo "done: $(ls "$DIR/svgs" | wc -l | tr -d ' ') icons in $DIR/svgs"
