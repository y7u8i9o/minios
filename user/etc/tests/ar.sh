# ar test, run as: sh /etc/tests/ar.sh. Every failing check prints a line
# starting with FAIL and the value received.
check() {
    test "$2" = "$3" || echo "FAIL $1: [$2]"
}
mkdir /ar
cd /ar
printf 'alpha\n' > a.o
printf 'beta\n' > b.o
printf 'a name that is longer than fifteen characters\n' > a-name-that-is-longer-than-fifteen.o
check ar-create-message "$(ar r lib.a a.o b.o 2>&1)" "ar: creating lib.a"
check ar-t "$(ar t lib.a | tr '\n' ' ')" "a.o b.o "
check ar-rc-quiet "$(ar rc lib2.a a.o 2>&1)" ""
check ar-magic "$(head -c 8 lib.a)" "!<arch>"
check ar-header-name "$(head -c 76 lib.a | tail -c 68 | head -c 4)" "a.o/"
check ar-p "$(ar p lib.a b.o)" "beta"
check ar-p-all "$(ar p lib.a | tr '\n' ' ')" "alpha beta "
check ar-r-verbose "$(ar rv lib.a a-name-that-is-longer-than-fifteen.o)" "a - a-name-that-is-longer-than-fifteen.o"
check ar-long-name "$(ar t lib.a | tail -n 1)" "a-name-that-is-longer-than-fifteen.o"
check ar-long-table "$(head -c 70 lib.a | tail -c 62 | head -c 2)" "//"
printf 'alpha two\n' > a.o
check ar-replace "$(ar rv lib.a a.o)" "r - a.o"
check ar-replaced-content "$(ar p lib.a a.o)" "alpha two"
check ar-order-kept "$(ar t lib.a | tr '\n' ' ')" "a.o b.o a-name-that-is-longer-than-fifteen.o "
check ar-tv "$(ar tv lib.a b.o | cut -c1-9)" "rw-r--r--"
check ar-tv-size "$(ar tv lib.a b.o | tr -s ' ' | cut -d' ' -f3)" "5"
mkdir out
cd out
ar x ../lib.a
check ar-x "$(cat a.o b.o | tr '\n' ' ')" "alpha two beta "
check ar-x-long "$(cat a-name-that-is-longer-than-fifteen.o)" "a name that is longer than fifteen characters"
cd ..
check ar-x-verbose "$(ar xv lib.a b.o)" "x - b.o"
check ar-d "$(ar dv lib.a b.o)" "d - b.o"
check ar-d-result "$(ar t lib.a | tr '\n' ' ')" "a.o a-name-that-is-longer-than-fifteen.o "
ar q lib.a b.o
ar q lib.a b.o
check ar-q-duplicates "$(ar t lib.a | tr '\n' ' ')" "a.o a-name-that-is-longer-than-fifteen.o b.o b.o "
sleep 1
printf 'gamma\n' > b.o
ar ru lib.a a.o b.o
check ar-u-updated "$(ar p lib.a b.o | head -n 1)" "gamma"
check ar-u-kept "$(ar p lib.a a.o)" "alpha two"
ar t nosuch.a 2> /dev/null; test $? != 0 || echo "FAIL ar-missing-archive"
ar z lib.a 2> /dev/null; test $? = 2 || echo "FAIL ar-usage"
printf 'not an archive\n' > text.a
ar t text.a 2> /dev/null; test $? != 0 || echo "FAIL ar-not-archive"
cat > Makefile <<'M'
libm.a(m.o): m.o
	ar rc libm.a m.o
all: libm.a(m.o)
	@echo built
M
printf 'member\n' > m.o
sleep 1
check ar-make-member "$(make all | tail -n 1)" "built"
sleep 1
touch m.o
check ar-make-member-rebuild "$(make all | head -n 1)" "ar rc libm.a m.o"
cd /
for f in a.o b.o a-name-that-is-longer-than-fifteen.o lib.a lib2.a text.a Makefile m.o libm.a out/a.o out/b.o out/a-name-that-is-longer-than-fifteen.o; do rm /ar/$f > /dev/null 2>&1; done
rmdir /ar/out /ar
echo "ar: done"
