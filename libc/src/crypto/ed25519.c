/* This file implements Ed25519 (RFC 8032, section 5.1) from the
 * description in the RFC. Field elements modulo p = 2^255 - 19 are five limbs of 51 bits;
 * products use 128 bit intermediates. Points are kept in the extended
 * homogeneous coordinates (X:Y:Z:T) of the twisted Edwards curve
 * -x^2 + y^2 = 1 + d x^2 y^2, with the addition and doubling formulas of
 * section 5.1.4. Scalars modulo the group order L are reduced bit by bit.
 *
 * The constants d, sqrt(-1) and the base point are computed from their
 * definitions on every call rather than written out: d = -121665/121666,
 * sqrt(-1) = 2^((p-1)/4), and the base point is the point with y = 4/5
 * and an even x. The cost is a few exponentiations, small beside the
 * scalar multiplications.
 *
 * Scalar multiplication runs the same sequence of operations for every
 * scalar and selects with masks, so signing on the host does not branch
 * on the secret. Verification handles public data only. The file depends
 * on sha2.c and string.h alone, so tools/pkgsign compiles it on the host. */
#include <minios/ed25519.h>
#include <minios/sha2.h>
#include <string.h>

#define MASK51 ((UINT64_C(1) << 51) - 1)

struct fe { uint64_t v[5]; };
struct point { struct fe x, y, z, t; };
struct curve {
    struct fe d, d2, sqrtm1;
    struct point base;
};

/* The functions below compute in the field of integers modulo p. */

static void fe_set(struct fe *h, uint64_t n)
{
    memset(h, 0, sizeof *h);
    h->v[0] = n;
}

/* fe_carry brings every limb below 2^51, folding the carry out of the top limb
 * back in as 19 times its value, since 2^255 = 19 modulo p. */
static void fe_carry(struct fe *h)
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

static void fe_add(struct fe *h, const struct fe *f, const struct fe *g)
{
    for (int i = 0; i < 5; i++)
        h->v[i] = f->v[i] + g->v[i];
    fe_carry(h);
}

/* The difference f - g is computed as f + 4p - g, which keeps every limb
 * positive for carried inputs. */
static void fe_sub(struct fe *h, const struct fe *f, const struct fe *g)
{
    static const uint64_t four_p[5] = {
        4 * (MASK51 - 18), 4 * MASK51, 4 * MASK51, 4 * MASK51, 4 * MASK51,
    };
    for (int i = 0; i < 5; i++)
        h->v[i] = f->v[i] + four_p[i] - g->v[i];
    fe_carry(h);
}

static void fe_neg(struct fe *h, const struct fe *f)
{
    struct fe zero;
    fe_set(&zero, 0);
    fe_sub(h, &zero, f);
}

static void fe_mul(struct fe *h, const struct fe *f, const struct fe *g)
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

static void fe_sq(struct fe *h, const struct fe *f)
{
    fe_mul(h, f, f);
}

/* fe_pow raises f to a 256 bit exponent given little endian. The
 * exponents are public constants. */
static void fe_pow(struct fe *h, const struct fe *f, const uint8_t e[32])
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
static void exponent(uint8_t e[32], uint8_t low, uint8_t high)
{
    memset(e, 0xff, 32);
    e[0] = low;
    e[31] = high;
}

static void fe_inv(struct fe *h, const struct fe *f)
{
    uint8_t e[32];
    exponent(e, 0xeb, 0x7f);            /* This is p - 2. */
    fe_pow(h, f, e);
}

static uint64_t load64_le(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = v << 8 | p[i];
    return v;
}

