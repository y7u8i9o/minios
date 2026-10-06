#pragma once
/* This header declares SHA-256, SHA-384 and SHA-512 (FIPS 180-4, RFC
 * 6234). The package installer checks archive digests with SHA-256,
 * Ed25519 hashes with SHA-512, and TLS uses SHA-384 as well. The same
 * source compiles on the host for tools/pkgsign. */
#include <stddef.h>
#include <stdint.h>

#define SHA256_DIGEST_SIZE 32
#define SHA384_DIGEST_SIZE 48
#define SHA512_DIGEST_SIZE 64

/* In both contexts, h is the chaining state, length counts the bytes
 * hashed so far and used counts the bytes waiting in block. A message
 * must remain below 2^64 bytes. */
struct sha256_ctx {
    uint32_t h[8];
    uint64_t length;
    uint8_t block[64];
    size_t used;
};

struct sha512_ctx {
    uint64_t h[8];
    uint64_t length;
    uint8_t block[128];
    size_t used;
};

void sha256_init(struct sha256_ctx *c);
void sha256_update(struct sha256_ctx *c, const void *data, size_t len);
void sha256_final(struct sha256_ctx *c, uint8_t digest[SHA256_DIGEST_SIZE]);
void sha256(const void *data, size_t len, uint8_t digest[SHA256_DIGEST_SIZE]);

void sha512_init(struct sha512_ctx *c);
void sha512_update(struct sha512_ctx *c, const void *data, size_t len);
void sha512_final(struct sha512_ctx *c, uint8_t digest[SHA512_DIGEST_SIZE]);
void sha512(const void *data, size_t len, uint8_t digest[SHA512_DIGEST_SIZE]);

/* SHA-384 is SHA-512 with other initial values, truncated to 48 bytes. */
void sha384_init(struct sha512_ctx *c);
void sha384_final(struct sha512_ctx *c, uint8_t digest[SHA384_DIGEST_SIZE]);
void sha384(const void *data, size_t len, uint8_t digest[SHA384_DIGEST_SIZE]);

/* SHA-256 crypt: hash key with the "$5$[rounds=N$]salt" setting into out
 * in the form "$5$[rounds=N$]salt$hash". A stored hash may serve as the
 * setting. Returns out, or NULL for another method or a short buffer. */
char *sha256_crypt(const char *key, const char *setting, char *out, size_t size);
