/* Multi-limb arithmetic and Montgomery multiplication (bignum.h). The
 * product uses the coarsely integrated operand scanning method: each
 * step adds one limb of b times a and then one multiple of m that clears
 * the lowest limb, so the intermediate value never exceeds n + 2 limbs. */
#include "bignum.h"
#include <string.h>

typedef unsigned __int128 u128;

int bn_from_bytes(uint64_t *a, int n, const uint8_t *b, size_t len)
{
    memset(a, 0, (size_t)n * 8);
    for (size_t i = 0; i < len; i++) {
        size_t bit = (len - 1 - i) * 8;
        if (b[i] && bit / 64 >= (size_t)n)
            return -1;
        if (bit / 64 < (size_t)n)
            a[bit / 64] |= (uint64_t)b[i] << (bit % 64);
    }
    return 0;
}

void bn_to_bytes(uint8_t *b, size_t len, const uint64_t *a, int n)
{
    for (size_t i = 0; i < len; i++) {
        size_t bit = (len - 1 - i) * 8;
        b[i] = bit / 64 < (size_t)n ? (uint8_t)(a[bit / 64] >> (bit % 64)) : 0;
    }
}

int bn_cmp(const uint64_t *a, const uint64_t *b, int n)
{
    for (int i = n - 1; i >= 0; i--)
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    return 0;
}

int bn_is_zero(const uint64_t *a, int n)
{
    uint64_t v = 0;
    for (int i = 0; i < n; i++)
        v |= a[i];
    return v == 0;
}

uint64_t bn_add(uint64_t *r, const uint64_t *a, const uint64_t *b, int n)
{
    u128 c = 0;
    for (int i = 0; i < n; i++) {
        c += (u128)a[i] + b[i];
        r[i] = (uint64_t)c;
        c >>= 64;
    }
    return (uint64_t)c;
}

uint64_t bn_sub(uint64_t *r, const uint64_t *a, const uint64_t *b, int n)
{
    uint64_t borrow = 0;
    for (int i = 0; i < n; i++) {
        uint64_t x = a[i], y = b[i];
        uint64_t d = x - y - borrow;
        borrow = (x < y) | ((x == y) & borrow);
        r[i] = d;
    }
    return borrow;
}

/* r = 2 r mod m for r below m. */
static void mod_double(const struct mont *c, uint64_t *r)
{
    uint64_t carry = bn_add(r, r, r, c->n);
    if (carry || bn_cmp(r, c->m, c->n) >= 0)
        bn_sub(r, r, c->m, c->n);
}

void mont_init(struct mont *c, const uint64_t *m, int n)
{
    c->n = n;
    memcpy(c->m, m, (size_t)n * 8);
    /* The inverse of m[0] modulo 2^64 by Newton's iteration: each step
     * doubles the number of correct low bits. */
    uint64_t x = m[0];
    for (int i = 0; i < 6; i++)
        x *= 2 - m[0] * x;
    c->minv = (uint64_t)0 - x;
    /* R mod m and R^2 mod m by doubling 1. */
    memset(c->one, 0, sizeof c->one);
    c->one[0] = 1;
    if (n == 1 && m[0] == 1)
        c->one[0] = 0;
    for (int i = 0; i < 64 * n; i++)
        mod_double(c, c->one);
    memcpy(c->r2, c->one, sizeof c->r2);
    for (int i = 0; i < 64 * n; i++)
        mod_double(c, c->r2);
}

void mont_mul(const struct mont *c, uint64_t *r, const uint64_t *a, const uint64_t *b)
{
    int n = c->n;
    uint64_t t[BN_MAX_LIMBS + 2] = { 0 };
    for (int i = 0; i < n; i++) {
        u128 carry = 0;
        for (int j = 0; j < n; j++) {
            carry += (u128)a[j] * b[i] + t[j];
            t[j] = (uint64_t)carry;
            carry >>= 64;
        }
        carry += t[n];
        t[n] = (uint64_t)carry;
        t[n + 1] = (uint64_t)(carry >> 64);
        uint64_t q = t[0] * c->minv;
        carry = (u128)q * c->m[0] + t[0];
        carry >>= 64;
        for (int j = 1; j < n; j++) {
            carry += (u128)q * c->m[j] + t[j];
            t[j - 1] = (uint64_t)carry;
            carry >>= 64;
        }
        carry += t[n];
        t[n - 1] = (uint64_t)carry;
        t[n] = t[n + 1] + (uint64_t)(carry >> 64);
    }
    /* t is below 2m: one subtraction brings it below m. */
    uint64_t d[BN_MAX_LIMBS];
    uint64_t borrow = bn_sub(d, t, c->m, n);
    if (t[n] || !borrow)
        memcpy(r, d, (size_t)n * 8);
    else
        memcpy(r, t, (size_t)n * 8);
}

void mont_to(const struct mont *c, uint64_t *r, const uint64_t *a)
{
    mont_mul(c, r, a, c->r2);
}

void mont_from(const struct mont *c, uint64_t *r, const uint64_t *a)
{
    uint64_t one[BN_MAX_LIMBS] = { 1 };
    mont_mul(c, r, a, one);
}

void mont_add(const struct mont *c, uint64_t *r, const uint64_t *a, const uint64_t *b)
{
    uint64_t carry = bn_add(r, a, b, c->n);
    if (carry || bn_cmp(r, c->m, c->n) >= 0)
        bn_sub(r, r, c->m, c->n);
}

void mont_sub(const struct mont *c, uint64_t *r, const uint64_t *a, const uint64_t *b)
{
    if (bn_sub(r, a, b, c->n))
        bn_add(r, r, c->m, c->n);
}

void mont_pow(const struct mont *c, uint64_t *r, const uint64_t *a, const uint8_t *e, size_t e_len)
{
    uint64_t acc[BN_MAX_LIMBS], base[BN_MAX_LIMBS];
    memcpy(acc, c->one, sizeof acc);
    memcpy(base, a, (size_t)c->n * 8);
    for (size_t i = 0; i < e_len; i++)
        for (int bit = 7; bit >= 0; bit--) {
            mont_mul(c, acc, acc, acc);
            if (e[i] >> bit & 1)
                mont_mul(c, acc, acc, base);
        }
    memcpy(r, acc, (size_t)c->n * 8);
}
