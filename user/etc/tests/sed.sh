# sed test, run as: sh /etc/tests/sed.sh. Every failing check prints a
# line starting with FAIL.
printf 'one\ntwo\nthree\nfour\nfive\n' > /s.txt
test "$(sed -n 2p /s.txt)" = "two" || echo "FAIL sed-n-p"
test "$(sed 's/o/0/g' /s.txt | tr '\n' ' ')" = "0ne tw0 three f0ur five " || echo "FAIL sed-s-g"
test "$(sed -e 's/e$/E/' -e '3d' /s.txt | tr '\n' ' ')" = "onE two four fivE " || echo "FAIL sed-e-d"
test "$(sed -n '/^t/p' /s.txt | tr '\n' ' ')" = "two three " || echo "FAIL sed-regex-p"
test "$(sed -n '2,4p' /s.txt | wc -l | tr -d ' ')" = "3" || echo "FAIL sed-range"
test "$(sed -n '$p' /s.txt)" = "five" || echo "FAIL sed-last"
test "$(sed '1!G;h;$!d' /s.txt | head -n 1)" = "five" || echo "FAIL sed-hold-reverse"
test "$(echo 'hello world' | sed -E 's/(hello) (world)/\2 \1/')" = "world hello" || echo "FAIL sed-E-backref"
test "$(echo 'hello world' | sed 's/\(hello\) \(world\)/\2 \1/')" = "world hello" || echo "FAIL sed-bre-backref"
test "$(echo abc | sed 'y/abc/xyz/')" = "xyz" || echo "FAIL sed-y"
test "$(printf 'a\nb\n' | sed 'a\
after' | tr '\n' ' ')" = "a after b after " || echo "FAIL sed-a"
test "$(printf 'a\nb\n' | sed '1i\
first' | tr '\n' ' ')" = "first a b " || echo "FAIL sed-i-text"
test "$(printf 'a\nb\nc\n' | sed '2c\
changed' | tr '\n' ' ')" = "a changed c " || echo "FAIL sed-c"
test "$(printf 'a\nb\nc\n' | sed -n '2{p;p;}' | tr '\n' ' ')" = "b b " || echo "FAIL sed-block"
test "$(printf 'a\nb\nc\n' | sed '2q')" = "a
b" || echo "FAIL sed-q"
test "$(printf 'a\nb\nc\n' | sed -n '$=')" = "3" || echo "FAIL sed-count"
test "$(printf 'x1\nx2\n' | sed -e 's/x/y/' -e 't end' -e 's/$/!/' -e ':end')" = "y1
y2" || echo "FAIL sed-t-label"
test "$(printf 'x1\nz2\n' | sed -e 's/x/y/' -e 't end' -e 's/$/!/' -e ':end')" = "y1
z2!" || echo "FAIL sed-t-notaken"
test "$(printf 'a\nb\nc\nd\n' | sed 'N;s/\n/+/')" = "a+b
c+d" || echo "FAIL sed-N"
test "$(printf 'a b\n' | sed 's/ /\n/' | wc -l | tr -d ' ')" = "2" || echo "FAIL sed-newline"
test "$(printf 'A\n' | sed 's/a/z/I')" = "z" || echo "FAIL sed-I-flag"
test "$(echo aaa | sed 's/a/b/2')" = "aba" || echo "FAIL sed-nth"
test "$(printf 'k=v\n' | sed 's/=.*//')" = "k" || echo "FAIL sed-greedy"
printf 'alpha\nbeta\n' > /s2.txt
sed -i '' 's/a/A/' /s2.txt
sed -i .bak 's/l/L/' /s2.txt
test "$(cat /s2.txt.bak | tr '\n' ' ')" = "Alpha betA " || echo "FAIL sed-in-place-backup"
test "$(cat /s2.txt | tr '\n' ' ')" = "ALpha betA " || echo "FAIL sed-in-place"
printf 's/one/1/\n/three/d\n' > /s.sed
test "$(sed -f /s.sed /s.txt | tr '\n' ' ')" = "1 two four five " || echo "FAIL sed-f"
test "$(printf 'x\n' | sed 'w /s3.txt' > /dev/null; cat /s3.txt)" = "x" || echo "FAIL sed-w"
test "$(printf 'a\nb\n' | sed '1r /s3.txt' | tr '\n' ' ')" = "a x b " || echo "FAIL sed-r"
test "$(printf 'a\tb\n' | sed -n l)" = 'a\tb$' || echo "FAIL sed-l"
rm -f /s.txt /s2.txt /s2.txt.bak /s3.txt /s.sed
echo "sed: done"
