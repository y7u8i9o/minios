set -e
pkg install /usr/share/packages/synth-*.mpk /usr/share/packages/luasynth-*.mpk
pkg verify synth luasynth
lua /etc/tests/luasynth-engine.lua /usr/share/apps/luasynth/
test "$(man -w luasynth)" = /usr/share/man/man1/luasynth.1
pkg remove luasynth
pkg verify synth
test -x /usr/bin/synth
test ! -f /usr/bin/luasynth
echo 'luasynth: package coexistence verified'
