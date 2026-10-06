#pragma once
/* Arithmetic modulo p = 2^255 - 19 for ed25519.c and x25519.c. Field
 * elements are five limbs of 51 bits; products use 128 bit
 * intermediates. The functions do not branch on the values, except
 * fe_pow on its public exponent. */
#include <stdint.h>
#include <string.h>

#define MASK51 ((UINT64_C(1) << 51) - 1)

struct fe { uint64_t v[5]; };

/* The functions below compute in the field of integers modulo p. */

static inline void fe_set(struct fe *h, uint64_t n)
{
    memset(h, 0, sizeof *h);
    h->v[0] = n;
}

/* fe_carry brings every limb below 2^51, folding the carry out of the top limb
 * back in as 19 times its value, since 2^255 = 19 modulo p. */
static inline void fe_carry(struct fe *h)
{
    uint64_t c;
    for (int i = 0; i < 4; i++) {
        c = h->v[i] >> 51;
        h->v[i] &= MASK51;
        h->v[i + 1] += c;
    }
    c = h->v[4] >> 51;
    h->v[4] &= MASK51;
    h->v[0] += 19 * c;
    c = h->v[0] >> 51;
    h->v[0] &= MASK51;
    h->v[1] += c;
}

static inline void fe_add(struct fe *h, const struct fe *f, const struct fe *g)
{
    for (int i = 0; i < 5; i++)
        h->v[i] = f->v[i] + g->v[i];
    fe_carry(h);
}

/* The difference f - g is computed as f + 4p - g, which preserves every limb
 * positive for carried inputs. */
static inline void fe_sub(struct fe *h, const struct fe *f, const struct fe *g)
{
    static const uint64_t four_p[5] = {
        4 * (MASK51 - 18), 4 * MASK51, 4 * MASK51, 4 * MASK51, 4 * MASK51,
    };
    for (int i = 0; i < 5; i++)
        h->v[i] = f->v[i] + four_p[i] - g->v[i];
    fe_carry(h);
}

static inline void fe_neg(struct fe *h, const struct fe *f)
{
    struct fe zero;
    fe_set(&zero, 0);
    fe_sub(h, &zero, f);
}

static inline void fe_mul(struct fe *h, const struct fe *f, const struct fe *g)
{
    const uint64_t *a = f->v, *b = g->v;
    uint64_t b19[5];
    for (int i = 1; i < 5; i++)
        b19[i] = 19 * b[i];
    unsigned __int128 r0 = (unsigned __int128)a[0] * b[0] + (unsigned __int128)a[1] * b19[4] +
                           (unsigned __int128)a[2] * b19[3] + (unsigned __int128)a[3] * b19[2] +
                           (unsigned __int128)a[4] * b19[1];
    unsigned __int128 r1 = (unsigned __int128)a[0] * b[1] + (unsigned __int128)a[1] * b[0] +
                           (unsigned __int128)a[2] * b19[4] + (unsigned __int128)a[3] * b19[3] +
                           (unsigned __int128)a[4] * b19[2];
    unsigned __int128 r2 = (unsigned __int128)a[0] * b[2] + (unsigned __int128)a[1] * b[1] +
                           (unsigned __int128)a[2] * b[0] + (unsigned __int128)a[3] * b19[4] +
                           (unsigned __int128)a[4] * b19[3];
    unsigned __int128 r3 = (unsigned __int128)a[0] * b[3] + (unsigned __int128)a[1] * b[2] +
                           (unsigned __int128)a[2] * b[1] + (unsigned __int128)a[3] * b[0] +
                           (unsigned __int128)a[4] * b19[4];
    unsigned __int128 r4 = (unsigned __int128)a[0] * b[4] + (unsigned __int128)a[1] * b[3] +
                           (unsigned __int128)a[2] * b[2] + (unsigned __int128)a[3] * b[1] +
                           (unsigned __int128)a[4] * b[0];
    r1 += (uint64_t)(r0 >> 51);
    r2 += (uint64_t)(r1 >> 51);
    r3 += (uint64_t)(r2 >> 51);
    r4 += (uint64_t)(r3 >> 51);
    h->v[0] = ((uint64_t)r0 & MASK51) + 19 * (uint64_t)(r4 >> 51);
    h->v[1] = (uint64_t)r1 & MASK51;
    h->v[2] = (uint64_t)r2 & MASK51;
    h->v[3] = (uint64_t)r3 & MASK51;
    h->v[4] = (uint64_t)r4 & MASK51;
    h->v[1] += h->v[0] >> 51;
    h->v[0] &= MASK51;
}

