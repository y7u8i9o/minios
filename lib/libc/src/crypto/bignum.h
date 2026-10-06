#pragma once
/* Arithmetic on numbers of n limbs of 64 bits, least significant limb
 * first, for ec.c and rsa.c. Products modulo an odd modulus m use the
 * Montgomery form a * R mod m with R = 2^(64 n). The functions take
 * inputs below m and return results below m. */
#include <stddef.h>
#include <stdint.h>

#define BN_MAX_LIMBS 65

struct mont {
    int n;
    uint64_t m[BN_MAX_LIMBS];
    uint64_t minv;                  /* -m^-1 modulo 2^64 */
    uint64_t one[BN_MAX_LIMBS];     /* R mod m, the Montgomery form of 1 */
    uint64_t r2[BN_MAX_LIMBS];      /* R^2 mod m */
};

/* Reads len big endian bytes into n limbs. Returns -1 when the value
 * does not fit. */
int bn_from_bytes(uint64_t *a, int n, const uint8_t *b, size_t len);
void bn_to_bytes(uint8_t *b, size_t len, const uint64_t *a, int n);
int bn_cmp(const uint64_t *a, const uint64_t *b, int n);
int bn_is_zero(const uint64_t *a, int n);
uint64_t bn_add(uint64_t *r, const uint64_t *a, const uint64_t *b, int n);  /* returns the carry */
uint64_t bn_sub(uint64_t *r, const uint64_t *a, const uint64_t *b, int n);  /* returns the borrow */

void mont_init(struct mont *c, const uint64_t *m, int n);
void mont_mul(const struct mont *c, uint64_t *r, const uint64_t *a, const uint64_t *b);
void mont_to(const struct mont *c, uint64_t *r, const uint64_t *a);
void mont_from(const struct mont *c, uint64_t *r, const uint64_t *a);
void mont_add(const struct mont *c, uint64_t *r, const uint64_t *a, const uint64_t *b);
void mont_sub(const struct mont *c, uint64_t *r, const uint64_t *a, const uint64_t *b);
/* r = a^e in Montgomery form, a in Montgomery form, e big endian. The
 * running time depends on e, which is public in every use. */
void mont_pow(const struct mont *c, uint64_t *r, const uint64_t *a, const uint8_t *e, size_t e_len);
