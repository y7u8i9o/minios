/* X25519 (RFC 7748, section 5) for minios/crypto.h: the Montgomery
 * ladder over the field of fe25519.h. The ladder runs the same steps for
 * every scalar and swaps with masks, so it does not branch on the
 * secret. */
#include <minios/crypto.h>
#include <string.h>
#include "fe25519.h"

/* Swaps f and g when swap is 1, without a branch. */
static void fe_cswap(struct fe *f, struct fe *g, uint64_t swap)
{
    uint64_t mask = (uint64_t)0 - swap;
    for (int i = 0; i < 5; i++) {
        uint64_t t = mask & (f->v[i] ^ g->v[i]);
        f->v[i] ^= t;
        g->v[i] ^= t;
    }
}

int x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32])
{
    uint8_t k[32];
    memcpy(k, scalar, 32);
    k[0] &= 248;
    k[31] &= 127;
    k[31] |= 64;
    struct fe x1, x2, z2, x3, z3, a24, a, aa, b, bb, e, c, d, da, cb, t;
    fe_frombytes(&x1, point);
    fe_set(&x2, 1);
    fe_set(&z2, 0);
    x3 = x1;
    fe_set(&z3, 1);
    fe_set(&a24, 121665);
    uint64_t swap = 0;
    for (int i = 254; i >= 0; i--) {
        uint64_t bit = (uint64_t)(k[i / 8] >> (i % 8) & 1);
        swap ^= bit;
        fe_cswap(&x2, &x3, swap);
        fe_cswap(&z2, &z3, swap);
        swap = bit;
        fe_add(&a, &x2, &z2);
        fe_sq(&aa, &a);
        fe_sub(&b, &x2, &z2);
        fe_sq(&bb, &b);
        fe_sub(&e, &aa, &bb);
        fe_add(&c, &x3, &z3);
        fe_sub(&d, &x3, &z3);
        fe_mul(&da, &d, &a);
        fe_mul(&cb, &c, &b);
        fe_add(&t, &da, &cb);
        fe_sq(&x3, &t);
        fe_sub(&t, &da, &cb);
        fe_sq(&t, &t);
        fe_mul(&z3, &x1, &t);
        fe_mul(&x2, &aa, &bb);
        fe_mul(&t, &a24, &e);
        fe_add(&t, &aa, &t);
        fe_mul(&z2, &e, &t);
    }
    fe_cswap(&x2, &x3, swap);
    fe_cswap(&z2, &z3, swap);
    fe_inv(&z2, &z2);
    fe_mul(&x2, &x2, &z2);
    fe_tobytes(out, &x2);
    crypto_wipe(k, sizeof k);
    uint8_t zero = 0;
    for (int i = 0; i < 32; i++)
        zero |= out[i];
    return zero ? 0 : -1;
}

void x25519_base(uint8_t out[32], const uint8_t scalar[32])
{
    static const uint8_t nine[32] = { 9 };
    x25519(out, scalar, nine);
}
