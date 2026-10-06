#!/usr/bin/env python3
"""make check-crypto: compares the primitives of minios/crypto.h with the
cryptography package of Python (OpenSSL) on random inputs, and with
published test vectors. The certificate checks build test chains with
the cryptography package and verify them with minios/x509.h. They also
load the CA bundle of third_party/cacert and verify the chain of the
owner's server at a fixed time. The TLS checks replay the trace of RFC
8448 and connect the client to TLS 1.3 servers of the ssl module of
Python on 127.0.0.1. usage: check.py ORACLE TMPDIR"""
import base64
import datetime
import hashlib
import ipaddress
import hmac as py_hmac
import os
import random
import socket
import ssl
import subprocess
import sys
import threading

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, padding, rsa, x25519
from cryptography.hazmat.primitives.asymmetric.utils import Prehashed, decode_dss_signature
from cryptography.hazmat.primitives.ciphers.aead import AESGCM, ChaCha20Poly1305
from cryptography.hazmat.primitives.kdf.hkdf import HKDF
from cryptography import x509
from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID

rng = random.Random(1)
proc = subprocess.Popen([sys.argv[1]], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
checks = failures = 0


def h(b):
    return b.hex() if b else "-"


def ask(*words):
    proc.stdin.write(" ".join(words) + "\n")
    proc.stdin.flush()
    return proc.stdout.readline().strip()


def check(name, got, want):
    global checks, failures
    checks += 1
    if got != want:
        failures += 1
        print(f"FAIL {name}: got {got[:80]}, want {want[:80]}")


def rand(n):
    return bytes(rng.getrandbits(8) for _ in range(n))


HASHES = {"sha256": (hashlib.sha256, hashes.SHA256), "sha384": (hashlib.sha384, hashes.SHA384),
          "sha512": (hashlib.sha512, hashes.SHA512)}

# Hashes, HMAC and HKDF, also with keys longer than a block.
for name, (py, _) in HASHES.items():
    for n in list(range(0, 260, 7)) + [1000, 4000]:
        data = rand(n)
        check(f"{name} {n}", ask("hash", name, h(data)), py(data).hexdigest())
    for n in (0, 1, 32, 64, 127, 128, 129, 300):
        key, data = rand(n), rand(rng.randrange(0, 200))
        check(f"hmac {name} key {n}", ask("hmac", name, h(key), h(data)), py_hmac.new(key, data, py).hexdigest())
    for salt_len, length in ((0, 32), (13, 77), (64, 200)):
        salt, ikm, info = rand(salt_len), rand(22), rand(rng.randrange(0, 40))
        want = HKDF(algorithm=HASHES[name][1](), length=length, salt=salt or None, info=info).derive(ikm)
        check(f"hkdf {name} {length}", ask("hkdf", name, h(salt), h(ikm), h(info), str(length)), want.hex())

# The AEAD ciphers, and a changed byte of the ciphertext.
for name, cls, klen in (("chacha", ChaCha20Poly1305, 32), ("gcm", AESGCM, 16)):
    for n in (0, 1, 15, 16, 17, 63, 64, 65, 300, 1500):
        key, nonce, aad, pt = rand(klen), rand(12), rand(rng.randrange(0, 40)), rand(n)
        want = cls(key).encrypt(nonce, pt, aad)
        check(f"{name} seal {n}", ask(f"{name}_seal", h(key), h(nonce), h(aad), h(pt)), want.hex())
        check(f"{name} open {n}", ask(f"{name}_open", h(key), h(nonce), h(aad), h(want)), h(pt) if pt else "")
        bad = bytearray(want)
        bad[rng.randrange(len(bad))] ^= 1
        check(f"{name} open changed {n}", ask(f"{name}_open", h(key), h(nonce), h(aad), h(bytes(bad))), "bad")

# ChaCha20-Poly1305 with the example of RFC 8439, section 2.8.2.
key = bytes(range(0x80, 0xa0))
nonce = bytes.fromhex("070000004041424344454647")
aad = bytes.fromhex("50515253c0c1c2c3c4c5c6c7")
pt = (b"Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, "
      b"sunscreen would be it.")
out = ask("chacha_seal", h(key), h(nonce), h(aad), h(pt))
check("rfc 8439 tag", out[-32:], "1ae10b594f09e26a7e902ecbd0600691")

# X25519 against OpenSSL, and the vector of RFC 7748, section 5.2.
for i in range(20):
    a, b = x25519.X25519PrivateKey.generate(), x25519.X25519PrivateKey.generate()
    pub = b.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    priv = a.private_bytes(serialization.Encoding.Raw, serialization.PrivateFormat.Raw, serialization.NoEncryption())
    check(f"x25519 {i}", ask("x25519", h(priv), h(pub)), a.exchange(b.public_key()).hex())
check("x25519 rfc 7748",
      ask("x25519", "a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4",
          "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c"),
      "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552")

# P-256 keys and ECDH.
for i in range(10):
    a, b = ec.generate_private_key(ec.SECP256R1()), ec.generate_private_key(ec.SECP256R1())
    d = a.private_numbers().private_value.to_bytes(32, "big")
    pub_a = a.public_key().public_bytes(serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)
    pub_b = b.public_key().public_bytes(serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)
    check(f"p256 public {i}", ask("p256_public", h(d)), pub_a.hex())
    check(f"p256 shared {i}", ask("p256_shared", h(d), h(pub_b)), a.exchange(ec.ECDH(), b.public_key()).hex())
check("p256 shared with a point off the curve", ask("p256_shared", h(d), h(pub_b[:-1] + bytes([pub_b[-1] ^ 1]))),
      "bad")

# ECDSA, also with a digest longer and shorter than the order.
for curve, cname in ((ec.SECP256R1(), "p256"), (ec.SECP384R1(), "p384")):
    for hname in ("sha256", "sha384", "sha512"):
        for i in range(4):
            key = ec.generate_private_key(curve)
            pub = key.public_key().public_bytes(serialization.Encoding.X962,
                                                serialization.PublicFormat.UncompressedPoint)
            digest = HASHES[hname][0](rand(50)).digest()
            r, s = decode_dss_signature(key.sign(digest, ec.ECDSA(Prehashed(HASHES[hname][1]()))))
            rb, sb = r.to_bytes((r.bit_length() + 7) // 8, "big"), s.to_bytes((s.bit_length() + 7) // 8, "big")
            check(f"ecdsa {cname} {hname} {i}", ask("ecdsa", cname, h(pub), h(digest), h(rb), h(sb)), "ok")
            other = bytes([digest[0] ^ 1]) + digest[1:]
            check(f"ecdsa {cname} {hname} changed", ask("ecdsa", cname, h(pub), h(other), h(rb), h(sb)), "bad")

# RSA with PKCS#1 v1.5 and PSS.
for bits in (2048, 3072, 4096):
    key = rsa.generate_private_key(public_exponent=65537, key_size=bits)
    nums = key.public_key().public_numbers()
    n = nums.n.to_bytes(bits // 8, "big")
    e = nums.e.to_bytes(3, "big")
    for hname in ("sha256", "sha384", "sha512"):
        digest = HASHES[hname][0](rand(30)).digest()
        alg = Prehashed(HASHES[hname][1]())
        sig = key.sign(digest, padding.PKCS1v15(), alg)
        check(f"rsa {bits} pkcs1 {hname}", ask("rsa_pkcs1", hname, h(n), h(e), h(digest), h(sig)), "ok")
        bad = bytes([sig[0] ^ 1]) + sig[1:]
        check(f"rsa {bits} pkcs1 {hname} changed", ask("rsa_pkcs1", hname, h(n), h(e), h(digest), h(bad)), "bad")
        pss = padding.PSS(mgf=padding.MGF1(HASHES[hname][1]()), salt_length=len(digest))
        sig = key.sign(digest, pss, alg)
        check(f"rsa {bits} pss {hname}", ask("rsa_pss", hname, h(n), h(e), h(digest), h(sig)), "ok")
        other = bytes([digest[0] ^ 1]) + digest[1:]
        check(f"rsa {bits} pss {hname} changed", ask("rsa_pss", hname, h(n), h(e), h(other), h(sig)), "bad")

# Base64, also with invalid text.
for n in range(0, 40):
    data = rand(n)
    check(f"base64 {n}", ask("b64", base64.b64encode(data).decode() or "-"), h(data) if data else "")
for text in ("abc", "ab=c", "a===", "ab==ab==", "ab*d"):
    check(f"base64 invalid {text}", ask("b64", text), "bad")

# Certificates. Each case writes a chain and a store as PEM files and
# compares the result of the oracle with the expected text.
TMP = sys.argv[2]
os.makedirs(TMP, exist_ok=True)
T0 = datetime.datetime(2026, 1, 1, tzinfo=datetime.timezone.utc)
DAY = 86400
NOW = int(T0.timestamp()) + 10 * DAY


def key_usage(sign=False, cert_sign=False, encipher=False):
    return x509.KeyUsage(digital_signature=sign, content_commitment=False, key_encipherment=encipher,
                         data_encipherment=False, key_agreement=False, key_cert_sign=cert_sign, crl_sign=cert_sign,
                         encipher_only=False, decipher_only=False)


def make_cert(subject, key, issuer, issuer_key, ca=False, pathlen=None, days=(0, 365), san=None,
              eku=(ExtendedKeyUsageOID.SERVER_AUTH,), ku=None, extra=None, digest=hashes.SHA256()):
    b = (x509.CertificateBuilder()
         .subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, subject)]))
         .issuer_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, issuer)]))
         .public_key(key.public_key()).serial_number(x509.random_serial_number())
         .not_valid_before(T0 + datetime.timedelta(days=days[0]))
         .not_valid_after(T0 + datetime.timedelta(days=days[1]))
         .add_extension(x509.BasicConstraints(ca=ca, path_length=pathlen), critical=True))
    if ku is None:
        ku = key_usage(cert_sign=True) if ca else key_usage(sign=True)
    b = b.add_extension(ku, critical=True)
    if eku and not ca:
        b = b.add_extension(x509.ExtendedKeyUsage(list(eku)), critical=False)
    if san:
        names = [x509.IPAddress(ipaddress.ip_address(s)) if s[0].isdigit() else x509.DNSName(s) for s in san]
        b = b.add_extension(x509.SubjectAlternativeName(names), critical=False)
    if extra:
        b = b.add_extension(extra, critical=True)
    return b.sign(issuer_key, digest)


