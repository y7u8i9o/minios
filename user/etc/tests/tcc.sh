# tcc test, run as: sh /etc/tests/tcc.sh. Every failing check prints a line
# starting with FAIL and the value received.
check() {
    test "$2" = "$3" || echo "FAIL $1: [$2]"
}
mkdir /tt
cd /tt
cat > hello.c <<'C'
#include <stdio.h>
int main(void) { printf("hello from tcc\n"); return 3; }
C
tcc -o hello hello.c; check tcc-compile "$?" "0"
./hello; check tcc-run-status "$?" "3"
check tcc-run-output "$(./hello)" "hello from tcc"
check tcc-run-flag "$(tcc -run hello.c)" "hello from tcc"
check tcc-version "$(tcc -v | cut -d' ' -f1-2)" "tcc version"
check tcc-preprocess "$(printf '#define X 4\nint v = X;\n' > pp.c; tcc -E pp.c | grep -v '^#' | tr -d '\n')" "int v = 4;"
cat > a.c <<'C'
int twice(int x) { return 2 * x; }
C
cat > b.c <<'C'
#include <stdio.h>
int twice(int);
int main(void) { printf("%d\n", twice(21)); return 0; }
C
tcc -c a.c -o a.o; check tcc-object "$?" "0"
tcc -c b.c -o b.o && tcc -o ab a.o b.o; check tcc-link-objects "$?" "0"
check tcc-two-objects "$(./ab)" "42"
tcc -shared -o libtwice.so a.c; check tcc-shared "$?" "0"
cp libtwice.so /lib/libtwice.so
tcc -o abso b.c -L/lib -ltwice; check tcc-link-shared "$?" "0"
check tcc-run-shared "$(./abso)" "42"
rm /lib/libtwice.so
cat > m.c <<'C'
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
int main(void) { char *p = malloc(8); strcpy(p, "ok"); printf("%s %.1f %d %ld\n", p, sqrt(16.0), INT_MAX, (long)INT64_MAX); return 0; }
C
check tcc-libc-headers "$(tcc -run m.c)" "ok 4.0 2147483647 9223372036854775807"
printf 'int main(void) { return undefined_function(); }\n' > u.c
tcc -o u u.c 2> /dev/null; test $? != 0 || echo "FAIL tcc-undefined-not-rejected"
printf 'int main(void) { return 1 +; }\n' > s.c
tcc -o s s.c 2> /dev/null; test $? != 0 || echo "FAIL tcc-syntax-error-not-rejected"
# The suite compares the diagnostics of tcc and the output of the program
# together, with the source named by its base name, and ignores trailing
# white space, as the upstream Makefile does with diff -b.
passed=0
failed=0
cd /usr/share/tcc/tests2
for src in *.c; do
    name=$(basename $src .c)
    flags=""
    case $name in 22_floating_point|24_math_library) flags="-lm" ;; esac
    rm -f /tt/t
    tcc -o /tt/t $flags $src > /tt/t.out 2>&1 && /tt/t >> /tt/t.out 2>&1
    sed 's/[ \t]*$//' /tt/t.out > /tt/t.norm
    sed 's/[ \t]*$//' $name.expect > /tt/t.want
    if cmp /tt/t.norm /tt/t.want > /dev/null; then
        passed=$((passed + 1))
    else
        failed=$((failed + 1))
        echo "FAIL tcc-suite-$name: [$(head -c 200 /tt/t.out | tr '\n' ' ')]"
    fi
done
echo "tcc suite: $passed passed, $failed failed"
cd /
find /tt -type f | xargs rm
rmdir /tt
echo "tcc: done"
