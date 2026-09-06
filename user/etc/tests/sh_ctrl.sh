# Control flow, expansions, functions and descriptor redirections.
x=0
for i in 1 2 3; do
    x=$((x + i))
done
test "$x" -eq 6 || echo 'FAIL for-arithmetic'
if false; then
    echo 'FAIL if'
elif test "$x" -eq 6; then
    echo 'script2: if'
else
    echo 'FAIL elif'
fi
while test "$x" -gt 0; do
    x=$((x - 1))
done
until test "$x" -eq 3; do x=$((x + 1)); done
test "$x" -eq 3 || echo 'FAIL loops'
for i in a b c; do
    test "$i" = a && continue
    test "$i" = b && break
    echo 'FAIL break-continue'
done
check_scope() {
    local x=inner
    test "$x" = inner || echo 'FAIL local'
    test "$#" -eq 2 || echo 'FAIL function-args'
    shift
    test "$1" = second || echo 'FAIL shift'
    return 4
}
check_scope first second
test "$?" -eq 4 || echo 'FAIL return'
test "$x" -eq 3 || echo 'FAIL scope-restore'
case hello in
    nope) echo 'FAIL case';;
    h*|bye) echo 'script2: case';;
    *) echo 'FAIL case-default';;
esac
read first rest <<EOF
hello two words
EOF
test "$first" = hello || echo 'FAIL heredoc-read'
test "$rest" = 'two words' || echo 'FAIL read-remainder'
read literal <<'EOF'
$first
EOF
test "$literal" = '$first' || echo 'FAIL quoted-heredoc'
{ echo error >&2; } 2> /sh-errors
test "$(cat /sh-errors)" = error || echo 'FAIL stderr'
test "$(sh -c 'echo err >&2' 2>&1)" = err || echo 'FAIL dup-stderr'
mkdir /sh-glob
echo a > /sh-glob/a.txt
echo b > /sh-glob/b.txt
test "$(echo /sh-glob/*.txt)" = '/sh-glob/a.txt /sh-glob/b.txt' || echo 'FAIL glob'
WORDS='one two'
count() { test "$#" -eq "$1"; }
count 3 $WORDS || echo 'FAIL splitting'
count 2 "$WORDS" || echo 'FAIL quoting'
test "$((1 + 2 * 3))" -eq 7 || echo 'FAIL precedence'
test "$((0 && 1 / 0))" -eq 0 || echo 'FAIL short-circuit'
alias hello='echo hello'
test "$(hello world)" = 'hello world' || echo 'FAIL alias'
eval 'x=done'
test "$x" = done || echo 'FAIL eval'
(x=subshell)
test "$x" = done || echo 'FAIL subshell'
test "${MISSING:-fallback}" = fallback || echo 'FAIL parameter-default'
test "${WORDS% two}" = one || echo 'FAIL parameter-trim'
rm /sh-errors /sh-glob/a.txt /sh-glob/b.txt
rmdir /sh-glob
(set -e; false; echo 'FAIL errexit-continued')
(set -e; false || true; if false; then :; fi; while false; do :; done; ! false; true && false || true; echo errexit-suspended) > /sh-errexit
test "$(cat /sh-errexit)" = errexit-suspended || echo 'FAIL errexit-suspended'
rm /sh-errexit
(set -e; set +e; false; echo errexit-off) > /sh-errexit
test "$(cat /sh-errexit)" = errexit-off || echo 'FAIL errexit-off'
rm /sh-errexit
set -e -- p q
test "$1$2" = pq || echo 'FAIL set-options-then-parameters'
set +e
set -z 2> /dev/null && echo 'FAIL set-unknown-option'
echo 'script2: done'
exit 11
