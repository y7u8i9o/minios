/* Hashes by algorithm, HMAC (RFC 2104) and HKDF (RFC 5869), and the
 * helpers crypto_memcmp and crypto_wipe (minios/crypto.h). */
#include <minios/crypto.h>
#include <string.h>

size_t hash_size(enum hash_alg alg)
{
    return alg == HASH_SHA256 ? SHA256_DIGEST_SIZE : alg == HASH_SHA384 ? SHA384_DIGEST_SIZE : SHA512_DIGEST_SIZE;
}

size_t hash_block_size(enum hash_alg alg)
{
    return alg == HASH_SHA256 ? 64 : 128;
}

void hash_init(struct hash_ctx *c, enum hash_alg alg)
{
    c->alg = alg;
    if (alg == HASH_SHA256)
        sha256_init(&c->u.s256);
    else if (alg == HASH_SHA384)
        sha384_init(&c->u.s512);
    else
        sha512_init(&c->u.s512);
}

void hash_update(struct hash_ctx *c, const void *data, size_t len)
{
    if (c->alg == HASH_SHA256)
        sha256_update(&c->u.s256, data, len);
    else
        sha512_update(&c->u.s512, data, len);
}

void hash_final(struct hash_ctx *c, uint8_t *out)
{
    if (c->alg == HASH_SHA256)
        sha256_final(&c->u.s256, out);
    else if (c->alg == HASH_SHA384)
        sha384_final(&c->u.s512, out);
    else
        sha512_final(&c->u.s512, out);
}

void hash(enum hash_alg alg, const void *data, size_t len, uint8_t *out)
{
    struct hash_ctx c;
    hash_init(&c, alg);
    hash_update(&c, data, len);
    hash_final(&c, out);
}

void hmac_init(struct hmac_ctx *c, enum hash_alg alg, const void *key, size_t key_len)
{
    uint8_t block[HASH_MAX_BLOCK], pad[HASH_MAX_BLOCK];
    size_t bs = hash_block_size(alg);
    memset(block, 0, sizeof block);
    /* A key longer than a block is replaced by its hash. */
    if (key_len > bs)
        hash(alg, key, key_len, block);
    else
        memcpy(block, key, key_len);
    for (size_t i = 0; i < bs; i++)
        pad[i] = block[i] ^ 0x36;
    hash_init(&c->inner, alg);
    hash_update(&c->inner, pad, bs);
    for (size_t i = 0; i < bs; i++)
        pad[i] = block[i] ^ 0x5c;
    hash_init(&c->outer, alg);
    hash_update(&c->outer, pad, bs);
    crypto_wipe(block, sizeof block);
    crypto_wipe(pad, sizeof pad);
}

void hmac_update(struct hmac_ctx *c, const void *data, size_t len)
{
    hash_update(&c->inner, data, len);
}

void hmac_final(struct hmac_ctx *c, uint8_t *out)
{
    uint8_t inner[HASH_MAX_SIZE];
    hash_final(&c->inner, inner);
    hash_update(&c->outer, inner, hash_size(c->outer.alg));
    hash_final(&c->outer, out);
    crypto_wipe(inner, sizeof inner);
}

void hmac(enum hash_alg alg, const void *key, size_t key_len, const void *data, size_t len, uint8_t *out)
{
    struct hmac_ctx c;
    hmac_init(&c, alg, key, key_len);
    hmac_update(&c, data, len);
    hmac_final(&c, out);
    crypto_wipe(&c, sizeof c);
}

void hkdf_extract(enum hash_alg alg, const void *salt, size_t salt_len, const void *ikm, size_t ikm_len,
                  uint8_t *prk)
{
    uint8_t zeros[HASH_MAX_SIZE] = { 0 };
    /* An absent salt is a string of zeros of the hash length. */
    if (!salt_len) {
        salt = zeros;
        salt_len = hash_size(alg);
    }
    hmac(alg, salt, salt_len, ikm, ikm_len, prk);
}

int hkdf_expand(enum hash_alg alg, const uint8_t *prk, const void *info, size_t info_len, uint8_t *out,
                size_t out_len)
{
    size_t hl = hash_size(alg);
    if (out_len > 255 * hl)
        return -1;
    uint8_t t[HASH_MAX_SIZE];
    size_t t_len = 0;
    for (uint8_t i = 1; out_len; i++) {
        struct hmac_ctx c;
        hmac_init(&c, alg, prk, hl);
        hmac_update(&c, t, t_len);
        hmac_update(&c, info, info_len);
        hmac_update(&c, &i, 1);
        hmac_final(&c, t);
        t_len = hl;
        size_t k = out_len < hl ? out_len : hl;
        memcpy(out, t, k);
        out += k;
        out_len -= k;
    }
    crypto_wipe(t, sizeof t);
    return 0;
}

int crypto_memcmp(const void *a, const void *b, size_t len)
{
    const volatile uint8_t *x = a, *y = b;
    uint8_t d = 0;
    for (size_t i = 0; i < len; i++)
        d |= x[i] ^ y[i];
    return d != 0;
}

void crypto_wipe(void *p, size_t len)
{
    volatile uint8_t *q = p;
    for (size_t i = 0; i < len; i++)
        q[i] = 0;
}
