/* The curves P-256 and P-384 (FIPS 186-4, SEC 1) for minios/crypto.h:
 * ECDSA verification, and the public key and the shared secret of ECDH
 * on P-256. Points are in Jacobian coordinates (X : Y : Z), which stand
 * for (X / Z^2, Y / Z^3); Z = 0 is the point at infinity. Coordinates are
 * in Montgomery form modulo p (bignum.h). Doubling and addition use the
 * formulas dbl-2001-b and add-2007-bl of the Explicit-Formulas Database
 * for curves with a = -3.
 *
 * The scalar multiplication of ECDH performs an addition for every bit
 * and keeps or discards its result with masks. The addition formulas
 * still branch for equal points and the point at infinity, which the
 * scalar decides only at the first set bit. */
#include <minios/crypto.h>
#include <string.h>
#include "bignum.h"

#define L 6                             /* limbs of P-384, the larger curve */

struct curve {
    int n;                              /* limbs */
    size_t size;                        /* bytes of a coordinate */
    struct mont p, q;                   /* the field and the group order */
    uint64_t b[L], gx[L], gy[L];        /* Montgomery form */
};

struct jac {
    uint64_t x[L], y[L], z[L];
};

static int hexval(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : 0;
}

/* A constant written in hexadecimal into n limbs. */
static void from_hex(uint64_t *a, int n, const char *hex)
{
    uint8_t bytes[48];
    size_t len = strlen(hex) / 2;
    for (size_t i = 0; i < len; i++)
        bytes[i] = (uint8_t)(hexval(hex[2 * i]) << 4 | hexval(hex[2 * i + 1]));
    bn_from_bytes(a, n, bytes, len);
}

static void curve_init(struct curve *c, enum ec_curve id)
{
    static const char *const p256[] = {
        "ffffffff00000001000000000000000000000000ffffffffffffffffffffffff",
        "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551",
        "5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b",
        "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296",
        "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5",
    };
    static const char *const p384[] = {
        "fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffeffffffff0000000000000000ffffffff",
        "ffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77aecec196accc52973",
        "b3312fa7e23ee7e4988e056be3f82d19181d9c6efe8141120314088f5013875ac656398d8a2ed19d2a85c8edd3ec2aef",
        "aa87ca22be8b05378eb1c71ef320ad746e1d3b628ba79b9859f741e082542a385502f25dbf55296c3a545e3872760ab7",
        "3617de4a96262c6f5d9e98bf9292dc29f8f41dbd289a147ce9da3113b5f0b8c00a60b1ce1d7e819d7a431d7c90ea0e5f",
    };
    const char *const *k = id == EC_P256 ? p256 : p384;
    c->n = id == EC_P256 ? 4 : 6;
    c->size = id == EC_P256 ? 32 : 48;
    uint64_t v[L];
    from_hex(v, c->n, k[0]);
    mont_init(&c->p, v, c->n);
    from_hex(v, c->n, k[1]);
    mont_init(&c->q, v, c->n);
    from_hex(v, c->n, k[2]);
    mont_to(&c->p, c->b, v);
    from_hex(v, c->n, k[3]);
    mont_to(&c->p, c->gx, v);
    from_hex(v, c->n, k[4]);
    mont_to(&c->p, c->gy, v);
}

size_t ec_size(enum ec_curve curve)
{
    return curve == EC_P256 ? 32 : 48;
}

/* ---- field shorthands ---- */

#define MUL(r, a, b) mont_mul(&c->p, r, a, b)
#define ADD(r, a, b) mont_add(&c->p, r, a, b)
#define SUB(r, a, b) mont_sub(&c->p, r, a, b)

static int is_infinity(const struct curve *c, const struct jac *p)
{
    return bn_is_zero(p->z, c->n);
}

static void set_infinity(struct jac *p)
{
    memset(p, 0, sizeof *p);
}

static void point_double(const struct curve *c, struct jac *r, const struct jac *p)
{
    if (is_infinity(c, p)) {
        set_infinity(r);
        return;
    }
    uint64_t delta[L], gamma[L], beta[L], alpha[L], t1[L], t2[L], x3[L], y3[L], z3[L];
    MUL(delta, p->z, p->z);
    MUL(gamma, p->y, p->y);
    MUL(beta, p->x, gamma);
    SUB(t1, p->x, delta);
    ADD(t2, p->x, delta);
    MUL(t1, t1, t2);
    ADD(alpha, t1, t1);
    ADD(alpha, alpha, t1);                  /* alpha = 3 (x - delta)(x + delta) */
    MUL(x3, alpha, alpha);
    ADD(t1, beta, beta);
    ADD(t1, t1, t1);                        /* 4 beta */
    ADD(t2, t1, t1);                        /* 8 beta */
    SUB(x3, x3, t2);
    ADD(z3, p->y, p->z);
    MUL(z3, z3, z3);
    SUB(z3, z3, gamma);
    SUB(z3, z3, delta);
    SUB(t1, t1, x3);
    MUL(y3, alpha, t1);
    MUL(t2, gamma, gamma);
    ADD(t2, t2, t2);
    ADD(t2, t2, t2);
    ADD(t2, t2, t2);                        /* 8 gamma^2 */
    SUB(y3, y3, t2);
    memcpy(r->x, x3, sizeof x3);
    memcpy(r->y, y3, sizeof y3);
    memcpy(r->z, z3, sizeof z3);
}

