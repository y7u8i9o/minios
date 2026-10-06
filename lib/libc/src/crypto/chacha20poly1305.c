/* ChaCha20-Poly1305 (RFC 8439) for minios/crypto.h. The ChaCha20 key
 * stream encrypts from block counter 1. Block 0 gives the key of
 * Poly1305, which authenticates the additional data, the ciphertext and
 * their lengths, each padded to 16 bytes. Poly1305 computes modulo
 * 2^130 - 5 in five limbs of 26 bits. */
#include <minios/crypto.h>
#include <string.h>

static uint32_t load32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void store32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void store64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

/* ---- ChaCha20 ---- */

#define ROTL(v, n) ((v) << (n) | (v) >> (32 - (n)))
#define QUARTER(a, b, c, d) \
    a += b; d ^= a; d = ROTL(d, 16); \
    c += d; b ^= c; b = ROTL(b, 12); \
    a += b; d ^= a; d = ROTL(d, 8); \
    c += d; b ^= c; b = ROTL(b, 7)

/* One block of key stream for the counter. */
static void chacha20_block(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], uint8_t out[64])
{
    uint32_t s[16], x[16];
    s[0] = 0x61707865;
    s[1] = 0x3320646e;
    s[2] = 0x79622d32;
    s[3] = 0x6b206574;
    for (int i = 0; i < 8; i++)
        s[4 + i] = load32(key + 4 * i);
    s[12] = counter;
    for (int i = 0; i < 3; i++)
        s[13 + i] = load32(nonce + 4 * i);
    memcpy(x, s, sizeof x);
    for (int i = 0; i < 10; i++) {
        QUARTER(x[0], x[4], x[8], x[12]);
        QUARTER(x[1], x[5], x[9], x[13]);
        QUARTER(x[2], x[6], x[10], x[14]);
        QUARTER(x[3], x[7], x[11], x[15]);
        QUARTER(x[0], x[5], x[10], x[15]);
        QUARTER(x[1], x[6], x[11], x[12]);
        QUARTER(x[2], x[7], x[8], x[13]);
        QUARTER(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; i++)
        store32(out + 4 * i, x[i] + s[i]);
    crypto_wipe(x, sizeof x);
    crypto_wipe(s, sizeof s);
}

static void chacha20_xor(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], const uint8_t *in,
                         size_t len, uint8_t *out)
{
    uint8_t ks[64];
    while (len) {
        chacha20_block(key, counter++, nonce, ks);
        size_t k = len < 64 ? len : 64;
        for (size_t i = 0; i < k; i++)
            out[i] = in[i] ^ ks[i];
        in += k;
        out += k;
        len -= k;
    }
    crypto_wipe(ks, sizeof ks);
}

/* ---- Poly1305 ---- */

struct poly1305 {
    uint32_t r[5], h[5], pad[4];
};

static void poly1305_init(struct poly1305 *p, const uint8_t key[32])
{
    /* r is clamped as the RFC requires. */
    p->r[0] = load32(key + 0) & 0x3ffffff;
    p->r[1] = (load32(key + 3) >> 2) & 0x3ffff03;
    p->r[2] = (load32(key + 6) >> 4) & 0x3ffc0ff;
    p->r[3] = (load32(key + 9) >> 6) & 0x3f03fff;
    p->r[4] = (load32(key + 12) >> 8) & 0x00fffff;
    memset(p->h, 0, sizeof p->h);
    for (int i = 0; i < 4; i++)
        p->pad[i] = load32(key + 16 + 4 * i);
}

/* One block of 16 bytes. hibit is 1 << 24 for a full block of the
 * message, 0 for a final block that carries its own padding byte. */
static void poly1305_block(struct poly1305 *p, const uint8_t m[16], uint32_t hibit)
{
    const uint32_t *r = p->r;
    uint32_t *h = p->h;
    uint32_t s1 = r[1] * 5, s2 = r[2] * 5, s3 = r[3] * 5, s4 = r[4] * 5;
    h[0] += load32(m + 0) & 0x3ffffff;
    h[1] += (load32(m + 3) >> 2) & 0x3ffffff;
    h[2] += (load32(m + 6) >> 4) & 0x3ffffff;
    h[3] += (load32(m + 9) >> 6) & 0x3ffffff;
    h[4] += (load32(m + 12) >> 8) | hibit;
    uint64_t d0 = (uint64_t)h[0] * r[0] + (uint64_t)h[1] * s4 + (uint64_t)h[2] * s3 + (uint64_t)h[3] * s2 +
                  (uint64_t)h[4] * s1;
    uint64_t d1 = (uint64_t)h[0] * r[1] + (uint64_t)h[1] * r[0] + (uint64_t)h[2] * s4 + (uint64_t)h[3] * s3 +
                  (uint64_t)h[4] * s2;
    uint64_t d2 = (uint64_t)h[0] * r[2] + (uint64_t)h[1] * r[1] + (uint64_t)h[2] * r[0] + (uint64_t)h[3] * s4 +
                  (uint64_t)h[4] * s3;
    uint64_t d3 = (uint64_t)h[0] * r[3] + (uint64_t)h[1] * r[2] + (uint64_t)h[2] * r[1] + (uint64_t)h[3] * r[0] +
                  (uint64_t)h[4] * s4;
    uint64_t d4 = (uint64_t)h[0] * r[4] + (uint64_t)h[1] * r[3] + (uint64_t)h[2] * r[2] + (uint64_t)h[3] * r[1] +
                  (uint64_t)h[4] * r[0];
    uint32_t c = (uint32_t)(d0 >> 26);
    h[0] = (uint32_t)d0 & 0x3ffffff;
    d1 += c;
    c = (uint32_t)(d1 >> 26);
    h[1] = (uint32_t)d1 & 0x3ffffff;
    d2 += c;
    c = (uint32_t)(d2 >> 26);
    h[2] = (uint32_t)d2 & 0x3ffffff;
    d3 += c;
    c = (uint32_t)(d3 >> 26);
    h[3] = (uint32_t)d3 & 0x3ffffff;
    d4 += c;
    c = (uint32_t)(d4 >> 26);
    h[4] = (uint32_t)d4 & 0x3ffffff;
    h[0] += c * 5;
    c = h[0] >> 26;
    h[0] &= 0x3ffffff;
    h[1] += c;
}

