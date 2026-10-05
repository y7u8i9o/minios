# The boot case ntp (V2 of docs/plan/release-0.6.0.md): ntpd against the
# SNTP server of the peer on the host, at NETPEER_PORT. The peer answers
# the four requests in turn: one hour ahead of the host, then 50 ms ahead
# of that, then with a client mode, then with a zero transmit time.
fail() { echo "ntp.sh: FAIL $*"; exit 1; }
/bin/dhcpc -1 -t 30 > /dev/null || fail "no lease from the QEMU DHCP server"
printf 'server 10.0.2.2 port %s\n' "$NETPEER_PORT" > /tmp/ntp.conf
before=$(date +%s)
ntpd -1 -c /tmp/ntp.conf > /tmp/1.out || fail "first query: $(cat /tmp/1.out)"
grep -q ', step$' /tmp/1.out || fail "no step: $(cat /tmp/1.out)"
after=$(date +%s)
step=$((after - before))
[ "$step" -ge 3595 ] && [ "$step" -le 3605 ] || fail "the clock moved by $step s"
echo "ntp.sh: stepped by $step s"
ntpd -1 -c /tmp/ntp.conf > /tmp/2.out || fail "second query: $(cat /tmp/2.out)"
grep -q ', slew$' /tmp/2.out || fail "no slew: $(cat /tmp/2.out)"
ntpd -q | grep -q '^action slew$' || fail "state: $(ntpd -q)"
offset=$(ntpd -q | sed -n 's/^offset_us //p')
[ "$offset" -ge 40000 ] && [ "$offset" -le 60000 ] || fail "slew offset $offset us"
echo "ntp.sh: slewed by $offset us"
if ntpd -1 -c /tmp/ntp.conf > /tmp/3.out; then fail "a client mode answer was accepted"; fi
grep -q 'not a server answer' /tmp/3.out || fail "mode: $(cat /tmp/3.out)"
if ntpd -1 -c /tmp/ntp.conf > /tmp/4.out; then fail "a zero transmit time was accepted"; fi
grep -q 'zero transmit time' /tmp/4.out || fail "zero time: $(cat /tmp/4.out)"
ntpd -q | grep -q '^error no valid answer$' || fail "state after a refusal: $(ntpd -q)"
echo "ntp.sh: done"
