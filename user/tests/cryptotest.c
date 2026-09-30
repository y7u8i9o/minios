/* This program checks libc/src/crypto against the test vectors of RFC
 * 6234 (SHA-256 and SHA-512, TEST1 to TEST4 of section 8.5) and RFC 8032
 * (Ed25519, TEST 1, 2, 3 and SHA(abc) of section 7.1). The same source runs on minios
 * as /bin/cryptotest, from the pkg_repo case, and on the host through
 * `make check-pkg`. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <minios/sha2.h>
#include <minios/ed25519.h>

static int failures;

static void check(int ok, const char *what)
{
    if (!ok) {
        printf("cryptotest: FAIL %s\n", what);
        failures++;
    }
}

static size_t unhex(uint8_t *out, size_t max, const char *hex)
{
    size_t n = 0;
    for (; hex[0] && hex[1] && n < max; hex += 2) {
        unsigned v;
        sscanf(hex, "%2x", &v);
        out[n++] = (uint8_t)v;
    }
    return n;
}

static int equal_hex(const uint8_t *data, size_t len, const char *hex)
{
    uint8_t want[128];
    return unhex(want, sizeof want, hex) == len && memcmp(want, data, len) == 0;
}

struct hash_vector {
    const char *name;
    const char *text;           /* The text is repeated count times. */
    long count;
    const char *sha256;
    const char *sha512;
};

static const struct hash_vector hashes[] = {
    { "TEST1", "abc", 1,
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
      "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
      "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f" },
    { "TEST2_1", "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 1,
      "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", NULL },
    { "TEST2_2", "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
                 "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu", 1,
      NULL,
      "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018"
      "501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909" },
    { "TEST3", "a", 1000000,
      "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
      "e718483d0ce769644e2e42c7bc15b4638e1f98b13b2044285632a803afa973eb"
      "de0ff244877ea60a4cb0432ce577c31beb009c5c2c49aa2e4eadb217ad8cc09b" },
    { "TEST4", "0123456701234567012345670123456701234567012345670123456701234567", 10,
      "594847328451bdfa85056225462cc1d867d877fb388df0ce35f25ab5562bfbb5",
      "89d05ba632c699c31231ded4ffc127d5a894dad412c0e024db872d1abd2ba814"
      "1a0f85072a9be1e2aa04cf33c765cb510813a39cd5a84c4acaa64d3f3fb7bae9" },
};

/* Each vector is hashed twice, once fed by repetition, which exercises
 * the partial blocks, and once as one buffer in a single call. */
static void test_hashes(void)
{
    char what[64];
    for (size_t i = 0; i < sizeof hashes / sizeof hashes[0]; i++) {
        const struct hash_vector *v = &hashes[i];
        size_t tl = strlen(v->text), total = tl * (size_t)v->count;
        uint8_t *all = malloc(total);
        for (long r = 0; r < v->count; r++)
            memcpy(all + (size_t)r * tl, v->text, tl);
        uint8_t d256[SHA256_DIGEST_SIZE], d512[SHA512_DIGEST_SIZE];
        if (v->sha256) {
            struct sha256_ctx c;
            sha256_init(&c);
            for (long r = 0; r < v->count; r++)
                sha256_update(&c, v->text, tl);
            sha256_final(&c, d256);
            snprintf(what, sizeof what, "SHA-256 %s by pieces", v->name);
            check(equal_hex(d256, sizeof d256, v->sha256), what);
            sha256(all, total, d256);
            snprintf(what, sizeof what, "SHA-256 %s at once", v->name);
            check(equal_hex(d256, sizeof d256, v->sha256), what);
        }
        if (v->sha512) {
            struct sha512_ctx c;
            sha512_init(&c);
            for (long r = 0; r < v->count; r++)
                sha512_update(&c, v->text, tl);
            sha512_final(&c, d512);
            snprintf(what, sizeof what, "SHA-512 %s by pieces", v->name);
            check(equal_hex(d512, sizeof d512, v->sha512), what);
            sha512(all, total, d512);
            snprintf(what, sizeof what, "SHA-512 %s at once", v->name);
            check(equal_hex(d512, sizeof d512, v->sha512), what);
        }
        free(all);
    }
}

struct sign_vector {
    const char *name, *seed, *pub, *msg, *sig;
};