def pem(*certs):
    return b"".join(c.public_bytes(serialization.Encoding.PEM) for c in certs)


case_number = 0


def verify(label, host, now, chain, store, want):
    global case_number
    case_number += 1
    cpath, spath = f"{TMP}/chain{case_number}.pem", f"{TMP}/store{case_number}.pem"
    open(cpath, "wb").write(chain if isinstance(chain, bytes) else pem(*chain))
    open(spath, "wb").write(store if isinstance(store, bytes) else pem(*store))
    check(f"x509 {label}", ask("verify", host, str(now), cpath, spath), want)


root_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
root = make_cert("Test Root", root_key, "Test Root", root_key, ca=True)
int_key = ec.generate_private_key(ec.SECP384R1())
inter = make_cert("Test CA", int_key, "Test Root", root_key, ca=True, pathlen=0, digest=hashes.SHA384())
leaf_key = ec.generate_private_key(ec.SECP256R1())
SAN = ["example.test", "*.example.test", "10.0.2.2"]
leaf = make_cert("example.test", leaf_key, "Test CA", int_key, days=(5, 100), san=SAN, digest=hashes.SHA384())

verify("valid", "example.test", NOW, [leaf, inter], [root], "ok")
verify("wildcard", "a.example.test", NOW, [leaf, inter], [root], "ok")
verify("case and final dot", "A.EXAMPLE.Test.", NOW, [leaf, inter], [root], "ok")
verify("ip address", "10.0.2.2", NOW, [leaf, inter], [root], "ok")
verify("other ip address", "10.0.2.3", NOW, [leaf, inter], [root], "certificate not valid for the host")
verify("two labels for a wildcard", "b.a.example.test", NOW, [leaf, inter], [root],
       "certificate not valid for the host")
