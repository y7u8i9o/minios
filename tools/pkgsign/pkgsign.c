/* pkgsign is the host side of signed package repositories
 * (docs/design/packages.md). It compiles lib/libc/src/crypto/sha2.c and
 * ed25519.c, the code with which pkg(1) verifies on minios.
 *
 *   pkgsign keygen SECRET
 *   pkgsign public SECRET ORIGIN...
 *   pkgsign sign SECRET FILE
 *   pkgsign verify PUBLIC FILE
 *   pkgsign digest FILE
 *
 * keygen writes a new secret key with mode 0600, public prints the public
 * key file of a secret key for the named origins, sign writes the
 * signature FILE.sig, verify checks FILE.sig against a public key file,
 * and digest prints the size and SHA-256 of FILE. A secret key file
 * contains `ed25519-secret HEX` with the 32 byte seed of RFC 8032. A
 * public key file contains `ed25519 HEX` and one line `origin NAME` for
 * each origin whose indexes the key may sign. A signature file contains
 * `ed25519 KEYID HEX`, where KEYID is the first eight bytes of the
 * SHA-256 of the public key. */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <minios/ed25519.h>
#include <minios/sha2.h>

static void die(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "pkgsign: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

static uint8_t *read_all(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        die("%s: %s", path, strerror(errno));
    size_t cap = 65536, n = 0;
    uint8_t *data = malloc(cap);
    for (;;) {
        if (n == cap)
            data = realloc(data, cap *= 2);
        size_t r = fread(data + n, 1, cap - n, f);
        if (r == 0)
            break;
        n += r;
    }
    if (ferror(f))
        die("%s: %s", path, strerror(errno));
    fclose(f);
    *len = n;
    return data;
}

static void hex(char *out, const uint8_t *data, size_t n)
{
    for (size_t i = 0; i < n; i++)
        sprintf(out + 2 * i, "%02x", data[i]);
}

static int unhex(uint8_t *out, size_t n, const char *s)
{
    if (strlen(s) != 2 * n)
        return -1;
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(s + 2 * i, "%2x", &v) != 1)
            return -1;
        out[i] = (uint8_t)v;
    }
    return 0;
}

/* read_key decodes the single `KIND HEX` line of a key file into n bytes. */
static void read_key(const char *path, const char *kind, uint8_t *out, size_t n)
{
    size_t len;
    char *text = (char *)read_all(path, &len), k[32], h[160];
    text = realloc(text, len + 1);
    text[len] = '\0';
    if (sscanf(text, "%31s %159s", k, h) != 2 || strcmp(k, kind) != 0 || unhex(out, n, h) < 0)
        die("%s: not a %s key file", path, kind);
    free(text);
}

/* An origin name consists of letters, digits and the characters ._-, at
 * most 63 of them, as pkg accepts it. */
static int origin_valid(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n > 63)
        return 0;
    for (; *s; s++)
        if (!isalnum((unsigned char)*s) && !strchr("._-", *s))
            return 0;
    return 1;
}

static void key_id(char id[17], const uint8_t pub[ED25519_PUBLIC_SIZE])
{
    uint8_t d[SHA256_DIGEST_SIZE];
    sha256(pub, ED25519_PUBLIC_SIZE, d);
    hex(id, d, 8);
}

static int keygen(const char *path)
{
    uint8_t seed[ED25519_SEED_SIZE];
    char text[2 * ED25519_SEED_SIZE + 1];
    FILE *r = fopen("/dev/urandom", "rb");
    if (!r || fread(seed, 1, sizeof seed, r) != sizeof seed)
        die("/dev/urandom: %s", strerror(errno));
    fclose(r);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
        die("%s: %s", path, strerror(errno));
    hex(text, seed, sizeof seed);
    FILE *f = fdopen(fd, "w");
    fprintf(f, "ed25519-secret %s\n", text);
    if (fclose(f) != 0)
        die("%s: %s", path, strerror(errno));
    memset(seed, 0, sizeof seed);
    return 0;
}

/* public prints the public key file of a secret key: the key line and
 * one line `origin NAME` for each origin whose indexes the key may sign. */
