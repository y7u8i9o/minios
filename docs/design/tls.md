# TLS client

The TLS client of libc fetches packages over HTTPS (`docs/plan/tls.md`).
`http_get` of `minios/http.h` uses the client for `https://` URLs, so
`http(1)` and `pkg(1)` support HTTPS. The cryptographic primitives are
described in `libc.md`.

## The client

`minios/tls.h` declares the client. The code is in
`lib/libc/src/net/tls.c`. The client implements TLS 1.3 of RFC 8446 and
nothing older:

- Key exchange with X25519 and secp256r1. The first ClientHello carries
  a share of each group.
- The suites TLS_CHACHA20_POLY1305_SHA256 and TLS_AES_128_GCM_SHA256.
  The client prefers ChaCha20-Poly1305, because the AES code of libc
  uses no hardware instructions. Both suites use SHA-256, so the
  transcript hash is SHA-256 from the first message on.
- The signature schemes ecdsa_secp256r1_sha256,
  ecdsa_secp384r1_sha384 and rsa_pss_rsae_sha256/384/512 for the
  handshake signature. The schemes rsa_pkcs1_sha256/384/512 are offered
  for certificates only.
- The extensions server_name (not for an IPv4 address),
  supported_groups, signature_algorithms, supported_versions, key_share
  and, after a HelloRetryRequest, cookie.

The client sends no session ID and no change_cipher_spec record. It
ignores the change_cipher_spec records of servers in middlebox
compatibility mode.

`tls_connect` runs the handshake on a connected socket:

1. The client sends the ClientHello. A HelloRetryRequest replaces the
   transcript with the hash of the first ClientHello (RFC 8446, section
   4.4.1). The second ClientHello carries the requested share and the
   cookie. A second HelloRetryRequest is an error.
2. The ServerHello must select TLS 1.3 through supported_versions, an
   offered suite, and a group with a share of the client. A ServerHello
   without supported_versions comes from an older server, and the client
   ends with the alert protocol_version.
3. The handshake secrets give the keys of the encrypted messages of the
   server: EncryptedExtensions, an optional CertificateRequest,
   Certificate, CertificateVerify and Finished.
4. The certificate chain must verify (`x509_verify_chain`) for the host
   at the current time. CertificateVerify must carry a valid signature
   of the leaf key over the transcript. The scheme must fit the key.
5. The client checks the Finished message of the server, derives the
   application secrets, and sends an empty Certificate when the server
   requested one, and its own Finished.

Handshake messages may span records, and a record may contain several
messages. A message must not span a change of the keys. The client sends
a fatal alert for every error that it detects. The text of the error
names the cause, for example "certificate expired" or "the server sent
the alert handshake_failure".

`tls_read` returns application data. The function also processes the
handshake messages after the handshake. A NewSessionTicket is ignored,
because the client does not resume sessions. A KeyUpdate replaces the
read keys. When the server requests an update, the client sends its own
KeyUpdate and replaces its write keys. close_notify and the end of the
TCP stream both end the data with 0. `tls_write` splits the data into
records of at most 16384 bytes. `tls_close` sends close_notify.

The socket may be non blocking. Every wait goes through `poll` with the
timeout of `tls_connect`, through the helpers of
`lib/libc/src/net/netio.c`, which `http.c` also uses.

## HTTPS

`http_parse_url` accepts `https://HOST[:PORT]/PATH` with the default
port 443. `http_get` loads the trust store for every request. The store
is `/etc/ssl/cert.pem`, or the file in the environment variable
`SSL_CERT_FILE`. The store is freed after the handshake. The request and
the response then run through `tls_write` and `tls_read`. A plain
connection ends its direction with `shutdown` after the request. A TLS
connection does not, because the server would not see a TCP half close
through TLS. The end of the TCP stream without close_notify ends the
body as for plain HTTP. The check of Content-Length detects a truncated
body.

`pkg` accepts `https://` URLs in `/etc/pkg.conf`. The signature of the
index and the digests of the archives remain the trust model of the
repositories (`packages.md`). TLS additionally hides the transfer and
authenticates the server.

## Certificates

`minios/x509.h` parses X.509 v3 certificates in DER and verifies a
server chain. The code is in `lib/libc/src/crypto/x509.c`.

`x509_parse` reads the fields of RFC 5280 that the client needs:

- the signed part, the issuer and the subject as DER ranges,
- the validity times, as UTCTime or GeneralizedTime in UTC,
- the key: EC on P-256 or P-384, or RSA,
- the signature algorithm: ECDSA or RSA PKCS#1 v1.5 with SHA-256,
  SHA-384 or SHA-512,
- the extensions basicConstraints, keyUsage, extKeyUsage and
  subjectAltName.

The algorithm inside the signed part must equal the outer algorithm.
An unsupported key or signature algorithm is no parse error. The
structure records the fact, and the verification fails with
`X509_ERR_ALGORITHM` only when the path uses the certificate. A critical
extension outside the four known extensions sets `unknown_critical`.

The structure points into the DER buffer of the caller. The parser
copies nothing.

## Trust store