verify("other host", "example.org", NOW, [leaf, inter], [root], "certificate not valid for the host")
verify("not yet valid", "example.test", NOW - 9 * DAY, [leaf, inter], [root], "certificate not yet valid")
verify("expired", "example.test", NOW + 200 * DAY, [leaf, inter], [root], "certificate expired")
der = bytearray(leaf.public_bytes(serialization.Encoding.DER))
der[-1] ^= 1
broken = x509.load_der_x509_certificate(bytes(der))
verify("broken signature", "example.test", NOW, [broken, inter], [root], "certificate signature invalid")
verify("missing intermediate", "example.test", NOW, [leaf], [root], "certificate issuer unknown")
verify("empty store", "example.test", NOW, [leaf, inter], b"", "certificate issuer unknown")
other_root_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
other_root = make_cert("Test Root", other_root_key, "Test Root", other_root_key, ca=True)
verify("anchor with another key", "example.test", NOW, [leaf, inter], [other_root],
       "certificate signature invalid")
verify("order and an unused certificate", "example.test", NOW, [leaf, other_root, inter], [root], "ok")

no_ca = make_cert("Test CA", int_key, "Test Root", root_key, ku=key_usage(cert_sign=True))
verify("intermediate without cA", "example.test", NOW, [leaf, no_ca], [root], "certificate issuer is no CA")
old_inter = make_cert("Test CA", int_key, "Test Root", root_key, ca=True, days=(0, 5))
verify("expired intermediate", "example.test", NOW, [leaf, old_inter], [root], "certificate expired")
int2_key = ec.generate_private_key(ec.SECP256R1())
int2 = make_cert("Test CA 2", int2_key, "Test CA", int_key, ca=True, digest=hashes.SHA384())
leaf2 = make_cert("example.test", leaf_key, "Test CA 2", int2_key, san=SAN)
verify("path length exceeded", "example.test", NOW, [leaf2, int2, inter], [root], "certificate issuer is no CA")
inter_free = make_cert("Test CA", int_key, "Test Root", root_key, ca=True, digest=hashes.SHA384())
verify("two intermediates", "example.test", NOW, [leaf2, int2, inter_free], [root], "ok")
inter_ku = make_cert("Test CA", int_key, "Test Root", root_key, ca=True, ku=key_usage(sign=True))
verify("intermediate without keyCertSign", "example.test", NOW, [leaf, inter_ku], [root],
       "certificate key usage forbids the use")

