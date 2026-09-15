set -e
pkg install /usr/share/packages/synth-*.mpk /usr/share/packages/luasynth-*.mpk
pkg verify synth luasynth
lua /etc/tests/luasynth-engine.lua /home/.local/share/apps/luasynth/
test "$(man -w luasynth)" = /home/.local/share/man/man1/luasynth.1
pkg remove luasynth
pkg verify synth
test -x /home/.local/bin/synth
test ! -f /home/.local/bin/luasynth
echo 'luasynth: package coexistence verified'