`x509_store_load` reads a PEM file into a `struct x509_store`. The
system trust store is `/etc/ssl/cert.pem`. The file is the CA bundle of
curl (`third_party/cacert/`), which the curl project extracts from the
root list of Mozilla. `user/Makefile` installs the file, and the base
package `ca-certificates` contains it. The metapackage `minimal` depends
on `ca-certificates`. The bundle of
2026-09-25 contains 121 certificates, and every certificate parses.

`x509_pem_decode` returns the DER buffers of the CERTIFICATE blocks of
PEM text. The decoder uses `base64_decode` of `minios/base64.h`.

## Chain verification

`x509_verify_chain` receives the certificates of the server, the store,
the host name and the current time. The first certificate is the leaf.
The function first checks the leaf:

1. The time lies between notBefore and notAfter.
2. No critical extension is unknown.
3. The key algorithm is supported, because the handshake signature uses
   the key.
4. A keyUsage extension contains digitalSignature. An extKeyUsage
   extension contains serverAuth or anyExtendedKeyUsage.
5. The host matches the subjectAltName (`x509_check_host`).

The function then searches a path from the leaf to the store. For each
certificate of the path, the search first tries the anchors of the store
whose subject equals the issuer of the certificate. A trust anchor is a
name and a key. The search checks neither the validity time nor the
extensions of an anchor. Without a matching anchor, the search tries the
other certificates of the server whose subject equals the issuer. Such
an intermediate must be valid at the time, must have cA set, must allow
keyCertSign when keyUsage is present, must allow serverAuth when
extKeyUsage is present, and its pathLenConstraint must allow the
intermediates below it. The search backtracks when a branch fails. A
path has at most 8 intermediates. A certificate of the server that
equals an anchor byte for byte also ends the path.

The order of the search matters for cross-signed certificates. The
server `code.calcraft.org` sends its leaf, Let's Encrypt YE2, ISRG Root
YE cross-signed by ISRG Root X2, and ISRG Root X2 cross-signed by ISRG
Root X1. The bundle contains the self-signed ISRG Root X2. The path
therefore ends at Root YE, whose issuer X2 is an anchor. The
cross-signed X2 of the server is not used.

When no path exists, the function returns the error of the branch that
reached the greatest depth. A missing intermediate gives
`X509_ERR_ISSUER`. A wrong signature of an existing intermediate gives
`X509_ERR_SIGNATURE`. An expired intermediate gives `X509_ERR_EXPIRED`.
`x509_strerror` returns the text of a result.

Names compare byte for byte. Certificate authorities encode the issuer
of a certificate with the bytes of the subject of the issuing
certificate, so the comparison suffices in practice.

## Host names

`x509_check_host` uses only the subjectAltName. A certificate without
the extension is not valid for any host. A DNS name matches the host
without regard to case. A final dot of the host is ignored. A wildcard
must form the whole leftmost label (`*.example.org`). The wildcard
replaces exactly one label, and the rest must contain at least two
labels. An IPv4 address in dotted form matches only the iPAddress
entries of the extension.

## Not implemented

Revocation (CRL, OCSP), name constraints, policy constraints and RSA-PSS
certificate signatures are not implemented. A certificate with a
critical nameConstraints extension therefore fails with
`X509_ERR_CRITICAL`.

## Tests

`make check-crypto` replays the trace of RFC 8448, section 3. The test
interface of `lib/libc/src/net/tls_internal.h` gives the client the
ClientHello and the X25519 key of the trace. The records of the server
of the trace pass through a socketpair. The ClientHello, the Finished
record, the application data record and the close_notify record of the
client must equal the trace byte for byte. A second replay injects a
KeyUpdate that requests an update. The data after the update, the
KeyUpdate of the client and the data of the client after it must
decrypt with the updated keys.

The check also connects the client to TLS 1.3 servers of the `ssl`
module of Python (OpenSSL) on 127.0.0.1: a P-256 leaf below a P-384
intermediate, an RSA leaf with RSA-PSS, a P-384 leaf, both suites, a
server restricted to secp256r1 with and without a HelloRetryRequest, a
server that requests a client certificate, transfers of 100000 bytes, a
wrong host name, an unknown issuer, and a server without TLS 1.3.

The boot case `pkg_https` serves the repository v1 of `pkg_repo` over
TLS 1.3 from the host. The certificate of the server comes from
`tools/gen_tls_fixtures.py`. Its issuer is the test CA
`/etc/tests/tls-ca.pem`. The guest checks that the system store rejects
the server, that `pkg` updates and installs with `SSL_CERT_FILE` set to
the test CA, that `http(1)` fetches the same index through the address
and through a host name of the certificate, and that another host name
fails.

`make check-crypto` builds test chains with the `cryptography` package
of Python and verifies each chain with the oracle
`lib/libc/tests/crypto/oracle.c`. The cases cover valid chains with EC
and RSA keys, wildcards, IP addresses, validity times, a changed
signature, a missing intermediate, an anchor with another key, an
intermediate without cA, an exceeded path length, key usages, an
unknown critical extension, and a leaf without subjectAltName. The
check also loads the bundle and verifies the chain of
`code.calcraft.org` from `lib/libc/tests/crypto/calcraft.pem` at
2026-10-06, which lies inside the validity of the leaf.