crit = x509.UnrecognizedExtension(x509.ObjectIdentifier("1.3.6.1.4.1.99999.1"), b"\x05\x00")
leaf_crit = make_cert("example.test", leaf_key, "Test CA", int_key, san=SAN, extra=crit)
verify("unknown critical extension", "example.test", NOW, [leaf_crit, inter], [root],
       "certificate has an unknown critical extension")
leaf_client = make_cert("example.test", leaf_key, "Test CA", int_key, san=SAN,
                        eku=(ExtendedKeyUsageOID.CLIENT_AUTH,))
verify("client usage only", "example.test", NOW, [leaf_client, inter], [root],
       "certificate key usage forbids the use")
leaf_enc = make_cert("example.test", leaf_key, "Test CA", int_key, san=SAN, ku=key_usage(encipher=True))
verify("key usage without digitalSignature", "example.test", NOW, [leaf_enc, inter], [root],
       "certificate key usage forbids the use")
leaf_nosan = make_cert("example.test", leaf_key, "Test CA", int_key)
verify("no subjectAltName", "example.test", NOW, [leaf_nosan, inter], [root], "certificate not valid for the host")
leaf_tld = make_cert("example.test", leaf_key, "Test CA", int_key, san=["*.test"])
verify("wildcard over a top level domain", "example.test", NOW, [leaf_tld, inter], [root],
       "certificate not valid for the host")

