# The boot case net_transfer: the program transfer of the package transfer
# against tools/transfer.py on the host (docs/design/filetransfer.md). The
# peer script of the case serves a folder on NETPEER_PORT. This script
# copies files in both directions as a client. Then it serves /tmp/served
# on port 9100 for the client of the host and waits for its last file.
fail() { echo "transfer.sh: FAIL $*"; exit 1; }
/bin/dhcpc -1 -t 30 > /dev/null || fail "no lease from the QEMU DHCP server"
pkg install /usr/share/packages/transfer-*.mpk > /dev/null || fail "pkg install"
T=/usr/bin/transfer
P="-p $NETPEER_PORT"
mkdir -p /tmp/in /tmp/served /tmp/up/tree/empty
# The host serves big.bin: 187500 lines "minios transfer", 3000000 bytes,
# with the modification time 1700000000. The part file of its first
# 1000000 bytes makes the download resume.
lua -e 'local f = io.open("/tmp/pattern", "wb"); f:write(string.rep("minios transfer\n", 187500)); f:close()'
head -c 1000000 /tmp/pattern > /tmp/in/.big.bin.3000000-1700000000.mft-part
$T ls $P > /tmp/ls.out || fail "ls"
grep -q ' big.bin$' /tmp/ls.out || fail "ls output: $(cat /tmp/ls.out)"
grep -q ' tree/$' /tmp/ls.out || fail "ls folder: $(cat /tmp/ls.out)"
$T get $P -o /tmp/in big.bin tree > /tmp/get.out || fail "get"
grep -q '^downloaded big.bin, 3000000 bytes$' /tmp/get.out || fail "get output: $(cat /tmp/get.out)"
cmp /tmp/in/big.bin /tmp/pattern || fail "big.bin content"
test ! -f /tmp/in/.big.bin.3000000-1700000000.mft-part || fail "part file left"
test -d /tmp/in/tree/empty || fail "empty folder"
test "$(cat /tmp/in/tree/sub/leaf.txt)" = leaf || fail "tree file"
echo "from minios" > /tmp/up/tree/note.txt
cp /tmp/pattern /tmp/up/copy.bin
$T put $P /tmp/up/tree /tmp/up/copy.bin > /tmp/put.out || fail "put"
grep -q '^uploaded copy.bin, 3000000 bytes$' /tmp/put.out || fail "put output: $(cat /tmp/put.out)"
grep -q '^uploaded tree/note.txt, 12 bytes$' /tmp/put.out || fail "put tree: $(cat /tmp/put.out)"
$T mkdir $P made || fail "mkdir"
$T mv $P made renamed || fail "mv"
if $T mkdir $P renamed 2> /tmp/err.out; then fail "mkdir of an existing folder"; fi
grep -q '(EEXIST)$' /tmp/err.out || fail "EEXIST: $(cat /tmp/err.out)"
$T rm $P renamed || fail "rm"
if $T ls -p 1 2> /tmp/err.out; then fail "ls of a closed port"; fi
grep -q '(ECONNREFUSED)$' /tmp/err.out || fail "ECONNREFUSED: $(cat /tmp/err.out)"
# The server for the host. The host puts up-host.bin with the
# modification time 1700000000, and the part file makes the server resume.
head -c 1000000 /tmp/pattern > /tmp/served/.up-host.bin.3000000-1700000000.mft-part
cp /tmp/pattern /tmp/served/guest.bin
$T serve -p 9100 /tmp/served > /tmp/serve.out &
server=$!
i=0
while [ ! -f /tmp/served/done.txt ] && [ $i -lt 240 ]; do
    sleep 1
    i=$((i + 1))
done
kill $server
test -f /tmp/served/done.txt || fail "the host did not finish: $(cat /tmp/serve.out)"
grep -q '^serving /tmp/served on port 9100$' /tmp/serve.out || fail "serve output: $(cat /tmp/serve.out)"
grep -q '^sent guest.bin, 3000000 bytes$' /tmp/serve.out || fail "sent line: $(cat /tmp/serve.out)"
grep -q '^stored up-host.bin, 3000000 bytes, resumed at 1000000$' /tmp/serve.out || fail "resumed upload: $(cat /tmp/serve.out)"
cmp /tmp/served/up-host.bin /tmp/pattern || fail "up-host.bin content"
echo "transfer.sh: done"
