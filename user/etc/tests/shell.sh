# Shell feature test, run as: sh /etc/tests/shell.sh x y
echo "args: $# $1 $2 all=$@"
NAME=world
echo "hello $NAME" 'single $NAME' "brace ${NAME}!"
echo escaped \$NAME "quoted \"inner\""
export EXPORTED=yes
sh -c 'echo child sees $EXPORTED'
false
echo "status after false: $?"
true && echo and-ran
false && echo and-skipped
false || echo or-ran
true || echo or-skipped
echo one; echo two
echo piped words | wc -w
sleep 0.2 &
echo background started
wait
echo background finished
echo $$ | wc -w
echo done > /shtest.out
cat /shtest.out
rm /shtest.out
exit 7