# An RSA leaf below a P-384 root, signed with SHA-512, and an RSA leaf
# below an RSA root with SHA-384.
ec_root_key = ec.generate_private_key(ec.SECP384R1())
ec_root = make_cert("EC Root", ec_root_key, "EC Root", ec_root_key, ca=True, digest=hashes.SHA384())
rsa_leaf_key = rsa.generate_private_key(public_exponent=65537, key_size=3072)
rsa_leaf = make_cert("rsa.test", rsa_leaf_key, "EC Root", ec_root_key, san=["rsa.test"], digest=hashes.SHA512())
verify("rsa leaf below an ec root", "rsa.test", NOW, [rsa_leaf], [root, ec_root], "ok")
rsa_leaf2 = make_cert("rsa.test", rsa_leaf_key, "Test Root", root_key, san=["rsa.test"], digest=hashes.SHA384())
verify("rsa leaf below an rsa root", "rsa.test", NOW, [rsa_leaf2], [ec_root, root], "ok")

# The CA bundle of the system and the chain of the owner's server. The
# server sends Root YE cross-signed by ISRG Root X2, and X2 cross-signed
# by X1. The path ends at the self-signed X2 of the bundle.
HERE = os.path.dirname(os.path.abspath(__file__))
BUNDLE = os.path.join(HERE, "../../../../third_party/cacert/cacert.pem")
CHAIN = open(os.path.join(HERE, "calcraft.pem"), "rb").read()
blocks = open(BUNDLE).read().count("-----BEGIN CERTIFICATE-----")
check("bundle", ask("store", BUNDLE), f"{blocks} 0")
SERVER_TIME = int(datetime.datetime(2026, 10, 6, tzinfo=datetime.timezone.utc).timestamp())
bundle = open(BUNDLE, "rb").read()
verify("server chain", "code.calcraft.org", SERVER_TIME, CHAIN, bundle, "ok")
verify("server chain, other host", "calcraft.org", SERVER_TIME, CHAIN, bundle, "certificate not valid for the host")
verify("server chain, expired", "code.calcraft.org", SERVER_TIME + 60 * DAY, CHAIN, bundle, "certificate expired")
verify("server chain without the bundle", "code.calcraft.org", SERVER_TIME, CHAIN, pem(root),
       "certificate issuer unknown")

# TLS: the trace of RFC 8448, also with a KeyUpdate of the server.
TRACE = os.path.join(HERE, "rfc8448.txt")
check("tls rfc 8448", ask("rfc8448", TRACE), "ok")
check("tls rfc 8448 key update", ask("rfc8448", TRACE, "keyupdate"), "ok")


def key_pem(key):
    return key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                             serialization.NoEncryption())


def tls_server(label, chain, key, groups=None, request_cert=False, tls12=False):
    """Starts an echo server with the certificate chain and returns its
    port. The server answers every connection until the client closes."""
    certfile, keyfile = f"{TMP}/{label}.pem", f"{TMP}/{label}.key"
    open(certfile, "wb").write(pem(*chain))
    open(keyfile, "wb").write(key_pem(key))
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(certfile, keyfile)
    if tls12:
        ctx.maximum_version = ssl.TLSVersion.TLSv1_2
    else:
        ctx.minimum_version = ssl.TLSVersion.TLSv1_3
    if groups:
        ctx.set_ecdh_curve(groups)
    if request_cert:
        ctx.verify_mode = ssl.CERT_OPTIONAL
        ctx.load_verify_locations(f"{TMP}/tls-root.pem")
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    sock.listen(8)

    def serve():
        while True:
            conn, _ = sock.accept()
            try:
                with ctx.wrap_socket(conn, server_side=True) as s:
                    while True:
                        data = s.recv(65536)
                        if not data:
                            break
                        s.sendall(data)
            except (ssl.SSLError, OSError):
                pass

    threading.Thread(target=serve, daemon=True).start()
    return sock.getsockname()[1]


