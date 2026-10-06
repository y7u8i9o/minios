#!/usr/bin/env python3
"""make check-crypto: compares the primitives of minios/crypto.h with the
cryptography package of Python (OpenSSL) on random inputs, and with
published test vectors. usage: check.py ORACLE"""
import hashlib
import hmac as py_hmac
import os
import random
import subprocess
import sys

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, padding, rsa, x25519
from cryptography.hazmat.primitives.asymmetric.utils import Prehashed, decode_dss_signature
from cryptography.hazmat.primitives.ciphers.aead import AESGCM, ChaCha20Poly1305
from cryptography.hazmat.primitives.kdf.hkdf import HKDF

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

proc.stdin.close()
proc.wait()
print(f"crypto tests: {checks} checks, {failures} failures")
sys.exit(1 if failures else 0)
