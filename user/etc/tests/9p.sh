# The boot case 9p (V5 of docs/plan/release-0.6.0.md): the host shares a
# scratch folder with the mount tag host. tests/cases/9p/mkshare fills the
# folder, and tests/cases/9p/post checks the changes on the host.
fail() { echo "9p.sh: FAIL $*"; exit 1; }
[ -f /dev/9p/host ] || [ -c /dev/9p/host ] || fail "no /dev/9p/host"
mkdir -p /mnt/host || fail "mkdir /mnt/host"
mount -t 9p host /mnt/host || fail "mount -t 9p host /mnt/host"
mount | grep -q '^/mnt/host type 9p$' || fail "mount list: $(mount)"
cd /mnt/host || fail "cd /mnt/host"

# Reading.
[ "$(cat hello.txt)" = "hello from the host" ] || fail "hello.txt: $(cat hello.txt)"
[ "$(cat folder/inner.txt)" = "inside" ] || fail "folder/inner.txt: $(cat folder/inner.txt)"
[ "$(readlink link)" = "hello.txt" ] || fail "readlink link: $(readlink link)"
[ "$(cat link)" = "hello from the host" ] || fail "cat link: $(cat link)"
names=$(ls | sort | tr '\n' ' ')
[ "$names" = "folder hello.txt link old.txt " ] || fail "ls: $names"
echo "9p.sh: read the host files"

# Writing, renaming and removing.
echo "written by the guest" > guest.txt || fail "create guest.txt"
echo "line 2" >> guest.txt || fail "append to guest.txt"
chmod 600 guest.txt || fail "chmod guest.txt"
ls -l guest.txt | grep -q '^-rw-------' || fail "mode of guest.txt: $(ls -l guest.txt)"
ln -s guest.txt guestlink || fail "symlink guestlink"
ln guest.txt hard.txt || fail "link hard.txt"
mkdir -p tree/sub || fail "mkdir tree/sub"
echo a > tree/sub/a.txt || fail "create tree/sub/a.txt"
mv tree/sub/a.txt tree/moved.txt || fail "rename tree/sub/a.txt"
rmdir tree/sub || fail "rmdir tree/sub"
mkdir empty && rmdir empty || fail "mkdir and rmdir empty"
rm old.txt || fail "rm old.txt"
[ ! -e old.txt ] || fail "old.txt remains"
rm tree 2> /dev/null && fail "rm removed a folder"
# The host reports ENOTEMPTY in the numbering of Linux.
msg=$(rmdir tree 2>&1) && fail "rmdir removed a folder that is not empty"
case "$msg" in *"Directory not empty"*) ;; *) fail "rmdir of a folder that is not empty: $msg" ;; esac
yes 0123456789abcdef | head -c 1048576 > big.bin || fail "write big.bin"
yes 0123456789abcdef | head -c 1048576 > /tmp/big.bin
cmp big.bin /tmp/big.bin || fail "big.bin differs from its copy"
[ "$(wc -c < big.bin | tr -d ' ')" = 1048576 ] || fail "size of big.bin: $(wc -c < big.bin)"
: > /tmp/empty.txt
cp big.bin truncated.bin && : > truncated.bin || fail "truncate truncated.bin"
cmp truncated.bin /tmp/empty.txt || fail "truncated.bin is not empty"
rm truncated.bin
echo "9p.sh: wrote, renamed and removed files and folders"

# Four readers at once send several requests before the first answer.
for i in 1 2 3 4; do cmp big.bin /tmp/big.bin > /tmp/cmp.$i 2>&1 & done
wait
for i in 1 2 3 4; do [ ! -s /tmp/cmp.$i ] || fail "parallel read $i: $(cat /tmp/cmp.$i)"; done
most=$(sed -n 's/^most_pending //p' /dev/9p/host)
[ "$most" -ge 2 ] || fail "at most $most pending requests: $(cat /dev/9p/host)"
echo "9p.sh: four parallel readers, at most $most pending requests"

# A program on the share.
cp /bin/memtouch ./memtouch || fail "copy memtouch"
./memtouch 1 | grep -q 'memtouch: 1 MiB written and read' || fail "memtouch from the share"
rm memtouch
echo "9p.sh: ran a program from the share"

# Mappings of a file on the share.
mmapfiletest /mnt/host/maptest || fail "mmapfiletest"
echo "9p.sh: mapped a file"

df /mnt/host > /dev/null || fail "df /mnt/host"
sync || fail "sync"
cd /
mount -u /mnt/host || fail "umount /mnt/host"
mount -t 9p host /mnt/host || fail "mount again"
[ "$(cat /mnt/host/guest.txt)" = "$(printf 'written by the guest\nline 2')" ] || fail "guest.txt after a new mount"
mount -u /mnt/host || fail "umount /mnt/host again"
echo "9p.sh: done"