def tls_case(label, port, host, want, suite="0", x25519_only=0, size=1000, store=None):
    got = ask("tls", str(port), host, store or f"{TMP}/tls-root.pem", suite, str(x25519_only), str(size))
    check(f"tls {label}", got if not want.endswith("*") else got[:len(want) - 1] + "*", want)


open(f"{TMP}/tls-root.pem", "wb").write(pem(root, ec_root))
# The clock of the oracle is the real time. The chains of the TLS checks
# are valid from 2026-01-01 to 2027-01-01 at the earliest, so the checks
# use new chains valid from yesterday for ten years.
TODAY = datetime.datetime.now(datetime.timezone.utc)
T0 = TODAY - datetime.timedelta(days=1)
inter_now = make_cert("Test CA", int_key, "Test Root", root_key, ca=True, pathlen=0, days=(0, 3650),
                      digest=hashes.SHA384())
ec_leaf_now = make_cert("example.test", leaf_key, "Test CA", int_key, days=(0, 3650), san=SAN,
                        digest=hashes.SHA384())
rsa_leaf_now = make_cert("rsa.test", rsa_leaf_key, "Test Root", root_key, days=(0, 3650), san=["rsa.test"])
p384_key = ec.generate_private_key(ec.SECP384R1())
p384_leaf_now = make_cert("p384.test", p384_key, "EC Root", ec_root_key, days=(0, 3650), san=["p384.test"],
                          digest=hashes.SHA384())

ec_port = tls_server("ec", [ec_leaf_now, inter_now], leaf_key)
tls_case("p-256 leaf", ec_port, "example.test", "ok 1303 001d 0")
tls_case("aes-128-gcm", ec_port, "a.example.test", "ok 1301 001d 0", suite="1301")
tls_case("chacha20-poly1305", ec_port, "example.test", "ok 1303 001d 0", suite="1303")
tls_case("100000 bytes", ec_port, "example.test", "ok 1303 001d 0", size=100000)
tls_case("100000 bytes with aes-128-gcm", ec_port, "example.test", "ok 1301 001d 0", suite="1301", size=100000)
tls_case("other host", ec_port, "example.org", "fail: certificate not valid for the host")
tls_case("unknown issuer", ec_port, "example.test", "fail: certificate issuer unknown",
         store=BUNDLE)
rsa_port = tls_server("rsa", [rsa_leaf_now], rsa_leaf_key)
tls_case("rsa leaf with rsa-pss", rsa_port, "rsa.test", "ok 1303 001d 0")
p384_port = tls_server("p384", [p384_leaf_now], p384_key)
tls_case("p-384 leaf", p384_port, "p384.test", "ok 1303 001d 0")
p256_port = tls_server("p256group", [ec_leaf_now, inter_now], leaf_key, groups="prime256v1")
tls_case("secp256r1 key exchange", p256_port, "example.test", "ok 1303 0017 0")
tls_case("hello retry request", p256_port, "example.test", "ok 1303 0017 1", x25519_only=1)
req_port = tls_server("request", [ec_leaf_now, inter_now], leaf_key, request_cert=True)
tls_case("certificate request", req_port, "example.test", "ok 1303 001d 0")
old_port = tls_server("tls12", [ec_leaf_now, inter_now], leaf_key, tls12=True)
tls_case("server without tls 1.3", old_port, "example.test", "fail: the server sent the alert *")

proc.stdin.close()
proc.wait()
print(f"crypto tests: {checks} checks, {failures} failures")
sys.exit(1 if failures else 0)
