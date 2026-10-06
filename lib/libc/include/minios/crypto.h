#pragma once
/* The cryptographic primitives of the TLS client (docs/plan/tls.md, T2):
 * hashes by algorithm, HMAC, HKDF, the AEAD ciphers ChaCha20-Poly1305 and
 * AES-128-GCM, X25519, the curves P-256 and P-384, and RSA signature
 * verification. The sources in lib/libc/src/crypto/ depend only on the C
 * library, so the host checks compile them unchanged. Byte strings of
 * numbers are big endian unless a function says otherwise. */
#include <stddef.h>
#include <stdint.h>
#include <minios/sha2.h>

/* ---- hashes ---- */

enum hash_alg { HASH_SHA256, HASH_SHA384, HASH_SHA512 };
#define HASH_MAX_SIZE 64
#define HASH_MAX_BLOCK 128

struct hash_ctx {
    enum hash_alg alg;
    union {
        struct sha256_ctx s256;
        struct sha512_ctx s512;
    } u;
};

size_t hash_size(enum hash_alg alg);
size_t hash_block_size(enum hash_alg alg);
void hash_init(struct hash_ctx *c, enum hash_alg alg);
void hash_update(struct hash_ctx *c, const void *data, size_t len);
/* Writes hash_size(c->alg) bytes. The context remains usable as a copy
 * of the state before the call when the caller copied it first. */
void hash_final(struct hash_ctx *c, uint8_t *out);
void hash(enum hash_alg alg, const void *data, size_t len, uint8_t *out);

/* ---- HMAC and HKDF (RFC 2104, RFC 5869) ---- */

struct hmac_ctx {
    struct hash_ctx inner, outer;
};

void hmac_init(struct hmac_ctx *c, enum hash_alg alg, const void *key, size_t key_len);
void hmac_update(struct hmac_ctx *c, const void *data, size_t len);
void hmac_final(struct hmac_ctx *c, uint8_t *out);
void hmac(enum hash_alg alg, const void *key, size_t key_len, const void *data, size_t len, uint8_t *out);
/* prk receives hash_size(alg) bytes. */
void hkdf_extract(enum hash_alg alg, const void *salt, size_t salt_len, const void *ikm, size_t ikm_len,
                  uint8_t *prk);
/* Returns 0, or -1 when out_len exceeds 255 times the hash size. */
int hkdf_expand(enum hash_alg alg, const uint8_t *prk, const void *info, size_t info_len, uint8_t *out,
                size_t out_len);

/* ---- AEAD ciphers (RFC 8439, NIST SP 800-38D) ---- */

#define AEAD_TAG_SIZE 16
#define AEAD_NONCE_SIZE 12

/* seal writes len bytes of ciphertext and the 16 byte tag after them.
 * open checks the tag of the len bytes and the tag after them and writes
 * len bytes of plaintext; it returns 0, or -1 for a wrong tag, and then
 * the output is zeroed. In and out may be the same buffer. */
void chacha20_poly1305_seal(const uint8_t key[32], const uint8_t nonce[12], const void *aad, size_t aad_len,
                            const void *in, size_t len, uint8_t *out);
int chacha20_poly1305_open(const uint8_t key[32], const uint8_t nonce[12], const void *aad, size_t aad_len,
                           const void *in, size_t len, const uint8_t tag[16], uint8_t *out);

struct aes128 {
    uint32_t rk[44];
};
void aes128_init(struct aes128 *k, const uint8_t key[16]);
void aes128_encrypt(const struct aes128 *k, const uint8_t in[16], uint8_t out[16]);
void aes128_gcm_seal(const uint8_t key[16], const uint8_t nonce[12], const void *aad, size_t aad_len,
                     const void *in, size_t len, uint8_t *out);
int aes128_gcm_open(const uint8_t key[16], const uint8_t nonce[12], const void *aad, size_t aad_len,
                    const void *in, size_t len, const uint8_t tag[16], uint8_t *out);

/* ---- X25519 (RFC 7748) ---- */

/* out = scalar * point; the u coordinates are little endian. Returns 0,
 * or -1 when the result is zero (a point of small order). */
int x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
void x25519_base(uint8_t out[32], const uint8_t scalar[32]);

/* ---- P-256 and P-384 (FIPS 186-4, SEC 1) ---- */

enum ec_curve { EC_P256, EC_P384 };

/* The size of a coordinate in bytes: 32 or 48. A public key is the
 * uncompressed point 0x04 || x || y. */
size_t ec_size(enum ec_curve curve);
/* Verifies the ECDSA signature (r, s) of the digest. r and s are big
 * endian integers of any length up to the coordinate size, as DER gives
 * them. A digest longer than the order is truncated to its leftmost
 * bits. Returns 0 for a valid signature, -1 otherwise, also for a public
 * key that is not a point of the curve. */
int ecdsa_verify(enum ec_curve curve, const uint8_t *pub, size_t pub_len, const uint8_t *digest,
                 size_t digest_len, const uint8_t *r, size_t r_len, const uint8_t *s, size_t s_len);
/* The public key of the private scalar priv (32 bytes, in 1 .. n - 1)
 * and the shared x coordinate with a peer key. Return 0 or -1 for an
 * invalid scalar or peer key. */
int ec_p256_public(uint8_t pub[65], const uint8_t priv[32]);
int ec_p256_shared(uint8_t shared[32], const uint8_t priv[32], const uint8_t peer[65]);

/* ---- RSA signature verification (RFC 8017) ---- */

/* n and e are the modulus and the public exponent, the modulus from 1024
 * to 4096 bits. The signature has the length of the modulus. Returns 0
 * for a valid signature, -1 otherwise. pss uses MGF1 with the same hash
 * and a salt of the hash length, as TLS 1.3 requires. */
int rsa_verify_pkcs1(const uint8_t *n, size_t n_len, const uint8_t *e, size_t e_len, enum hash_alg alg,
                     const uint8_t *digest, const uint8_t *sig, size_t sig_len);
int rsa_verify_pss(const uint8_t *n, size_t n_len, const uint8_t *e, size_t e_len, enum hash_alg alg,
                   const uint8_t *digest, const uint8_t *sig, size_t sig_len);

/* Compares in time that depends only on len. Returns 0 when equal. */
int crypto_memcmp(const void *a, const void *b, size_t len);
/* Zeroes memory in a way that the compiler does not remove. */
void crypto_wipe(void *p, size_t len);
