# Exercise the actual application archives, not copies of base binaries.
set -e
names='calc code gedit hexview luasynth mandel paint player playtone pong sequencer synth unicode view'
for name in $names; do
    test ! -f /bin/$name
done
test ! -f /usr/share/apps/code.lua
test ! -f /usr/share/man/man1/code.1
test ! -f /etc/skel/desktop/Code.app
test ! -f /etc/skel/desktop/Pong.app
pkg check /usr/share/packages/*.mpk
test "$(pkg list)" = ''
pkg install /usr/share/packages/*.mpk
count=$(pkg list | wc -l | tr -d ' ')
test "$count" = 14 || { echo "FAIL package count: [$count]"; exit 1; }
pkg verify
echo 'pkg-apps: records verified'
for name in $names; do
    test -x /usr/bin/$name || { echo "FAIL executable: $name"; exit 1; }
done
export PATH=/bin:/usr/bin
calc --self-test
test "$(man -w code)" = /usr/share/man/man1/code.1
man -f code | grep 'source editor'
man -k 'source editor' | grep 'code (1)'
man code | grep 'Ctrl+N'
test -f /usr/share/apps/code.lua
test -f /usr/share/apps/pong.lua
grep 'Code=/usr/bin/code' /var/lib/pkg/launcher
grep 'Unicode viewer=/usr/bin/unicode' /var/lib/pkg/launcher
lua -e 'local s=require "mime"; assert(s.handler("text/x-lua")=="/usr/bin/code"); assert(s.handler("text/plain")=="/usr/bin/gedit"); assert(s.handler("image/png")=="/usr/bin/view"); assert(s.handler("audio/wav")=="/usr/bin/player"); assert(s.handler("application/octet-stream")=="/usr/bin/hexview"); assert(s.handler("inode/directory")=="/bin/files")'
pkg remove code
test ! -f /usr/bin/code
test ! -f /usr/share/apps/code.lua
test ! -f /usr/share/man/man1/code.1
if man -w code 2>/dev/null; then
    echo 'FAIL removed manual still found'
    exit 1
fi
lua -e 'assert(require("mime").handler("text/x-lua")=="/usr/bin/gedit")'
pkg install /usr/share/packages/code-*.mpk
pkg verify code
pkg remove $names
test "$(pkg list)" = ''
for name in $names; do
    test ! -f /usr/bin/$name
done
lua -e 'assert(require("mime").handler("text/x-lua")==nil)'
echo 'pkg-apps: done'