static void store64_le(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

/* fe_frombytes reads the low 255 bits of s and leaves bit 255 to the
 * caller. */
static void fe_frombytes(struct fe *h, const uint8_t s[32])
{
    h->v[0] = load64_le(s) & MASK51;
    h->v[1] = (load64_le(s + 6) >> 3) & MASK51;
    h->v[2] = (load64_le(s + 12) >> 6) & MASK51;
    h->v[3] = (load64_le(s + 19) >> 1) & MASK51;
    h->v[4] = (load64_le(s + 24) >> 12) & MASK51;
}

/* fe_tobytes writes the canonical encoding, whose value is below p. */
static void fe_tobytes(uint8_t s[32], const struct fe *f)
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

static int fe_equal(const struct fe *f, const struct fe *g)
{
    uint8_t a[32], b[32];
    fe_tobytes(a, f);
    fe_tobytes(b, g);
    return memcmp(a, b, 32) == 0;
}

static int fe_iszero(const struct fe *f)
{
    struct fe zero;
    fe_set(&zero, 0);
    return fe_equal(f, &zero);
}

static int fe_isodd(const struct fe *f)
{
    uint8_t s[32];
    fe_tobytes(s, f);
    return s[0] & 1;
}

/* The functions below compute with points of the curve. */

static void point_identity(struct point *p)
{
    fe_set(&p->x, 0);
    fe_set(&p->y, 1);
    fe_set(&p->z, 1);
    fe_set(&p->t, 0);
}

/* point_add follows section 5.1.4. The formula is complete, so it also
 * doubles. */
static void point_add(struct point *r, const struct point *p, const struct point *q, const struct curve *c)
{
    struct fe a, b, cc, d, e, f, g, h, t;
    fe_sub(&a, &p->y, &p->x);
    fe_sub(&t, &q->y, &q->x);
    fe_mul(&a, &a, &t);
    fe_add(&b, &p->y, &p->x);
    fe_add(&t, &q->y, &q->x);
    fe_mul(&b, &b, &t);
    fe_mul(&cc, &p->t, &q->t);
    fe_mul(&cc, &cc, &c->d2);
    fe_mul(&d, &p->z, &q->z);
    fe_add(&d, &d, &d);
    fe_sub(&e, &b, &a);
    fe_sub(&f, &d, &cc);
    fe_add(&g, &d, &cc);
    fe_add(&h, &b, &a);
    fe_mul(&r->x, &e, &f);
    fe_mul(&r->y, &g, &h);
    fe_mul(&r->t, &e, &h);
    fe_mul(&r->z, &f, &g);
}

/* point_double follows section 5.1.4. */
static void point_double(struct point *r, const struct point *p)
{
    struct fe a, b, c, e, f, g, h, t;
    fe_sq(&a, &p->x);
    fe_sq(&b, &p->y);
    fe_sq(&c, &p->z);
    fe_add(&c, &c, &c);
    fe_add(&h, &a, &b);
    fe_add(&t, &p->x, &p->y);
    fe_sq(&t, &t);
    fe_sub(&e, &h, &t);
    fe_sub(&g, &a, &b);
    fe_add(&f, &c, &g);
    fe_mul(&r->x, &e, &f);
    fe_mul(&r->y, &g, &h);
    fe_mul(&r->t, &e, &h);
    fe_mul(&r->z, &f, &g);
}

/* point_select sets r to q when bit is 1 and leaves it when bit is 0,
 * without a branch. */
static void point_select(struct point *r, const struct point *q, unsigned bit)
{
    uint64_t mask = (uint64_t)0 - bit;
    uint64_t *dst = &r->x.v[0];
    const uint64_t *src = &q->x.v[0];
    for (int i = 0; i < 20; i++)
        dst[i] = (dst[i] & ~mask) | (src[i] & mask);
}

/* point_mul computes [s]p for a 256 bit little endian scalar s. */
static void point_mul(struct point *r, const uint8_t s[32], const struct point *p, const struct curve *c)
{
    struct point q, t;
    point_identity(&q);
    for (int i = 255; i >= 0; i--) {
        point_double(&q, &q);
        point_add(&t, &q, p, c);
        point_select(&q, &t, (s[i / 8] >> (i % 8)) & 1);
    }
    *r = q;
}

/* point_encode follows section 5.1.2. It writes y and puts the low bit
 * of x in bit 255. */
static void point_encode(uint8_t s[32], const struct point *p)
{
    struct fe zinv, x, y;
    fe_inv(&zinv, &p->z);
    fe_mul(&x, &p->x, &zinv);
    fe_mul(&y, &p->y, &zinv);
    fe_tobytes(s, &y);
    s[31] |= (uint8_t)(fe_isodd(&x) << 7);
}

/* point_decode follows section 5.1.3. It returns 0 on success and -1
 * when s is not the canonical encoding of a curve point. */
static int point_decode(struct point *p, const uint8_t s[32], const struct curve *c)
{
    uint8_t y_bytes[32], check[32], e[32];
    memcpy(y_bytes, s, 32);
    int sign = y_bytes[31] >> 7;
    y_bytes[31] &= 0x7f;
    fe_frombytes(&p->y, y_bytes);
    fe_tobytes(check, &p->y);
    if (memcmp(check, y_bytes, 32) != 0)
        return -1;                      /* y is not below p. */
    /* x^2 = u / v with u = y^2 - 1 and v = d y^2 + 1. The candidate root
     * is u v^3 (u v^7)^((p-5)/8). */
    struct fe one, u, v, v3, v7, x, vx2, t;
    fe_set(&one, 1);
    fe_sq(&u, &p->y);
    fe_mul(&v, &u, &c->d);
    fe_sub(&u, &u, &one);
    fe_add(&v, &v, &one);
    fe_sq(&v3, &v);
    fe_mul(&v3, &v3, &v);
    fe_sq(&v7, &v3);
    fe_mul(&v7, &v7, &v);
    fe_mul(&t, &u, &v7);
    exponent(e, 0xfd, 0x0f);            /* This is (p - 5) / 8. */
    fe_pow(&t, &t, e);
    fe_mul(&x, &u, &v3);
    fe_mul(&x, &x, &t);
    fe_sq(&vx2, &x);
    fe_mul(&vx2, &vx2, &v);
    if (!fe_equal(&vx2, &u)) {
        struct fe minus_u;
        fe_neg(&minus_u, &u);
        if (!fe_equal(&vx2, &minus_u))
            return -1;                  /* u / v has no square root. */
        fe_mul(&x, &x, &c->sqrtm1);
    }
    if (fe_iszero(&x) && sign)
        return -1;
    if (fe_isodd(&x) != sign)
        fe_neg(&x, &x);
    p->x = x;
    fe_set(&p->z, 1);
    fe_mul(&p->t, &p->x, &p->y);
    return 0;
}

static void curve_init(struct curve *c)
{
    struct fe n, inv;
    uint8_t e[32], enc[32];
    fe_set(&n, 121666);
    fe_inv(&inv, &n);
    fe_set(&n, 121665);
    fe_neg(&n, &n);
    fe_mul(&c->d, &n, &inv);
    fe_add(&c->d2, &c->d, &c->d);
    fe_set(&n, 2);
    exponent(e, 0xfb, 0x1f);            /* This is (p - 1) / 4. */
    fe_pow(&c->sqrtm1, &n, e);
    /* The base point has y = 4/5 and an even x, which is sign bit 0. */
    fe_set(&n, 5);
    fe_inv(&inv, &n);
    fe_set(&n, 4);
    fe_mul(&n, &n, &inv);
    fe_tobytes(enc, &n);
    point_decode(&c->base, enc, c);
}

/* The functions below compute with scalars modulo the group order
 * L = 2^252 + 27742317777372353535851937790883648493. */

static const uint8_t order[32] = {
    0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10,
};

/* sc_reduce reduces n little endian bytes of in modulo L. The remainder
 * is doubled and receives the next bit, from the top, and L is subtracted
 * whenever the result reaches it; a mask selects the subtraction. */
static void sc_reduce(uint8_t out[32], const uint8_t *in, size_t n)
{
    uint32_t r[9] = {0}, l[9] = {0}, t[9];
    for (int i = 0; i < 32; i++)
        l[i / 4] |= (uint32_t)order[i] << (8 * (i % 4));
    for (long bit = (long)n * 8 - 1; bit >= 0; bit--) {
        for (int i = 8; i > 0; i--)
            r[i] = r[i] << 1 | r[i - 1] >> 31;
        r[0] = r[0] << 1 | ((in[bit / 8] >> (bit % 8)) & 1);
        uint64_t borrow = 0;
        for (int i = 0; i < 9; i++) {
            uint64_t d = (uint64_t)r[i] - l[i] - borrow;
            t[i] = (uint32_t)d;
            borrow = (d >> 32) & 1;
        }
        uint32_t keep = (uint32_t)0 - (uint32_t)borrow;     /* keep is all ones when r < L. */
        for (int i = 0; i < 9; i++)
            r[i] = (r[i] & keep) | (t[i] & ~keep);
    }
    for (int i = 0; i < 32; i++)
        out[i] = (uint8_t)(r[i / 4] >> (8 * (i % 4)));
}

/* sc_muladd computes (a + b * c) modulo L for 32 byte little endian a,
 * b and c. */
static void sc_muladd(uint8_t out[32], const uint8_t a[32], const uint8_t b[32], const uint8_t c[32])
{
    uint32_t x[8], y[8], prod[16] = {0};
    for (int i = 0; i < 8; i++) {
        x[i] = y[i] = 0;
        for (int k = 0; k < 4; k++) {
            x[i] |= (uint32_t)b[4 * i + k] << (8 * k);
            y[i] |= (uint32_t)c[4 * i + k] << (8 * k);
        }
    }
    for (int i = 0; i < 8; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < 8; j++) {
            uint64_t v = (uint64_t)x[i] * y[j] + prod[i + j] + carry;
            prod[i + j] = (uint32_t)v;
            carry = v >> 32;
        }
        prod[i + 8] = (uint32_t)carry;
    }
    uint64_t carry = 0;
    for (int i = 0; i < 16; i++) {
        uint64_t v = (uint64_t)prod[i] + carry;
        if (i < 8)
            v += (uint64_t)a[4 * i] | (uint64_t)a[4 * i + 1] << 8 | (uint64_t)a[4 * i + 2] << 16 | (uint64_t)a[4 * i + 3] << 24;
        prod[i] = (uint32_t)v;
        carry = v >> 32;
    }
    uint8_t wide[64];
    for (int i = 0; i < 64; i++)
        wide[i] = (uint8_t)(prod[i / 4] >> (8 * (i % 4)));
    sc_reduce(out, wide, sizeof wide);
}