static void point_add(const struct curve *c, struct jac *r, const struct jac *p, const struct jac *q)
{
    if (is_infinity(c, p)) {
        *r = *q;
        return;
    }
    if (is_infinity(c, q)) {
        *r = *p;
        return;
    }
    uint64_t z1z1[L], z2z2[L], u1[L], u2[L], s1[L], s2[L], h[L], i[L], j[L], rr[L], v[L], t[L];
    MUL(z1z1, p->z, p->z);
    MUL(z2z2, q->z, q->z);
    MUL(u1, p->x, z2z2);
    MUL(u2, q->x, z1z1);
    MUL(s1, p->y, q->z);
    MUL(s1, s1, z2z2);
    MUL(s2, q->y, p->z);
    MUL(s2, s2, z1z1);
    SUB(h, u2, u1);
    SUB(rr, s2, s1);
    if (bn_is_zero(h, c->n)) {
        if (bn_is_zero(rr, c->n))
            point_double(c, r, p);
        else
            set_infinity(r);
        return;
    }
    ADD(i, h, h);
    MUL(i, i, i);
    MUL(j, h, i);
    ADD(rr, rr, rr);
    MUL(v, u1, i);
    struct jac out;
    MUL(out.x, rr, rr);
    SUB(out.x, out.x, j);
    SUB(out.x, out.x, v);
    SUB(out.x, out.x, v);
    SUB(t, v, out.x);
    MUL(out.y, rr, t);
    MUL(t, s1, j);
    ADD(t, t, t);
    SUB(out.y, out.y, t);
    ADD(t, p->z, q->z);
    MUL(t, t, t);
    SUB(t, t, z1z1);
    SUB(t, t, z2z2);
    MUL(out.z, t, h);
    *r = out;
}

/* The affine coordinates of p, not in Montgomery form. p is not the
 * point at infinity. */
static void to_affine(const struct curve *c, uint64_t *x, uint64_t *y, const struct jac *p)
{
    uint64_t pm2[L], two[L] = { 2 }, zi[L], zi2[L], zi3[L], t[L];
    uint8_t exp[48];
    bn_sub(pm2, c->p.m, two, c->n);
    bn_to_bytes(exp, c->size, pm2, c->n);
    mont_pow(&c->p, zi, p->z, exp, c->size);
    MUL(zi2, zi, zi);
    MUL(zi3, zi2, zi);
    MUL(t, p->x, zi2);
    mont_from(&c->p, x, t);
    MUL(t, p->y, zi3);
    mont_from(&c->p, y, t);
}

/* Reads the public key 04 || x || y into p and checks that the point lies
 * on the curve y^2 = x^3 - 3x + b with coordinates below p. */
static int load_point(const struct curve *c, struct jac *p, const uint8_t *pub, size_t len)
{
    uint64_t x[L], y[L], lhs[L], rhs[L], t[L];
    if (len != 1 + 2 * c->size || pub[0] != 4)
        return -1;
    bn_from_bytes(x, c->n, pub + 1, c->size);
    bn_from_bytes(y, c->n, pub + 1 + c->size, c->size);
    if (bn_cmp(x, c->p.m, c->n) >= 0 || bn_cmp(y, c->p.m, c->n) >= 0)
        return -1;
    mont_to(&c->p, p->x, x);
    mont_to(&c->p, p->y, y);
    memcpy(p->z, c->p.one, sizeof p->z);
    MUL(lhs, p->y, p->y);
    MUL(rhs, p->x, p->x);
    MUL(rhs, rhs, p->x);
    ADD(t, p->x, p->x);
    ADD(t, t, p->x);
    SUB(rhs, rhs, t);
    ADD(rhs, rhs, c->b);
    return bn_cmp(lhs, rhs, c->n) == 0 ? 0 : -1;
}

static void generator(const struct curve *c, struct jac *g)
{
    memcpy(g->x, c->gx, sizeof g->x);
    memcpy(g->y, c->gy, sizeof g->y);
    memcpy(g->z, c->p.one, sizeof g->z);
}

static int bit_of(const uint64_t *k, int i)
{
    return (int)(k[i / 64] >> (i % 64) & 1);
}

/* r = k * p with an addition for every bit, selected by masks. */
static void scalar_mult(const struct curve *c, struct jac *r, const uint64_t *k, const struct jac *p)
{
    struct jac acc, sum;
    set_infinity(&acc);
    for (int i = 64 * c->n - 1; i >= 0; i--) {
        point_double(c, &acc, &acc);
        point_add(c, &sum, &acc, p);
        uint64_t mask = (uint64_t)0 - (uint64_t)bit_of(k, i);
        uint64_t *a = (uint64_t *)&acc, *s = (uint64_t *)&sum;
        for (size_t w = 0; w < sizeof acc / 8; w++)
            a[w] = (a[w] & ~mask) | (s[w] & mask);
    }
    *r = acc;
    crypto_wipe(&sum, sizeof sum);
}