/* The data in blocks of 16 bytes, the last one padded with zeros, as the
 * AEAD construction pads the additional data and the ciphertext. */
static void poly1305_padded(struct poly1305 *p, const uint8_t *data, size_t len)
{
    while (len >= 16) {
        poly1305_block(p, data, 1u << 24);
        data += 16;
        len -= 16;
    }
    if (len) {
        uint8_t block[16] = { 0 };
        memcpy(block, data, len);
        poly1305_block(p, block, 1u << 24);
    }
}

static void poly1305_final(struct poly1305 *p, uint8_t mac[16])
{
    uint32_t *h = p->h, c;
    c = h[1] >> 26;
    h[1] &= 0x3ffffff;
    h[2] += c;
    c = h[2] >> 26;
    h[2] &= 0x3ffffff;
    h[3] += c;
    c = h[3] >> 26;
    h[3] &= 0x3ffffff;
    h[4] += c;
    c = h[4] >> 26;
    h[4] &= 0x3ffffff;
    h[0] += c * 5;
    c = h[0] >> 26;
    h[0] &= 0x3ffffff;
    h[1] += c;
    /* g = h - p; h is replaced by g when g does not borrow. */
    uint32_t g[5];
    g[0] = h[0] + 5;
    c = g[0] >> 26;
    g[0] &= 0x3ffffff;
    for (int i = 1; i < 4; i++) {
        g[i] = h[i] + c;
        c = g[i] >> 26;
        g[i] &= 0x3ffffff;
    }
    g[4] = h[4] + c - (1u << 26);
    uint32_t mask = (g[4] >> 31) - 1;
    for (int i = 0; i < 5; i++)
        h[i] = (h[i] & ~mask) | (g[i] & mask);
    /* h modulo 2^128 plus the pad. */
    uint32_t w0 = h[0] | h[1] << 26, w1 = h[1] >> 6 | h[2] << 20, w2 = h[2] >> 12 | h[3] << 14,
             w3 = h[3] >> 18 | h[4] << 8;
    uint64_t f = (uint64_t)w0 + p->pad[0];
    store32(mac, (uint32_t)f);
    f = (uint64_t)w1 + p->pad[1] + (f >> 32);
    store32(mac + 4, (uint32_t)f);
    f = (uint64_t)w2 + p->pad[2] + (f >> 32);
    store32(mac + 8, (uint32_t)f);
    f = (uint64_t)w3 + p->pad[3] + (f >> 32);
    store32(mac + 12, (uint32_t)f);
    crypto_wipe(p, sizeof *p);
}

static void aead_tag(const uint8_t key[32], const uint8_t nonce[12], const void *aad, size_t aad_len,
                     const uint8_t *ct, size_t len, uint8_t tag[16])
{
    uint8_t block0[64], lengths[16];
    chacha20_block(key, 0, nonce, block0);
    struct poly1305 p;
    poly1305_init(&p, block0);
    poly1305_padded(&p, aad, aad_len);
    poly1305_padded(&p, ct, len);
    store64(lengths, aad_len);
    store64(lengths + 8, len);
    poly1305_block(&p, lengths, 1u << 24);
    poly1305_final(&p, tag);
    crypto_wipe(block0, sizeof block0);
}

void chacha20_poly1305_seal(const uint8_t key[32], const uint8_t nonce[12], const void *aad, size_t aad_len,
                            const void *in, size_t len, uint8_t *out)
{
    chacha20_xor(key, 1, nonce, in, len, out);
    aead_tag(key, nonce, aad, aad_len, out, len, out + len);
}

int chacha20_poly1305_open(const uint8_t key[32], const uint8_t nonce[12], const void *aad, size_t aad_len,
                           const void *in, size_t len, const uint8_t tag[16], uint8_t *out)
{
    uint8_t want[16];
    aead_tag(key, nonce, aad, aad_len, in, len, want);
    if (crypto_memcmp(want, tag, 16) != 0) {
        memset(out, 0, len);
        return -1;
    }
    chacha20_xor(key, 1, nonce, in, len, out);
    return 0;
}