static int public(const char *secret, int norigins, char **origins)
{
    uint8_t seed[ED25519_SEED_SIZE], pub[ED25519_PUBLIC_SIZE];
    char text[2 * ED25519_PUBLIC_SIZE + 1];
    for (int i = 0; i < norigins; i++)
        if (!origin_valid(origins[i]))
            die("%s: not a valid origin name", origins[i]);
    read_key(secret, "ed25519-secret", seed, sizeof seed);
    ed25519_public_key(pub, seed);
    hex(text, pub, sizeof pub);
    printf("ed25519 %s\n", text);
    for (int i = 0; i < norigins; i++)
        printf("origin %s\n", origins[i]);
    return 0;
}

static int sign(const char *secret, const char *file)
{
    uint8_t seed[ED25519_SEED_SIZE], pub[ED25519_PUBLIC_SIZE], sig[ED25519_SIGNATURE_SIZE];
    char id[17], text[2 * ED25519_SIGNATURE_SIZE + 1], sigpath[4096];
    size_t len;
    read_key(secret, "ed25519-secret", seed, sizeof seed);
    uint8_t *data = read_all(file, &len);
    ed25519_public_key(pub, seed);
    ed25519_sign(sig, data, len, seed);
    if (!ed25519_verify(sig, data, len, pub))
        die("%s: the new signature does not verify", file);
    key_id(id, pub);
    hex(text, sig, sizeof sig);
    snprintf(sigpath, sizeof sigpath, "%s.sig", file);
    FILE *f = fopen(sigpath, "w");
    if (!f)
        die("%s: %s", sigpath, strerror(errno));
    fprintf(f, "ed25519 %s %s\n", id, text);
    if (fclose(f) != 0)
        die("%s: %s", sigpath, strerror(errno));
    free(data);
    memset(seed, 0, sizeof seed);
    return 0;
}

static int verify(const char *public_file, const char *file)
{
    uint8_t pub[ED25519_PUBLIC_SIZE], sig[ED25519_SIGNATURE_SIZE];
    char id[17], sigpath[4096], kind[16], sid[32], shex[160];
    size_t len, siglen;
    read_key(public_file, "ed25519", pub, sizeof pub);
    snprintf(sigpath, sizeof sigpath, "%s.sig", file);
    char *sigtext = (char *)read_all(sigpath, &siglen);
    sigtext = realloc(sigtext, siglen + 1);
    sigtext[siglen] = '\0';
    if (sscanf(sigtext, "%15s %31s %159s", kind, sid, shex) != 3 || strcmp(kind, "ed25519") != 0 ||
        unhex(sig, sizeof sig, shex) < 0)
        die("%s: malformed signature file", sigpath);
    key_id(id, pub);
    if (strcmp(id, sid) != 0) {
        fprintf(stderr, "pkgsign: %s: signed by key %s, not %s\n", file, sid, id);
        return 1;
    }
    uint8_t *data = read_all(file, &len);
    if (!ed25519_verify(sig, data, len, pub)) {
        fprintf(stderr, "pkgsign: %s: the signature does not verify\n", file);
        return 1;
    }
    free(data);
    free(sigtext);
    return 0;
}

static int digest(const char *file)
{
    size_t len;
    uint8_t d[SHA256_DIGEST_SIZE];
    char text[2 * SHA256_DIGEST_SIZE + 1];
    uint8_t *data = read_all(file, &len);
    sha256(data, len, d);
    hex(text, d, sizeof d);
    printf("%zu %s\n", len, text);
    free(data);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "keygen") == 0)
        return keygen(argv[2]);
    if (argc >= 4 && strcmp(argv[1], "public") == 0)
        return public(argv[2], argc - 3, argv + 3);
    if (argc == 4 && strcmp(argv[1], "sign") == 0)
        return sign(argv[2], argv[3]);
    if (argc == 4 && strcmp(argv[1], "verify") == 0)
        return verify(argv[2], argv[3]);
    if (argc == 3 && strcmp(argv[1], "digest") == 0)
        return digest(argv[2]);
    fprintf(stderr, "usage: pkgsign keygen SECRET\n"
                    "       pkgsign public SECRET ORIGIN...\n"
                    "       pkgsign sign SECRET FILE\n"
                    "       pkgsign verify PUBLIC FILE\n"
                    "       pkgsign digest FILE\n");
    return 2;
}