/* Reads a big endian integer of up to size bytes after leading zeros. */
static int load_int(const struct curve *c, uint64_t *a, const uint8_t *b, size_t len)
{
    while (len && !*b) {
        b++;
        len--;
    }
    if (len > c->size)
        return -1;
    return bn_from_bytes(a, c->n, b, len);
}

int ecdsa_verify(enum ec_curve curve, const uint8_t *pub, size_t pub_len, const uint8_t *digest,
                 size_t digest_len, const uint8_t *r_bytes, size_t r_len, const uint8_t *s_bytes, size_t s_len)
{
    struct curve cv, *c = &cv;
    curve_init(c, curve);
    struct jac q;
    uint64_t r[L], s[L], e[L], w[L], u1[L], u2[L], t[L], two[L] = { 2 }, x[L], y[L];
    if (load_point(c, &q, pub, pub_len) < 0)
        return -1;
    if (load_int(c, r, r_bytes, r_len) < 0 || load_int(c, s, s_bytes, s_len) < 0)
        return -1;
    if (bn_is_zero(r, c->n) || bn_is_zero(s, c->n) || bn_cmp(r, c->q.m, c->n) >= 0 ||
        bn_cmp(s, c->q.m, c->n) >= 0)
        return -1;
    /* The leftmost bits of the digest as long as the order, which has
     * exactly 8 * size bits for both curves. */
    bn_from_bytes(e, c->n, digest, digest_len < c->size ? digest_len : c->size);
    if (bn_cmp(e, c->q.m, c->n) >= 0)
        bn_sub(e, e, c->q.m, c->n);
    /* w = s^-1 = s^(q - 2) modulo the prime order q. */
    uint8_t exp[48];
    bn_sub(t, c->q.m, two, c->n);
    bn_to_bytes(exp, c->size, t, c->n);
    mont_to(&c->q, t, s);
    mont_pow(&c->q, w, t, exp, c->size);
    mont_to(&c->q, t, e);
    mont_mul(&c->q, u1, t, w);
    mont_from(&c->q, u1, u1);
    mont_to(&c->q, t, r);
    mont_mul(&c->q, u2, t, w);
    mont_from(&c->q, u2, u2);
    /* u1 G + u2 Q by Shamir's trick: one doubling per bit and an addition
     * of G, Q or G + Q. The values are public. */
    struct jac g, gq, acc;
    generator(c, &g);
    point_add(c, &gq, &g, &q);
    set_infinity(&acc);
    for (int i = 64 * c->n - 1; i >= 0; i--) {
        point_double(c, &acc, &acc);
        int b1 = bit_of(u1, i), b2 = bit_of(u2, i);
        if (b1 && b2)
            point_add(c, &acc, &acc, &gq);
        else if (b1)
            point_add(c, &acc, &acc, &g);
        else if (b2)
            point_add(c, &acc, &acc, &q);
    }
    if (is_infinity(c, &acc))
        return -1;
    to_affine(c, x, y, &acc);
    /* x below p is below 2q, so one subtraction reduces it modulo q. */
    if (bn_cmp(x, c->q.m, c->n) >= 0)
        bn_sub(x, x, c->q.m, c->n);
    return bn_cmp(x, r, c->n) == 0 ? 0 : -1;
}

/* The private scalar must lie in 1 .. q - 1. */
static int load_private(const struct curve *c, uint64_t *k, const uint8_t priv[32])
{
    bn_from_bytes(k, c->n, priv, 32);
    return bn_is_zero(k, c->n) || bn_cmp(k, c->q.m, c->n) >= 0 ? -1 : 0;
}

int ec_p256_public(uint8_t pub[65], const uint8_t priv[32])
{
    struct curve cv, *c = &cv;
    curve_init(c, EC_P256);
    uint64_t k[L], x[L], y[L];
    if (load_private(c, k, priv) < 0)
        return -1;
    struct jac g, p;
    generator(c, &g);
    scalar_mult(c, &p, k, &g);
    to_affine(c, x, y, &p);
    pub[0] = 4;
    bn_to_bytes(pub + 1, 32, x, c->n);
    bn_to_bytes(pub + 33, 32, y, c->n);
    crypto_wipe(k, sizeof k);
    return 0;
}

int ec_p256_shared(uint8_t shared[32], const uint8_t priv[32], const uint8_t peer[65])
{
    struct curve cv, *c = &cv;
    curve_init(c, EC_P256);
    uint64_t k[L], x[L], y[L];
    struct jac q, p;
    if (load_private(c, k, priv) < 0 || load_point(c, &q, peer, 65) < 0)
        return -1;
    scalar_mult(c, &p, k, &q);
    crypto_wipe(k, sizeof k);
    if (is_infinity(c, &p))
        return -1;
    to_affine(c, x, y, &p);
    bn_to_bytes(shared, 32, x, c->n);
    return 0;
}