static inline void fe_sq(struct fe *h, const struct fe *f)
{
    fe_mul(h, f, f);
}

/* fe_pow raises f to a 256 bit exponent given little endian. The
 * exponents are public constants. */
static inline void fe_pow(struct fe *h, const struct fe *f, const uint8_t e[32])
{
    struct fe r, base = *f;
    fe_set(&r, 1);
    for (int i = 255; i >= 0; i--) {
        fe_sq(&r, &r);
        if ((e[i / 8] >> (i % 8)) & 1)
            fe_mul(&r, &r, &base);
    }
    *h = r;
}

/* The exponents p - 2, (p - 5) / 8 and (p - 1) / 4 have all bits set
 * except in their lowest and highest bytes. */
static inline void exponent(uint8_t e[32], uint8_t low, uint8_t high)
{
    memset(e, 0xff, 32);
    e[0] = low;
    e[31] = high;
}

static inline void fe_inv(struct fe *h, const struct fe *f)
{
    uint8_t e[32];
    exponent(e, 0xeb, 0x7f);            /* This is p - 2. */
    fe_pow(h, f, e);
}

static inline uint64_t load64_le(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = v << 8 | p[i];
    return v;
}

static inline void store64_le(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

/* fe_frombytes reads the low 255 bits of s and leaves bit 255 to the
 * caller. */
static inline void fe_frombytes(struct fe *h, const uint8_t s[32])
{
    h->v[0] = load64_le(s) & MASK51;
    h->v[1] = (load64_le(s + 6) >> 3) & MASK51;
    h->v[2] = (load64_le(s + 12) >> 6) & MASK51;
    h->v[3] = (load64_le(s + 19) >> 1) & MASK51;
    h->v[4] = (load64_le(s + 24) >> 12) & MASK51;
}

/* fe_tobytes writes the canonical encoding, whose value is below p. */
static inline void fe_tobytes(uint8_t s[32], const struct fe *f)
{
    struct fe t = *f;
    fe_carry(&t);
    fe_carry(&t);
    /* t is now below 2p. q is 1 exactly when t + 19 reaches 2^255, that
     * is when t is at least p; adding 19 and dropping bit 255 then
     * subtracts p. */
    uint64_t q = (t.v[0] + 19) >> 51;
    for (int i = 1; i < 5; i++)
        q = (t.v[i] + q) >> 51;
    t.v[0] += 19 * q;
    for (int i = 0; i < 4; i++) {
        t.v[i + 1] += t.v[i] >> 51;
        t.v[i] &= MASK51;
    }
    t.v[4] &= MASK51;
    store64_le(s, t.v[0] | t.v[1] << 51);
    store64_le(s + 8, t.v[1] >> 13 | t.v[2] << 38);
    store64_le(s + 16, t.v[2] >> 26 | t.v[3] << 25);
    store64_le(s + 24, t.v[3] >> 39 | t.v[4] << 12);
}

static inline int fe_equal(const struct fe *f, const struct fe *g)
{
    uint8_t a[32], b[32];
    fe_tobytes(a, f);
    fe_tobytes(b, g);
    return memcmp(a, b, 32) == 0;
}

static inline int fe_iszero(const struct fe *f)
{
    struct fe zero;
    fe_set(&zero, 0);
    return fe_equal(f, &zero);
}

static inline int fe_isodd(const struct fe *f)
{
    uint8_t s[32];
    fe_tobytes(s, f);
    return s[0] & 1;
}
