# TLS

The owner revoked the decision "no TLS" on 2026-10-06: minios fetches
packages from the owner's server over HTTPS. The plan adds a TLS 1.3
client to libc and uses it in `http_get` and `pkg`. Each milestone ends
with its tests and is marked completed here.

## Requirements

The owner's server (`code.calcraft.org`) negotiates TLS 1.3 with X25519
and ChaCha20-Poly1305. Its certificate has an ECDSA P-256 key and is
signed with ECDSA P-384 by Let's Encrypt YE2, below ISRG Root YE, which
ISRG Root X2 cross-signs. Other servers commonly use RSA certificates
and AES-GCM. The client therefore implements:

- TLS 1.3 only (RFC 8446), as a client, with server authentication. No
  client certificates, no session resumption, no early data.
- Key exchange: X25519 and secp256r1.
- Cipher suites: TLS_CHACHA20_POLY1305_SHA256 and TLS_AES_128_GCM_SHA256,
  the suite every TLS 1.3 client must offer.
- Signatures: ECDSA with P-256 and P-384, RSA PKCS#1 v1.5 for
  certificates and RSA-PSS for the handshake signature.
- Certificates: X.509 v3 in DER, chains up to a trust anchor, validity
  times against the clock of the system (NTP since V2), basic
  constraints, and host names through subjectAltName with wildcards.
- Trust store: the Mozilla CA list as curl publishes it, installed as
  `/etc/ssl/cert.pem`.

## Milestones

### T1. Entropy for programs (completed 2026-10-06)

`getrandom` system call over the generator of `kernel/lib/random.c`,
and `getentropy`/`getrandom` in libc. Test: `libctest` reads random
bytes, and two reads differ.

### T2. Cryptographic primitives (completed 2026-10-06)

SHA-384, HMAC, HKDF, ChaCha20-Poly1305, AES-128-GCM, X25519, P-256 and
P-384 (ECDSA verification, ECDH on P-256), RSA verification with
PKCS#1 v1.5 and PSS, in `lib/libc/src/crypto/`. Test: a host check with
the test vectors of the RFCs and of NIST.

Result: `make check-crypto` compares 340 results of the C code with the
`cryptography` package of Python (OpenSSL) on random inputs, and with
the vectors of RFC 7748 and RFC 8439. Every check passes.

### T3. Certificates and the trust store

X.509 parsing, chain verification, host name checks and the trust
store. Test: a host check with the chain of the owner's server, an
expired certificate, a wrong host name and a broken signature.

### T4. The TLS 1.3 client

Record layer, handshake, key schedule and alerts, `minios/tls.h`, and
`https://` in `http_get` and `pkg`. Test: a host check that replays the
handshake of RFC 8448, and a boot case in which `pkg` installs a package
from an HTTPS server on the host with a test CA.

### T5. The repository on the owner's server

`make publish-repo` builds the repositories of both architectures and
copies them to the server. `/etc/pkg.conf` names the HTTPS URL.

## Size

About 7000 changed lines with tests and documents.
