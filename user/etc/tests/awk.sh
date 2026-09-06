# awk test, run as: sh /etc/tests/awk.sh. Every failing check prints a
# line starting with FAIL.
printf 'alice 30 paris\nbob 25 rome\ncarol 35 oslo\n' > /a.txt
test "$(awk '{ print $1 }' /a.txt | tr '\n' ' ')" = "alice bob carol " || echo "FAIL awk-field"
test "$(awk '{ s += $2 } END { print s }' /a.txt)" = "90" || echo "FAIL awk-sum"
test "$(awk '$2 > 28 { print $3 }' /a.txt | tr '\n' ' ')" = "paris oslo " || echo "FAIL awk-compare"
test "$(awk 'NR == 2' /a.txt)" = "bob 25 rome" || echo "FAIL awk-NR"
test "$(awk 'END { print NR, NF }' /a.txt)" = "3 3" || echo "FAIL awk-END"
test "$(awk -F: '{ print $2 }' <<'X'
a:b:c
X
)" = "b" || echo "FAIL awk-F"
test "$(awk 'BEGIN { OFS = "-" } { $1 = $1; print }' /a.txt | head -n 1)" = "alice-30-paris" || echo "FAIL awk-OFS"
test "$(awk 'BEGIN { printf "%5.2f|%-4s|%03d|%x\n", 3.14159, "ab", 7, 255 }')" = " 3.14|ab  |007|ff" || echo "FAIL awk-printf"
test "$(awk 'BEGIN { print length("hello"), substr("hello", 2, 3), index("hello", "l"), toupper("ab") tolower("CD") }')" = "5 ell 3 ABcd" || echo "FAIL awk-strings"
test "$(awk 'BEGIN { n = split("a,b,c", p, ","); print n, p[1], p[3] }')" = "3 a c" || echo "FAIL awk-split"
test "$(awk 'BEGIN { s = "foo bar foo"; n = gsub(/foo/, "baz", s); print n, s }')" = "2 baz bar baz" || echo "FAIL awk-gsub"
test "$(awk 'BEGIN { s = "hello"; sub(/l+/, "L", s); print s }')" = "heLo" || echo "FAIL awk-sub"
test "$(awk 'BEGIN { if (match("xxabcxx", /abc/)) print RSTART, RLENGTH }')" = "3 3" || echo "FAIL awk-match"
test "$(awk 'BEGIN { print sqrt(16), int(3.9), 2^10, 7 % 3, exp(0), log(1), sin(0), cos(0), atan2(0, 1) }')" = "4 3 1024 1 1 0 0 1 0" || echo "FAIL awk-math"
test "$(awk 'BEGIN { print 1e3, 0.1 + 0.2, 1/4, 10/3 }')" = "1000 0.3 0.25 3.33333" || echo "FAIL awk-numbers"
test "$(awk '{ a[$3] = $1 } END { print a["rome"], a["oslo"] }' /a.txt)" = "bob carol" || echo "FAIL awk-array"
test "$(awk 'BEGIN { a["x"] = 1; a["y"] = 2; n = 0; for (k in a) n += a[k]; print n, ("x" in a), ("z" in a) }')" = "3 1 0" || echo "FAIL awk-for-in"
test "$(awk 'BEGIN { a[1]; delete a[1]; print length(a) }')" = "0" || echo "FAIL awk-delete"
test "$(awk 'function f(n) { return n <= 1 ? 1 : n * f(n - 1) } BEGIN { print f(10) }')" = "3628800" || echo "FAIL awk-function"
test "$(awk 'BEGIN { i = 0; while (i < 3) { i++ }; do { i-- } while (i > 1); for (j = 0; j < 2; j++) i += 10; print i }')" = "21" || echo "FAIL awk-loops"
test "$(awk 'BEGIN { x = "10"; y = 9; print (x < y), (x + 0 < y), x y }')" = "1 0 109" || echo "FAIL awk-coercion"
test "$(printf 'b\na\nc\n' | awk '{ print | "sort" }')" = "a
b
c" || echo "FAIL awk-pipe-out"
test "$(awk 'BEGIN { "echo piped" | getline line; print line }')" = "piped" || echo "FAIL awk-getline-cmd"
test "$(awk 'BEGIN { while ((getline line < "/a.txt") > 0) n++; print n }')" = "3" || echo "FAIL awk-getline-file"
test "$(awk 'BEGIN { print "to file" > "/a2.txt"; close("/a2.txt"); getline l < "/a2.txt"; print l }')" = "to file" || echo "FAIL awk-redirect"
test "$(awk 'BEGIN { print ENVIRON["TEST"] }')" = "1" || echo "FAIL awk-environ"
test "$(awk -v n=5 'BEGIN { print n * 2 }')" = "10" || echo "FAIL awk-v"
test "$(awk 'BEGIN { print ARGC, ARGV[1] }' one two)" = "3 one" || echo "FAIL awk-argv"
test "$(printf 'a b\nc d\n' | awk 'BEGIN { RS = "" } { print NF }')" = "4" || echo "FAIL awk-RS-paragraph"
test "$(printf 'x\n' | awk '{ print length() }')" = "1" || echo "FAIL awk-length-record"
test "$(awk 'BEGIN { printf "%s\n", sprintf("%c%c", 72, "i") }')" = "Hi" || echo "FAIL awk-sprintf-c"
test "$(awk 'BEGIN { print substr("héllo", 2, 1) }')" = "é" || echo "FAIL awk-utf8"
test "$(awk 'BEGIN { print toupper("straße") }')" = "STRASSE" -o "$(awk 'BEGIN { print toupper("straße") }')" = "STRAßE" || echo "FAIL awk-toupper-utf8"
test "$(awk 'BEGIN { print strftime("%Y", 0) }')" = "1970" || echo "FAIL awk-strftime"
test "$(awk 'BEGIN { print mktime("1970 01 02 00 00 00") }')" = "86400" || echo "FAIL awk-mktime"
test "$(awk 'BEGIN { srand(7); a = rand(); srand(7); b = rand(); print (a == b), (a >= 0 && a < 1) }')" = "1 1" || echo "FAIL awk-rand"
test "$(awk 'BEGIN { print (systime() > 1000000000) }')" = "1" || echo "FAIL awk-systime"
test "$(awk 'BEGIN { exit 3 }'; echo $?)" = "3" || echo "FAIL awk-exit"
test "$(awk 'BEGIN { system("echo sys") }')" = "sys" || echo "FAIL awk-system"
printf 'BEGIN { print "from file" }\n' > /a.awk
test "$(awk -f /a.awk)" = "from file" || echo "FAIL awk-f"
test "$(awk --version | head -c 3)" = "awk" || echo "FAIL awk-version"
rm -f /a.txt /a2.txt /a.awk
echo "awk: done"