static const struct sign_vector signs[] = {
    { "TEST 1",
      "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
      "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
      "",
      "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
      "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b" },
    { "TEST 2",
      "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
      "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
      "72",
      "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
      "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00" },
    { "TEST 3",
      "c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
      "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025",
      "af82",
      "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac"
      "18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a" },
    { "TEST SHA(abc)",
      "833fe62409237b9d62ec77587520911e9a759cec1d19755b7da901b96dca3d42",
      "ec172b93ad5e563bf4932c70e1245034c35467ef2efd4d64ebf819683467e2bf",
      "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
      "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
      "dc2a4459e7369633a52b1bf277839a00201009a3efbf3ecb69bea2186c26b589"
      "09351fc9ac90b3ecfdfbc7c66431e0303dca179c138ac17ad9bef1177331a704" },
};

/* The group order L, added to a valid S, gives a signature that encodes
 * the same scalar non-canonically. */
static const char order_hex[] = "edd3f55c1a631258d69cf7a2def9de1400000000000000000000000000000010";

static void test_signatures(void)
{
    char what[80];
    for (size_t i = 0; i < sizeof signs / sizeof signs[0]; i++) {
        const struct sign_vector *v = &signs[i];
        uint8_t seed[32], pub[32], want_pub[32], msg[64], sig[64], want_sig[64], bad[64];
        unhex(seed, sizeof seed, v->seed);
        unhex(want_pub, sizeof want_pub, v->pub);
        size_t len = unhex(msg, sizeof msg, v->msg);
        unhex(want_sig, sizeof want_sig, v->sig);
        ed25519_public_key(pub, seed);
        snprintf(what, sizeof what, "Ed25519 %s public key", v->name);
        check(memcmp(pub, want_pub, 32) == 0, what);
        ed25519_sign(sig, msg, len, seed);
        snprintf(what, sizeof what, "Ed25519 %s signature", v->name);
        check(memcmp(sig, want_sig, 64) == 0, what);
        snprintf(what, sizeof what, "Ed25519 %s verifies", v->name);
        check(ed25519_verify(want_sig, msg, len, want_pub) == 1, what);
        /* A changed message, a changed R, a changed S and another key
         * must each fail. */
        uint8_t other[65];
        memcpy(other, msg, len);
        other[len] = 0x01;
        snprintf(what, sizeof what, "Ed25519 %s refuses a longer message", v->name);
        check(ed25519_verify(want_sig, other, len + 1, want_pub) == 0, what);
        if (len) {
            other[0] ^= 0x01;
            snprintf(what, sizeof what, "Ed25519 %s refuses a changed message", v->name);
            check(ed25519_verify(want_sig, other, len, want_pub) == 0, what);
        }
        memcpy(bad, want_sig, 64);
        bad[3] ^= 0x10;
        snprintf(what, sizeof what, "Ed25519 %s refuses a changed R", v->name);
        check(ed25519_verify(bad, msg, len, want_pub) == 0, what);
        memcpy(bad, want_sig, 64);
        bad[40] ^= 0x01;
        snprintf(what, sizeof what, "Ed25519 %s refuses a changed S", v->name);
        check(ed25519_verify(bad, msg, len, want_pub) == 0, what);
        /* S + L encodes the same scalar modulo L and is refused because
         * it is not below L. */
        uint8_t order[32];
        unhex(order, sizeof order, order_hex);
        memcpy(bad, want_sig, 64);
        unsigned carry = 0;
        for (int b = 0; b < 32; b++) {
            unsigned s = bad[32 + b] + order[b] + carry;
            bad[32 + b] = (uint8_t)s;
            carry = s >> 8;
        }
        snprintf(what, sizeof what, "Ed25519 %s refuses S + L", v->name);
        check(ed25519_verify(bad, msg, len, want_pub) == 0, what);
        uint8_t wrong[32];
        unhex(wrong, sizeof wrong, signs[(i + 1) % (sizeof signs / sizeof signs[0])].pub);
        snprintf(what, sizeof what, "Ed25519 %s refuses another key", v->name);
        check(ed25519_verify(want_sig, msg, len, wrong) == 0, what);
    }
    /* A public key whose y is not below p does not decode. */
    uint8_t pub[32], sig[64];
    memset(pub, 0xff, sizeof pub);
    pub[31] = 0x7f;
    unhex(sig, sizeof sig, signs[0].sig);
    check(ed25519_verify(sig, "", 0, pub) == 0, "Ed25519 refuses a key with y >= p");
}

int main(void)
{
    test_hashes();
    test_signatures();
    printf("cryptotest: %d failures\n", failures);
    return failures ? 1 : 0;
}
