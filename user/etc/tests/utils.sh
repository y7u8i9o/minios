# Utility test, run as: sh /etc/tests/utils.sh. Every failing check prints
# a line starting with FAIL.
printf 'banana\napple\ncherry\napple\n' > /u.txt
test "$(sort /u.txt | uniq | tr '\n' ' ')" = "apple banana cherry " || echo "FAIL sort-uniq"
test "$(sort -u /u.txt | wc -l | tr -d ' ')" = "3" || echo "FAIL sort-u"
test "$(sort -rn /dev/null | wc -l | tr -d ' ')" = "0" || echo "FAIL sort-empty"
test "$(grep -c apple /u.txt)" = "2" || echo "FAIL grep-c"
test "$(grep -n cherry /u.txt)" = "3:cherry" || echo "FAIL grep-n"
test "$(grep -vi APPLE /u.txt | tr '\n' ',')" = "banana,cherry," || echo "FAIL grep-vi"
test "$(tail -n 1 /u.txt)" = "apple" || echo "FAIL tail"
test "$(head -n 1 /u.txt | rev)" = "ananab" || echo "FAIL rev"
test "$(seq 3 | tr '\n' '+')" = "1+2+3+" || echo "FAIL seq"
test "$(seq 2 2 6 | nl | cut -f2 | tr '\n' ' ')" = "2 4 6 " || echo "FAIL nl-cut"
test "$(echo a:b:c | cut -d: -f2,3)" = "b:c" || echo "FAIL cut-fields"
test "$(echo abcdef | cut -c2-4)" = "bcd" || echo "FAIL cut-chars"
test "$(echo hello | tr a-z A-Z)" = "HELLO" || echo "FAIL tr"
test "$(echo aabbcc | tr -s abc)" = "abc" || echo "FAIL tr-s"
test "$(expr 2 + 3 '*' 4)" = "14" || echo "FAIL expr"
test "$(expr '(' 2 + 3 ')' '*' 4)" = "20" || echo "FAIL expr-paren"
test "$(expr 7 % 3)" = "1" || echo "FAIL expr-mod"
test "$(expr 3 '<' 5)" = "1" || echo "FAIL expr-cmp"
test "$(basename /bin/sh)" = "sh" || echo "FAIL basename"
test "$(basename /tmp/file.txt .txt)" = "file" || echo "FAIL basename-suffix"
test "$(dirname /bin/sh)" = "/bin" || echo "FAIL dirname"
test "$(dirname sh)" = "." || echo "FAIL dirname-dot"
test "$(echo copy | tee /u2.txt)" = "copy" || echo "FAIL tee"
test "$(cat /u2.txt)" = "copy" || echo "FAIL tee-file"
test "$(printf 'x\nx\ny\n' | uniq -c | tr -s ' ' | tr '\n' ';')" = " 2 x; 1 y;" || echo "FAIL uniq-c"
test "$(which sh)" = "/bin/sh" || echo "FAIL which"
test "$(env FOO=bar printenv FOO)" = "bar" || echo "FAIL env"
test "$(yes | head -n 2 | tr '\n' ' ')" = "y y " || echo "FAIL yes"
cmp /u.txt /u.txt > /dev/null || echo "FAIL cmp-same"
printf 'banana\napple\ncherry\n' > /u3.txt
cmp /u.txt /u3.txt > /dev/null && echo "FAIL cmp-differ"
test "$(diff /u.txt /u3.txt | tail -n 1)" = "< apple" || echo "FAIL diff"
test "$(life -q -g 4 -w 8 -h 8 | tail -n 1 | cut -d, -f1)" = "generation 4" || echo "FAIL life"
test "$(maze 3 2 7 | wc -l | tr -d ' ')" = "5" || echo "FAIL maze"
test "$(banner a | wc -l | tr -d ' ')" = "14" || echo "FAIL banner"
test "$(cowsay hi | head -n 2 | tail -n 1)" = "< hi >" || echo "FAIL cowsay"
test "$(fortune | wc -l | tr -d ' ')" != "0" || echo "FAIL fortune"
test "$(stat /u.txt | grep -c Size)" = "1" || echo "FAIL stat"
ln /u.txt /u4.txt
test "$(cat /u4.txt | wc -l | tr -d ' ')" = "4" || echo "FAIL ln"
test "$(du -s /u.txt | cut -f1)" = "1" || echo "FAIL du"
test "$(uptime | cut -d' ' -f1)" = "up" || echo "FAIL uptime"
test "$(free | grep -c Mem)" = "1" || echo "FAIL free"
test "$(uname)" = "minios" || echo "FAIL uname"
case "$(uname -m)" in x86_64|aarch64) ;; *) echo "FAIL uname-m" ;; esac
test "$(uname -a)" = "$(uname -s) $(uname -n) $(uname -r) $(uname -v) $(uname -m)" || echo "FAIL uname-a"
test -f /u.txt || echo "FAIL test-f"
test -d /bin || echo "FAIL test-d"
test 3 -lt 5 || echo "FAIL test-lt"
test ! -e /nonexistent || echo "FAIL test-not"

