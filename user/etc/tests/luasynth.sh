set -e
pkg install /usr/share/packages/synth-*.mpk /usr/share/packages/luasynth-*.mpk
pkg verify synth luasynth
lua /etc/tests/luasynth-engine.lua /usr/local/share/apps/luasynth/
test "$(man -w luasynth)" = /usr/local/share/man/man1/luasynth.1
pkg remove luasynth
pkg verify synth
test -x /usr/local/bin/synth
test ! -f /usr/local/bin/luasynth
echo 'luasynth: package coexistence verified'
