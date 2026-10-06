/* The crypto oracle of make check-crypto: reads one operation per line,
 * "NAME ARG ...", with arguments in hexadecimal ("-" for an empty
 * string), and prints the result in hexadecimal, or "ok" and "bad" for a
 * verification. check.py compares the results with the cryptography
 * package of Python. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <minios/crypto.h>

#define MAX 4096

static size_t unhex(const char *s, uint8_t *out)
{
    if (!strcmp(s, "-"))
        return 0;
    size_t n = strlen(s) / 2;
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        sscanf(s + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
    return n;
}

static void print_hex(const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        printf("%02x", b[i]);
    printf("\n");
}

static enum hash_alg alg_of(const char *name)
{
    return !strcmp(name, "sha256") ? HASH_SHA256 : !strcmp(name, "sha384") ? HASH_SHA384 : HASH_SHA512;
}

int main(void)
{
    static char line[16 * MAX];
    static uint8_t a[MAX], b[MAX], c[MAX], d[MAX], e[MAX], out[MAX];
    while (fgets(line, sizeof line, stdin)) {
        char *w[8];
        int n = 0;
        for (char *t = strtok(line, " \n"); t && n < 8; t = strtok(NULL, " \n"))
            w[n++] = t;
        if (!n)
            continue;
        const char *op = w[0];
        if (!strcmp(op, "hash")) {
            enum hash_alg alg = alg_of(w[1]);
            size_t la = unhex(w[2], a);
            hash(alg, a, la, out);
            print_hex(out, hash_size(alg));
        } else if (!strcmp(op, "hmac")) {
            enum hash_alg alg = alg_of(w[1]);
            size_t lk = unhex(w[2], a), ld = unhex(w[3], b);
            hmac(alg, a, lk, b, ld, out);
            print_hex(out, hash_size(alg));
        } else if (!strcmp(op, "hkdf")) {
            enum hash_alg alg = alg_of(w[1]);
            size_t ls = unhex(w[2], a), li = unhex(w[3], b), lf = unhex(w[4], c);
            size_t len = (size_t)atoi(w[5]);
            uint8_t prk[HASH_MAX_SIZE];
            hkdf_extract(alg, a, ls, b, li, prk);
            hkdf_expand(alg, prk, c, lf, out, len);
            print_hex(out, len);
        } else if (!strcmp(op, "chacha_seal") || !strcmp(op, "gcm_seal")) {
            unhex(w[1], a);
            unhex(w[2], b);
            size_t la = unhex(w[3], c), lp = unhex(w[4], d);
            if (op[0] == 'c')
                chacha20_poly1305_seal(a, b, c, la, d, lp, out);
            else
                aes128_gcm_seal(a, b, c, la, d, lp, out);
            print_hex(out, lp + 16);
        } else if (!strcmp(op, "chacha_open") || !strcmp(op, "gcm_open")) {
            unhex(w[1], a);
            unhex(w[2], b);
            size_t la = unhex(w[3], c), lc = unhex(w[4], d);
            int r = op[0] == 'c' ? chacha20_poly1305_open(a, b, c, la, d, lc - 16, d + lc - 16, out)
                                 : aes128_gcm_open(a, b, c, la, d, lc - 16, d + lc - 16, out);
            if (r < 0)
                printf("bad\n");
            else
                print_hex(out, lc - 16);
        } else if (!strcmp(op, "x25519")) {
            unhex(w[1], a);
            unhex(w[2], b);
            if (x25519(out, a, b) < 0)
                printf("zero\n");
            else
                print_hex(out, 32);
        } else if (!strcmp(op, "p256_public")) {
            unhex(w[1], a);
            if (ec_p256_public(out, a) < 0)
                printf("bad\n");
            else
                print_hex(out, 65);
        } else if (!strcmp(op, "p256_shared")) {
            unhex(w[1], a);
            unhex(w[2], b);
            if (ec_p256_shared(out, a, b) < 0)
                printf("bad\n");
            else
                print_hex(out, 32);
        } else if (!strcmp(op, "ecdsa")) {
            enum ec_curve cv = !strcmp(w[1], "p256") ? EC_P256 : EC_P384;
            size_t lp = unhex(w[2], a), lh = unhex(w[3], b), lr = unhex(w[4], c), ls = unhex(w[5], d);
            printf("%s\n", ecdsa_verify(cv, a, lp, b, lh, c, lr, d, ls) == 0 ? "ok" : "bad");
        } else if (!strcmp(op, "rsa_pkcs1") || !strcmp(op, "rsa_pss")) {
            enum hash_alg alg = alg_of(w[1]);
            size_t ln = unhex(w[2], a), le = unhex(w[3], b), lh = unhex(w[4], c), ls = unhex(w[5], d);
            (void)lh;
            int r = op[4] == 'p' && op[5] == 'k' ? rsa_verify_pkcs1(a, ln, b, le, alg, c, d, ls)
                                                 : rsa_verify_pss(a, ln, b, le, alg, c, d, ls);
            printf("%s\n", r == 0 ? "ok" : "bad");
        } else {
            printf("unknown\n");
        }
        (void)e;
        fflush(stdout);
    }
    return 0;
}
