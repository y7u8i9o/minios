# exec without a command, trap and set -f of /bin/sh (docs/design/sh.md).
trap 'echo exit trap ran' EXIT
trap 'echo usr1 trap ran' USR1
kill -USR1 $$
echo after usr1
set -f
echo /etc/mot*
set +f
echo /etc/mot*
exec 6>&1 >/dev/null
echo hidden output
echo visible through 6 >&6
exec >&6 6>&-
echo restored
trap
echo arith $((${UNSET_VALUE:-7}%8+1))
