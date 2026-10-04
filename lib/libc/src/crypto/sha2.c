/* This file implements SHA-256 and SHA-512 from the description in FIPS
 * 180-4 and RFC 6234. The round constants are the first 32 or 64 bits of the
 * fractional parts of the cube roots of the first 64 or 80 primes, the
 * initial values those of the square roots of the first eight primes.
 * Nothing here depends on the rest of libc, so tools/pkgsign compiles
 * this file on the host. */
#include <minios/sha2.h>
#include <string.h>

static const uint32_t k256[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static const uint64_t k512[80] = {
    0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full, 0xe9b5dba58189dbbcull,
    0x3956c25bf348b538ull, 0x59f111f1b605d019ull, 0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull,
    0xd807aa98a3030242ull, 0x12835b0145706fbeull, 0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull,
    0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull, 0xc19bf174cf692694ull,
    0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull, 0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull,
    0x2de92c6f592b0275ull, 0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull,
    0x983e5152ee66dfabull, 0xa831c66d2db43210ull, 0xb00327c898fb213full, 0xbf597fc7beef0ee4ull,
    0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull, 0x06ca6351e003826full, 0x142929670a0e6e70ull,
    0x27b70a8546d22ffcull, 0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull, 0x53380d139d95b3dfull,
    0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull, 0x92722c851482353bull,
    0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull, 0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull,
    0xd192e819d6ef5218ull, 0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull,
    0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull, 0x2748774cdf8eeb99ull, 0x34b0bcb5e19b48a8ull,
    0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull, 0x5b9cca4f7763e373ull, 0x682e6ff3d6b2b8a3ull,
    0x748f82ee5defb2fcull, 0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
    0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull, 0xc67178f2e372532bull,
    0xca273eceea26619cull, 0xd186b8c721c0c207ull, 0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull,
    0x06f067aa72176fbaull, 0x0a637dc5a2c898a6ull, 0x113f9804bef90daeull, 0x1b710b35131c471bull,
    0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull, 0x431d67c49c100d4cull,
    0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull, 0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull,
};

static uint32_t ror32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static uint64_t ror64(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

static uint32_t load32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint64_t load64(const uint8_t *p)
{
    return (uint64_t)load32(p) << 32 | load32(p + 4);
}

static void store32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void store64(uint8_t *p, uint64_t v)
{
    store32(p, (uint32_t)(v >> 32));
    store32(p + 4, (uint32_t)v);
}

static void sha256_block(uint32_t h[8], const uint8_t *block)
{
    uint32_t w[64];
    for (int t = 0; t < 16; t++)
        w[t] = load32(block + 4 * t);
    for (int t = 16; t < 64; t++) {
        uint32_t s0 = ror32(w[t - 15], 7) ^ ror32(w[t - 15], 18) ^ (w[t - 15] >> 3);
        uint32_t s1 = ror32(w[t - 2], 17) ^ ror32(w[t - 2], 19) ^ (w[t - 2] >> 10);
        w[t] = w[t - 16] + s0 + w[t - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int t = 0; t < 64; t++) {
        uint32_t t1 = hh + (ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25)) + ((e & f) ^ (~e & g)) + k256[t] + w[t];
        uint32_t t2 = (ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

void sha256_init(struct sha256_ctx *c)
{
    static const uint32_t iv[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    };
    memcpy(c->h, iv, sizeof iv);
    c->length = 0;
    c->used = 0;
}

void sha256_update(struct sha256_ctx *c, const void *data, size_t len)
{
    const uint8_t *p = data;
    c->length += len;
    if (c->used) {
        size_t take = 64 - c->used < len ? 64 - c->used : len;
        memcpy(c->block + c->used, p, take);
        c->used += take;
        p += take;
        len -= take;
        if (c->used < 64)
            return;
        sha256_block(c->h, c->block);
        c->used = 0;
    }
    for (; len >= 64; p += 64, len -= 64)
        sha256_block(c->h, p);
    memcpy(c->block, p, len);
    c->used = len;
}

/* The padding appends one bit, zeros up to 56 bytes modulo 64, and the
 * message length in bits as a big endian 64 bit number. */
void sha256_final(struct sha256_ctx *c, uint8_t digest[SHA256_DIGEST_SIZE])
{
    uint64_t bits = c->length * 8;
    c->block[c->used++] = 0x80;
    if (c->used > 56) {
        memset(c->block + c->used, 0, 64 - c->used);
        sha256_block(c->h, c->block);
        c->used = 0;
    }
    memset(c->block + c->used, 0, 56 - c->used);
    store64(c->block + 56, bits);
    sha256_block(c->h, c->block);
    for (int i = 0; i < 8; i++)
        store32(digest + 4 * i, c->h[i]);
    memset(c, 0, sizeof *c);
}

void sha256(const void *data, size_t len, uint8_t digest[SHA256_DIGEST_SIZE])
{
    struct sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, data, len);
    sha256_final(&c, digest);
}

static void sha512_block(uint64_t h[8], const uint8_t *block)
{
    uint64_t w[80];
    for (int t = 0; t < 16; t++)
        w[t] = load64(block + 8 * t);
    for (int t = 16; t < 80; t++) {
        uint64_t s0 = ror64(w[t - 15], 1) ^ ror64(w[t - 15], 8) ^ (w[t - 15] >> 7);
        uint64_t s1 = ror64(w[t - 2], 19) ^ ror64(w[t - 2], 61) ^ (w[t - 2] >> 6);
        w[t] = w[t - 16] + s0 + w[t - 7] + s1;
    }
    uint64_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int t = 0; t < 80; t++) {
        uint64_t t1 = hh + (ror64(e, 14) ^ ror64(e, 18) ^ ror64(e, 41)) + ((e & f) ^ (~e & g)) + k512[t] + w[t];
        uint64_t t2 = (ror64(a, 28) ^ ror64(a, 34) ^ ror64(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

void sha512_init(struct sha512_ctx *c)
{
    static const uint64_t iv[8] = {
        0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull, 0x3c6ef372fe94f82bull, 0xa54ff53a5f1d36f1ull,
        0x510e527fade682d1ull, 0x9b05688c2b3e6c1full, 0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull,
    };
    memcpy(c->h, iv, sizeof iv);
    c->length = 0;
    c->used = 0;
}

void sha512_update(struct sha512_ctx *c, const void *data, size_t len)
{
    const uint8_t *p = data;
    c->length += len;
    if (c->used) {
        size_t take = 128 - c->used < len ? 128 - c->used : len;
        memcpy(c->block + c->used, p, take);
        c->used += take;
        p += take;
        len -= take;
        if (c->used < 128)
            return;
        sha512_block(c->h, c->block);
        c->used = 0;
    }
    for (; len >= 128; p += 128, len -= 128)
        sha512_block(c->h, p);
    memcpy(c->block, p, len);
    c->used = len;
}

/* The length field is 128 bits wide. Its upper half receives the three
 * bits of the byte count that the conversion to bits shifts out. */
void sha512_final(struct sha512_ctx *c, uint8_t digest[SHA512_DIGEST_SIZE])
{
    uint64_t bits = c->length * 8;
    c->block[c->used++] = 0x80;
    if (c->used > 112) {
        memset(c->block + c->used, 0, 128 - c->used);
        sha512_block(c->h, c->block);
        c->used = 0;
    }
    memset(c->block + c->used, 0, 112 - c->used);
    store64(c->block + 112, c->length >> 61);
    store64(c->block + 120, bits);
    sha512_block(c->h, c->block);
    for (int i = 0; i < 8; i++)
        store64(digest + 8 * i, c->h[i]);
    memset(c, 0, sizeof *c);
}

void sha512(const void *data, size_t len, uint8_t digest[SHA512_DIGEST_SIZE])
{
    struct sha512_ctx c;
    sha512_init(&c);
    sha512_update(&c, data, len);
    sha512_final(&c, digest);
}
