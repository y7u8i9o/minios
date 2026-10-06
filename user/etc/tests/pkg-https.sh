# This is the HTTPS repository test, run as sh /etc/tests/pkg-https.sh.
# The boot harness sets NETPEER_PORT to the port of the TLS 1.3 server of
# the case's peer on the host, reached as 10.0.2.2
# (tests/cases/pkg_https/peer). The certificate of the server comes from
# the test CA /etc/tests/tls-ca.pem, which the system trust store does
# not contain. Every failing check prints a line starting with FAIL and
# the value received.
check() {
    test "$2" = "$3" || echo "FAIL $1: [$2]"
}
# contains checks that the value contains the text.
contains() {
    case "$2" in
        *"$3"*) ;;
        *) echo "FAIL $1: [$2]" ;;
    esac
}
URL=https://10.0.2.2:$NETPEER_PORT
check dhcp "$(/bin/dhcpc -1 -t 30 > /dev/null 2>&1; echo $?)" "0"
printf 'repo main %s\ntimeout 60\n' "$URL/v1" > /etc/pkg.conf

# The system trust store loads, and it does not contain the test CA.
contains untrusted "$(pkg update 2>&1)" "certificate issuer unknown"
contains no-store "$(SSL_CERT_FILE=/nonexistent pkg update 2>&1)" "trust store /nonexistent"

# With the test CA, the index and the packages arrive over TLS.
export SSL_CERT_FILE=/etc/tests/tls-ca.pem
check update "$(pkg update)" "main: 3 packages from $URL/v1"
out="$(pkg install repoprog)"
check install "$(echo "$out" | grep -v '^fetched' | tr '\n' ' ')" "installed repohello 1.0 installed repolib 1.0 installed repoprog 1.0 "
check runs "$(/usr/bin/repoprog)" "pkgfix 42"

# http(1) fetches the same index, also through a host name of the
# certificate. A name outside the certificate fails.
check http "$(http -o /tmp/index $URL/v1/index; echo $?)" "0"
check http-same "$(cmp /tmp/index /var/lib/pkg/_repos/main/index && echo same)" "same"
printf '10.0.2.2 repo.test\n10.0.2.2 other.test\n' >> /etc/hosts
check http-name "$(http https://repo.test:$NETPEER_PORT/v1/index > /dev/null; echo $?)" "0"
contains http-other "$(http https://other.test:$NETPEER_PORT/v1/index 2>&1)" "certificate not valid for the host"
echo "pkg-https: done"