/* sc_canonical returns 1 when the little endian s is below L. */
static int sc_canonical(const uint8_t s[32])
{
    for (int i = 31; i >= 0; i--) {
        if (s[i] < order[i])
            return 1;
        if (s[i] > order[i])
            return 0;
    }
    return 0;
}

/* The functions below derive keys, sign and verify. */

/* expand_seed follows section 5.1.5. The first half of SHA-512(seed),
 * clamped, is the secret scalar, and the second half is the prefix that
 * makes signing deterministic. */
static void expand_seed(uint8_t scalar[32], uint8_t prefix[32], const uint8_t seed[32])
{
    uint8_t h[64];
    sha512(seed, 32, h);
    memcpy(scalar, h, 32);
    memcpy(prefix, h + 32, 32);
    scalar[0] &= 248;
    scalar[31] &= 127;
    scalar[31] |= 64;
    memset(h, 0, sizeof h);
}

void ed25519_public_key(uint8_t pub[ED25519_PUBLIC_SIZE], const uint8_t seed[ED25519_SEED_SIZE])
{
    struct curve c;
    struct point a;
    uint8_t scalar[32], prefix[32];
    curve_init(&c);
    expand_seed(scalar, prefix, seed);
    point_mul(&a, scalar, &c.base, &c);
    point_encode(pub, &a);
    memset(scalar, 0, sizeof scalar);
    memset(prefix, 0, sizeof prefix);
}

