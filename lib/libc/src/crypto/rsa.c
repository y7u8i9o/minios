/* RSA signature verification (RFC 8017) for minios/crypto.h: the
 * encoded message is the signature to the power e modulo n
 * (RSAVP1). PKCS#1 v1.5 compares it with the expected encoding of the
 * digest. PSS checks the encoding of section 9.1.2 with MGF1. */
#include <minios/crypto.h>
#include <string.h>
#include "bignum.h"

#define MAX_BYTES 512                   /* 4096 bit moduli */

/* m = sig^e mod n into em, which has the length of the modulus. */
static int rsavp1(const uint8_t *n, size_t n_len, const uint8_t *e, size_t e_len, const uint8_t *sig,
                  size_t sig_len, uint8_t *em, size_t *k_out)
{
    while (n_len && !*n) {
        n++;
        n_len--;
    }
    while (e_len && !*e) {
        e++;
        e_len--;
    }
    if (n_len < 128 || n_len > MAX_BYTES || !(n[n_len - 1] & 1) || !e_len || sig_len != n_len)
        return -1;
    int limbs = (int)((n_len + 7) / 8);
    uint64_t m[BN_MAX_LIMBS], s[BN_MAX_LIMBS], t[BN_MAX_LIMBS];
    struct mont c;
    bn_from_bytes(m, limbs, n, n_len);
    bn_from_bytes(s, limbs, sig, sig_len);
    if (bn_cmp(s, m, limbs) >= 0)
        return -1;
    mont_init(&c, m, limbs);
    mont_to(&c, t, s);
    mont_pow(&c, t, t, e, e_len);
    mont_from(&c, s, t);
    bn_to_bytes(em, n_len, s, limbs);
    *k_out = n_len;
    return 0;
}

int rsa_verify_pkcs1(const uint8_t *n, size_t n_len, const uint8_t *e, size_t e_len, enum hash_alg alg,
                     const uint8_t *digest, const uint8_t *sig, size_t sig_len)
{
    /* The DER encoding of DigestInfo before the digest. */
    static const uint8_t sha256_info[] = { 0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
                                           0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20 };
    static const uint8_t sha384_info[] = { 0x30, 0x41, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
                                           0x65, 0x03, 0x04, 0x02, 0x02, 0x05, 0x00, 0x04, 0x30 };
    static const uint8_t sha512_info[] = { 0x30, 0x51, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
                                           0x65, 0x03, 0x04, 0x02, 0x03, 0x05, 0x00, 0x04, 0x40 };
    const uint8_t *info = alg == HASH_SHA256 ? sha256_info : alg == HASH_SHA384 ? sha384_info : sha512_info;
    size_t info_len = sizeof sha256_info, hl = hash_size(alg), k;
    uint8_t em[MAX_BYTES], want[MAX_BYTES];
    if (rsavp1(n, n_len, e, e_len, sig, sig_len, em, &k) < 0 || k < info_len + hl + 11)
        return -1;
    /* 00 01 FF .. FF 00 DigestInfo digest */
    want[0] = 0;
    want[1] = 1;
    size_t ps = k - 3 - info_len - hl;
    memset(want + 2, 0xff, ps);
    want[2 + ps] = 0;
    memcpy(want + 3 + ps, info, info_len);
    memcpy(want + 3 + ps + info_len, digest, hl);
    return memcmp(em, want, k) == 0 ? 0 : -1;
}

/* MGF1 with the hash: the mask of len bytes from seed. */
static void mgf1(enum hash_alg alg, const uint8_t *seed, size_t seed_len, uint8_t *mask, size_t len)
{
    uint8_t block[HASH_MAX_SIZE];
    size_t hl = hash_size(alg);
    for (uint32_t counter = 0; len; counter++) {
        uint8_t ctr[4] = { (uint8_t)(counter >> 24), (uint8_t)(counter >> 16), (uint8_t)(counter >> 8),
                           (uint8_t)counter };
        struct hash_ctx h;
        hash_init(&h, alg);
        hash_update(&h, seed, seed_len);
        hash_update(&h, ctr, 4);
        hash_final(&h, block);
        size_t k = len < hl ? len : hl;
        memcpy(mask, block, k);
        mask += k;
        len -= k;
    }
}

int rsa_verify_pss(const uint8_t *n, size_t n_len, const uint8_t *e, size_t e_len, enum hash_alg alg,
                   const uint8_t *digest, const uint8_t *sig, size_t sig_len)
{
    uint8_t m[MAX_BYTES], db[MAX_BYTES];
    size_t k, hl = hash_size(alg), salt_len = hl;
    if (rsavp1(n, n_len, e, e_len, sig, sig_len, m, &k) < 0)
        return -1;
    while (n_len && !*n) {
        n++;
        n_len--;
    }
    /* emBits = modBits - 1; the encoded message takes the last emLen
     * bytes of m, and an unused leading byte must be zero. */
    int top = 7;
    while (top >= 0 && !(n[0] >> top & 1))
        top--;
    size_t mod_bits = (n_len - 1) * 8 + (size_t)top + 1, em_bits = mod_bits - 1, em_len = (em_bits + 7) / 8;
    const uint8_t *em = m + (k - em_len);
    if (k > em_len && m[0] != 0)
        return -1;
    if (em_len < hl + salt_len + 2 || em[em_len - 1] != 0xbc)
        return -1;
    size_t db_len = em_len - hl - 1;
    const uint8_t *h = em + db_len;
    unsigned unused = (unsigned)(8 * em_len - em_bits);
    if (unused && em[0] >> (8 - unused))
        return -1;
    mgf1(alg, h, hl, db, db_len);
    for (size_t i = 0; i < db_len; i++)
        db[i] ^= em[i];
    if (unused)
        db[0] &= (uint8_t)(0xff >> unused);
    /* DB = PS (zeros) || 01 || salt */
    size_t ps = db_len - salt_len - 1;
    for (size_t i = 0; i < ps; i++)
        if (db[i])
            return -1;
    if (db[ps] != 1)
        return -1;
    static const uint8_t zeros[8];
    uint8_t want[HASH_MAX_SIZE];
    struct hash_ctx c;
    hash_init(&c, alg);
    hash_update(&c, zeros, 8);
    hash_update(&c, digest, hl);
    hash_update(&c, db + ps + 1, salt_len);
    hash_final(&c, want);
    return memcmp(want, h, hl) == 0 ? 0 : -1;
}
