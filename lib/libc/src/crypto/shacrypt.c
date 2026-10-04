/* SHA-256 crypt, the "$5$" password hash of Ulrich Drepper's
 * specification "Unix crypt using SHA-256 and SHA-512" (2008), written
 * from that description. The same source compiles on the host for the
 * crypto test of `make check-pkg`. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <minios/sha2.h>

#define SALT_MAX       16
#define ROUNDS_DEFAULT 5000
#define ROUNDS_MIN     1000
#define ROUNDS_MAX     999999999UL

static const char b64[] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

/* Append n characters encoding the 24 bit group b2 b1 b0, least
 * significant six bits first. */
static char *put24(char *p, uint8_t b2, uint8_t b1, uint8_t b0, int n)
{
    uint32_t w = ((uint32_t)b2 << 16) | ((uint32_t)b1 << 8) | b0;
    while (n-- > 0) {
        *p++ = b64[w & 0x3f];
        w >>= 6;
    }
    return p;
}

/* Feed len bytes made of repetitions of the 32 byte block d. */
static void update_repeated(struct sha256_ctx *c, const uint8_t d[SHA256_DIGEST_SIZE], size_t len)
{
    for (; len >= SHA256_DIGEST_SIZE; len -= SHA256_DIGEST_SIZE)
        sha256_update(c, d, SHA256_DIGEST_SIZE);
    sha256_update(c, d, len);
}

char *sha256_crypt(const char *key, const char *setting, char *out, size_t size)
{
    if (strncmp(setting, "$5$", 3) != 0)
        return NULL;
    const char *s = setting + 3;
    unsigned long rounds = ROUNDS_DEFAULT;
    int explicit_rounds = 0;
    if (strncmp(s, "rounds=", 7) == 0) {
        char *end;
        unsigned long r = strtoul(s + 7, &end, 10);
        if (*end != '$' || end == s + 7)
            return NULL;
        rounds = r < ROUNDS_MIN ? ROUNDS_MIN : r > ROUNDS_MAX ? ROUNDS_MAX : r;
        explicit_rounds = 1;
        s = end + 1;
    }
    size_t salt_len = strcspn(s, "$");
    if (salt_len > SALT_MAX)
        salt_len = SALT_MAX;
    const char *salt = s;
    size_t key_len = strlen(key);

    /* Digest B of key, salt, key. */
    struct sha256_ctx c;
    uint8_t b[SHA256_DIGEST_SIZE], a[SHA256_DIGEST_SIZE];
    sha256_init(&c);
    sha256_update(&c, key, key_len);
    sha256_update(&c, salt, salt_len);
    sha256_update(&c, key, key_len);
    sha256_final(&c, b);

    /* Digest A of key, salt, B stretched to the key length, then B or the
     * key for each bit of the key length from the lowest. */
    sha256_init(&c);
    sha256_update(&c, key, key_len);
    sha256_update(&c, salt, salt_len);
    update_repeated(&c, b, key_len);
    for (size_t n = key_len; n > 0; n >>= 1) {
        if (n & 1)
            sha256_update(&c, b, sizeof b);
        else
            sha256_update(&c, key, key_len);
    }
    sha256_final(&c, a);

    /* The byte sequences P (from the key) and S (from the salt). */
    uint8_t dp[SHA256_DIGEST_SIZE], ds[SHA256_DIGEST_SIZE];
    sha256_init(&c);
    for (size_t i = 0; i < key_len; i++)
        sha256_update(&c, key, key_len);
    sha256_final(&c, dp);
    sha256_init(&c);
    for (unsigned i = 0; i < 16u + a[0]; i++)
        sha256_update(&c, salt, salt_len);
    sha256_final(&c, ds);
    char *p_seq = malloc(key_len + 1);
    if (!p_seq)
        return NULL;
    for (size_t i = 0; i < key_len; i++)
        p_seq[i] = (char)dp[i % SHA256_DIGEST_SIZE];
    uint8_t s_seq[SALT_MAX];
    for (size_t i = 0; i < salt_len; i++)
        s_seq[i] = ds[i % SHA256_DIGEST_SIZE];

    /* The rounds. */
    for (unsigned long i = 0; i < rounds; i++) {
        sha256_init(&c);
        if (i & 1)
            sha256_update(&c, p_seq, key_len);
        else
            sha256_update(&c, a, sizeof a);
        if (i % 3)
            sha256_update(&c, s_seq, salt_len);
        if (i % 7)
            sha256_update(&c, p_seq, key_len);
        if (i & 1)
            sha256_update(&c, a, sizeof a);
        else
            sha256_update(&c, p_seq, key_len);
        sha256_final(&c, a);
    }
    memset(p_seq, 0, key_len);
    free(p_seq);

    char hash[64], *h = hash;
    static const uint8_t order[10][3] = {
        { 0, 10, 20 }, { 21, 1, 11 }, { 12, 22, 2 }, { 3, 13, 23 }, { 24, 4, 14 },
        { 15, 25, 5 }, { 6, 16, 26 }, { 27, 7, 17 }, { 18, 28, 8 }, { 9, 19, 29 },
    };
    for (int i = 0; i < 10; i++)
        h = put24(h, a[order[i][0]], a[order[i][1]], a[order[i][2]], 4);
    h = put24(h, 0, a[31], a[30], 3);
    *h = '\0';
    int n;
    if (explicit_rounds)
        n = snprintf(out, size, "$5$rounds=%lu$%.*s$%s", rounds, (int)salt_len, salt, hash);
    else
        n = snprintf(out, size, "$5$%.*s$%s", (int)salt_len, salt, hash);
    memset(a, 0, sizeof a);
    memset(b, 0, sizeof b);
    return n > 0 && (size_t)n < size ? out : NULL;
}