/* ed25519_sign follows section 5.1.6. */
void ed25519_sign(uint8_t sig[ED25519_SIGNATURE_SIZE], const void *msg, size_t len,
                  const uint8_t seed[ED25519_SEED_SIZE])
{
    struct curve c;
    struct point p;
    struct sha512_ctx h;
    uint8_t scalar[32], prefix[32], pub[32], digest[64], r[32], k[32];
    curve_init(&c);
    expand_seed(scalar, prefix, seed);
    point_mul(&p, scalar, &c.base, &c);
    point_encode(pub, &p);
    sha512_init(&h);
    sha512_update(&h, prefix, 32);
    sha512_update(&h, msg, len);
    sha512_final(&h, digest);
    sc_reduce(r, digest, sizeof digest);
    point_mul(&p, r, &c.base, &c);
    point_encode(sig, &p);
    sha512_init(&h);
    sha512_update(&h, sig, 32);
    sha512_update(&h, pub, 32);
    sha512_update(&h, msg, len);
    sha512_final(&h, digest);
    sc_reduce(k, digest, sizeof digest);
    sc_muladd(sig + 32, r, k, scalar);
    memset(scalar, 0, sizeof scalar);
    memset(prefix, 0, sizeof prefix);
    memset(r, 0, sizeof r);
}

/* ed25519_verify follows section 5.1.7. It carries out the check
 * [S]B = R + [k]A as the comparison of the encoding of [S]B + [k](-A)
 * with the bytes of R. */
int ed25519_verify(const uint8_t sig[ED25519_SIGNATURE_SIZE], const void *msg, size_t len,
                   const uint8_t pub[ED25519_PUBLIC_SIZE])
{
    struct curve c;
    struct point a, sb, ka;
    struct sha512_ctx h;
    uint8_t digest[64], k[32], enc[32];
    if (!sc_canonical(sig + 32))
        return 0;
    curve_init(&c);
    if (point_decode(&a, pub, &c) < 0)
        return 0;
    fe_neg(&a.x, &a.x);
    fe_neg(&a.t, &a.t);
    sha512_init(&h);
    sha512_update(&h, sig, 32);
    sha512_update(&h, pub, 32);
    sha512_update(&h, msg, len);
    sha512_final(&h, digest);
    sc_reduce(k, digest, sizeof digest);
    point_mul(&sb, sig + 32, &c.base, &c);
    point_mul(&ka, k, &a, &c);
    point_add(&sb, &sb, &ka, &c);
    point_encode(enc, &sb);
    return memcmp(enc, sig, 32) == 0;
}