mkdir /ufind
mkdir /ufind/sub
printf 'alpha\n' > /ufind/a.txt
printf 'beta\n' > /ufind/sub/b.txt
printf 'skip\n' > /ufind/sub/c.log
test "$(find /ufind -type f -name '*.txt' | sort | tr '\n' ' ')" = "/ufind/a.txt /ufind/sub/b.txt " || echo "FAIL find-name-type"
test "$(find /ufind -maxdepth 1 -type f)" = "/ufind/a.txt" || echo "FAIL find-maxdepth"
test "$(find /ufind -type f -print0 | xargs -0 -n 2 echo | wc -w | tr -d ' ')" = "3" || echo "FAIL find-print0-xargs"
test "$(printf "'two words' one\n" | xargs -n 1 echo | tr '\n' ',')" = "two words,one," || echo "FAIL xargs-quotes"
test "$(printf 'red\nblue\n' | xargs -I '{}' echo color='{}' | tr '\n' ' ')" = "color=red color=blue " || echo "FAIL xargs-replace"

printf 'compress me compress me compress me compress me\n' > /gzip.txt
gzip -c /gzip.txt > /gzip.txt.gz
gzip -t /gzip.txt.gz || echo "FAIL gzip-test"
test "$(gzip -dc /gzip.txt.gz)" = "compress me compress me compress me compress me" || echo "FAIL gzip-roundtrip"
test "$(pager /gzip.txt)" = "compress me compress me compress me compress me" || echo "FAIL pager-pipe"
test "$(man -w find)" = "/usr/share/man/man1/find.1" || echo "FAIL man-where"
test "$(man -f xargs | grep -c 'build and execute')" = "1" || echo "FAIL man-whatis"

# Terminal utility options, including redirected output without colour.
echo 'utils: terminal options'
test "$(ls -1 /ufind)" = "$(printf 'a.txt\nsub')" || echo 'FAIL ls-one'
test "$(ls -d /ufind)" = /ufind || echo 'FAIL ls-directory'
test "$(ls -F /ufind | tail -n 1)" = sub/ || echo 'FAIL ls-classify'
test "$(ls -r1 /ufind | head -n 1)" = sub || echo 'FAIL ls-reverse'
ls -lh /u.txt | grep -F u.txt > /dev/null || echo 'FAIL ls-long'
test "$(grep -E -c '^(apple|cherry)$' /u.txt)" = 3 || echo 'FAIL grep-ere'
test "$(grep -c '^apple$' /u.txt)" = 2 || echo 'FAIL grep-bre'
test "$(grep -F -c '^apple$' /u.txt)" = 0 || echo 'FAIL grep-fixed'
test "$(printf 'cat\nconcatenate\ncat!\n' | grep -w -c cat)" = 2 || echo 'FAIL grep-word'
test "$(grep -rh beta /ufind)" = beta || echo 'FAIL grep-recursive'
test "$(grep -H alpha /ufind/a.txt)" = /ufind/a.txt:alpha || echo 'FAIL grep-filename'
echo 'utils: grep options checked'
test "$(head -c 3 /u.txt)" = ban || echo 'FAIL head-bytes'
test "$(tail -c 4 /u.txt)" = ple || echo 'FAIL tail-bytes'
test "$(tail -n 0 /u.txt)" = '' || echo 'FAIL tail-zero'
test "$(printf 'a\n\n\nb\n' | cat -s | wc -l | tr -d ' ')" = 3 || echo 'FAIL cat-squeeze'
test "$(printf 'a\n\nb\n' | cat -b | grep -c '2')" = 1 || echo 'FAIL cat-nonblank'
test "$(printf 'a\tb\n' | cat -A)" = 'a^Ib$' || echo 'FAIL cat-visible'
test "$(cat -n /u.txt | tail -n 1 | tr -s ' ')" = "$(printf ' 4\tapple')" || echo 'FAIL cat-number'
tree -L 1 /ufind | grep sub > /dev/null || echo 'FAIL tree-depth'
tree -d /ufind | grep a.txt > /dev/null && echo 'FAIL tree-directories'
echo 'utils: text and tree checked'
test "$(less -RNS /u2.txt)" = copy || echo 'FAIL less-pipe'
test "$(df | grep -c '^mfs ')" = 1 || echo 'FAIL df-mfs'
test "$(ps | head -n 1 | tr -s ' ')" = ' PID PPID PGID USER STATE TIME RSS NAME' || echo 'FAIL ps-columns'
ps | grep ' root ' > /dev/null || echo 'FAIL ps-user'
echo 'utils: system options checked'
echo first > /follow.txt
tail -n 0 -f /follow.txt > /follow.out &
follower=$!
sleep 0.5
echo appended >> /follow.txt
sleep 0.5
kill -15 "$follower"
wait
test "$(cat /follow.out)" = appended || echo 'FAIL tail-follow'
rm /follow.txt /follow.out

rm /u.txt /u2.txt /u3.txt /u4.txt /gzip.txt /gzip.txt.gz
rm /ufind/a.txt /ufind/sub/b.txt /ufind/sub/c.log
rmdir /ufind/sub /ufind
echo "utils: done"
