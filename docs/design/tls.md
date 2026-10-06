# TLS client

The TLS client of libc fetches packages over HTTPS (`docs/plan/tls.md`).
This document describes the parts that exist. The cryptographic
primitives are described in `libc.md`.

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
