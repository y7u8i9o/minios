#pragma once
#include <stdint.h>

/* The fields of a long double for the code that takes it apart (the
 * classification, the argument reduction of the trigonometric functions,
 * %La). Two formats: the x87 80 bit format of x86_64 (64 significand bits
 * with an explicit integer bit) and IEEE binary128 of aarch64 (113 bits,
 * the integer bit implicit). Both have a 15 bit exponent with bias 16383.
 * significand is the leading 64 bits with the integer bit at bit 63, as
 * x87 stores it: set for normal numbers, infinities and NaNs. lower is
 * nonzero when binary128 has significand bits below those 64. */
struct ld_parts {
    unsigned negative;
    unsigned raw_exponent;      /* 0 for zero and subnormals, 0x7fff for infinities and NaNs */
    uint64_t significand;
    uint64_t lower;
};

static inline struct ld_parts ld_split(long double v)
{
    struct ld_parts p;
#if __LDBL_MANT_DIG__ == 64
    union {
        long double value;
        struct {
            uint64_t significand;
            uint16_t sign_exponent;
            uint16_t padding[3];
        } parts;
    } u = { v };
    p.negative = u.parts.sign_exponent >> 15;
    p.raw_exponent = u.parts.sign_exponent & 0x7fffU;
    p.significand = u.parts.significand;
    p.lower = 0;
#elif __LDBL_MANT_DIG__ == 113
    union {
        long double value;
        struct { uint64_t lo, hi; } words;
    } u = { v };
    p.negative = (unsigned)(u.words.hi >> 63);
    p.raw_exponent = (unsigned)(u.words.hi >> 48) & 0x7fffU;
    uint64_t fraction_hi = u.words.hi & ((1ULL << 48) - 1);
    uint64_t integer = p.raw_exponent != 0;
    if (p.raw_exponent == 0x7fff)
        integer = 1;
    p.significand = integer << 63 | fraction_hi << 15 | u.words.lo >> 49;
    p.lower = u.words.lo & ((1ULL << 49) - 1);
#else
#error "unsupported long double format"
#endif
    return p;
}
