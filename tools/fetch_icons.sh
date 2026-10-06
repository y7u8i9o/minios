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
stop:circle-stop
kill:ban
profile:chart-line
folder-new:folder-plus
drive:hard-drive
recent:clock-rotate-left
documents:file-lines
pictures:image
music:music
videos:film
downloads:download
menu:bars
calendar:calendar-days
power-off:power-off
restart:rotate-right
show-desktop:desktop
notifications:bell
notifications-off:bell-slash
close:xmark
app-default:window-maximize
app-term:terminal
app-files:folder
app-clock:clock
app-sysinfo:microchip
app-sysmon:gauge-high
app-logview:file-lines
app-profiler:chart-line
app-evtest:computer-mouse
app-screenshot:camera
app-settings:gear
app-x12settings:sliders
app-wireview:network-wired
app-logout:right-from-bracket
app-gedit:pen-to-square
app-hexview:hashtag
app-player:music
app-calc:calculator
app-synth:wave-square
app-code:code
app-paint:paintbrush
app-luasynth:keyboard
app-mandel:hurricane
app-pong:table-tennis-paddle-ball
app-sequencer:table-cells
app-view:image
app-unicode:font
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
