/* X.509 certificates (minios/x509.h): a DER reader, the certificate
 * fields of RFC 5280 that a TLS client needs, PEM decoding, the trust
 * store, and the path search of x509_verify_chain. */
#include <minios/x509.h>
#include <minios/base64.h>
#include <minios/crypto.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_DEPTH 8                     /* intermediates between the leaf and the anchor */
#define MAX_CHAIN 32                    /* certificates of a chain that the search considers */

/* A DER element: its content lies from p to end. */
struct der {
    const uint8_t *p, *end;
};

enum {
    TAG_BOOLEAN = 0x01,
    TAG_INTEGER = 0x02,
    TAG_BIT_STRING = 0x03,
    TAG_OCTET_STRING = 0x04,
    TAG_NULL = 0x05,
    TAG_OID = 0x06,
    TAG_UTC_TIME = 0x17,
    TAG_GENERALIZED_TIME = 0x18,
    TAG_SEQUENCE = 0x30,
    TAG_EXPLICIT_0 = 0xa0,
    TAG_EXPLICIT_3 = 0xa3,
    TAG_DNS_NAME = 0x82,
    TAG_IP_ADDRESS = 0x87,
};

/* Reads the next element of d. *tag receives its tag, *out its content,
 * and *start the first byte of the tag. Returns 0, or -1 for an invalid
 * or truncated element. */
static int der_read(struct der *d, uint8_t *tag, struct der *out, const uint8_t **start)
{
    const uint8_t *p = d->p;
    if (d->end - p < 2)
        return -1;
    if ((p[0] & 0x1f) == 0x1f)
        return -1;                      /* tag numbers above 30 do not occur in certificates */
    *tag = *p++;
    if (start)
        *start = d->p;
    size_t len = *p++;
    if (len & 0x80) {
        int n = (int)(len & 0x7f);
        if (n == 0 || n > 3 || d->end - p < n)
            return -1;
        len = 0;
        while (n--)
            len = len << 8 | *p++;
    }
    if ((size_t)(d->end - p) < len)
        return -1;
    out->p = p;
    out->end = p + len;
    d->p = p + len;
    return 0;
}

/* Reads the next element and requires the tag. */
static int der_expect(struct der *d, uint8_t tag, struct der *out)
{
    uint8_t t;
    if (der_read(d, &t, out, NULL) < 0 || t != tag)
        return -1;
    return 0;
}

/* The tag of the next element, or 0 at the end. */
static uint8_t der_peek(const struct der *d)
{
    return d->p < d->end ? d->p[0] : 0;
}

static size_t der_len(const struct der *d)
{
    return (size_t)(d->end - d->p);
}

static int oid_is(const struct der *oid, const uint8_t *want, size_t len)
{
    return der_len(oid) == len && memcmp(oid->p, want, len) == 0;
}

#define OID_IS(oid, ...) oid_is(oid, (const uint8_t[]){ __VA_ARGS__ }, sizeof((const uint8_t[]){ __VA_ARGS__ }))

/* The digits of a time field from s, n characters. Returns -1 for a
 * character that is not a digit. */
static int digits(const uint8_t *s, int n)
{
    int v = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9')
            return -1;
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

/* UTCTime YYMMDDHHMMSSZ or GeneralizedTime YYYYMMDDHHMMSSZ (RFC 5280,
 * section 4.1.2.5). */
static int parse_time(struct der *d, int64_t *out)
{
    uint8_t tag;
    struct der t;
    if (der_read(d, &tag, &t, NULL) < 0)
        return -1;
    const uint8_t *s = t.p;
    size_t len = der_len(&t);
    int year;
    if (tag == TAG_UTC_TIME && len == 13) {
        year = digits(s, 2);
        if (year < 0)
            return -1;
        year += year >= 50 ? 1900 : 2000;
        s += 2;
    } else if (tag == TAG_GENERALIZED_TIME && len == 15) {
        year = digits(s, 4);
        s += 4;
    } else {
        return -1;
    }
    int mon = digits(s, 2), day = digits(s + 2, 2), hour = digits(s + 4, 2), min = digits(s + 6, 2),
        sec = digits(s + 8, 2);
    if (year < 0 || mon < 1 || mon > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 || min < 0 || min > 59 ||
        sec < 0 || sec > 60 || s[10] != 'Z')
        return -1;
    struct tm tm = { .tm_year = year - 1900, .tm_mon = mon - 1, .tm_mday = day, .tm_hour = hour, .tm_min = min,
                     .tm_sec = sec };
    *out = (int64_t)timegm(&tm);
    return 0;
}

/* An AlgorithmIdentifier of a signature. Parameters must be absent or
 * NULL. *alg and *alg_len receive the whole element. */
static int parse_sig_alg(struct der *d, struct x509_cert *c, const uint8_t **alg, size_t *alg_len)
{
    struct der seq, oid, rest;
    uint8_t tag;
    if (der_read(d, &tag, &seq, alg) < 0 || tag != TAG_SEQUENCE || der_expect(&seq, TAG_OID, &oid) < 0)
        return -1;
    *alg_len = (size_t)(seq.end - *alg);
    if (der_peek(&seq) == TAG_NULL && der_expect(&seq, TAG_NULL, &rest) < 0)
        return -1;
    c->sig_supported = 1;
    if (OID_IS(&oid, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02))
        c->sig_alg = X509_SIG_ECDSA_SHA256;
    else if (OID_IS(&oid, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x03))
        c->sig_alg = X509_SIG_ECDSA_SHA384;
    else if (OID_IS(&oid, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x04))
        c->sig_alg = X509_SIG_ECDSA_SHA512;
    else if (OID_IS(&oid, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0b))
        c->sig_alg = X509_SIG_RSA_SHA256;
    else if (OID_IS(&oid, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0c))
        c->sig_alg = X509_SIG_RSA_SHA384;
    else if (OID_IS(&oid, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0d))
        c->sig_alg = X509_SIG_RSA_SHA512;
    else
        c->sig_supported = 0;
    return 0;
}

/* The content of a BIT STRING without unused bits. */
static int bit_string(struct der *d, struct der *out)
{
    if (der_expect(d, TAG_BIT_STRING, out) < 0 || der_len(out) < 1 || out->p[0] != 0)
        return -1;
    out->p++;
    return 0;
}

/* SubjectPublicKeyInfo with an EC key on P-256 or P-384, or an RSA key. */
static int parse_key(struct der *d, struct x509_cert *c)
{
    struct der spki, alg, oid, key;
    if (der_expect(d, TAG_SEQUENCE, &spki) < 0 || der_expect(&spki, TAG_SEQUENCE, &alg) < 0 ||
        der_expect(&alg, TAG_OID, &oid) < 0 || bit_string(&spki, &key) < 0)
        return -1;
    c->key_supported = 0;
    if (OID_IS(&oid, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01)) {
        struct der curve;
        if (der_expect(&alg, TAG_OID, &curve) < 0)
            return 0;
        if (OID_IS(&curve, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07) && der_len(&key) == 65)
            c->key_type = X509_KEY_EC_P256;
        else if (OID_IS(&curve, 0x2b, 0x81, 0x04, 0x00, 0x22) && der_len(&key) == 97)
            c->key_type = X509_KEY_EC_P384;
        else
            return 0;
        c->key = key.p;
        c->key_len = der_len(&key);
        c->key_supported = 1;
    } else if (OID_IS(&oid, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01)) {
        struct der rsa, n, e;
        if (der_expect(&key, TAG_SEQUENCE, &rsa) < 0 || der_expect(&rsa, TAG_INTEGER, &n) < 0 ||
            der_expect(&rsa, TAG_INTEGER, &e) < 0)
            return -1;
        c->key_type = X509_KEY_RSA;
        c->key = n.p;
        c->key_len = der_len(&n);
        c->rsa_e = e.p;
        c->rsa_e_len = der_len(&e);
        c->key_supported = 1;
    }
    return 0;
}

/* A small non-negative INTEGER. */
static int small_int(struct der *d, int *out)
{
    struct der v;
    if (der_expect(d, TAG_INTEGER, &v) < 0 || der_len(&v) < 1 || der_len(&v) > 3 || (v.p[0] & 0x80))
        return -1;
    int x = 0;
    for (const uint8_t *p = v.p; p < v.end; p++)
        x = x << 8 | *p;
    *out = x;
    return 0;
}

static int parse_basic_constraints(struct der *value, struct x509_cert *c)
{
    struct der seq, b;
    if (der_expect(value, TAG_SEQUENCE, &seq) < 0)
        return -1;
    if (der_peek(&seq) == TAG_BOOLEAN) {
        if (der_expect(&seq, TAG_BOOLEAN, &b) < 0 || der_len(&b) != 1)
            return -1;
        c->is_ca = b.p[0] != 0;
    }
    if (der_peek(&seq) == TAG_INTEGER && small_int(&seq, &c->path_len) < 0)
        return -1;
    return 0;
}

static int parse_key_usage(struct der *value, struct x509_cert *c)
{
    struct der bits;
    if (der_expect(value, TAG_BIT_STRING, &bits) < 0 || der_len(&bits) < 1)
        return -1;
    c->key_usage = 0;
    for (int i = 0; i < 9 && 1 + i / 8 < (int)der_len(&bits); i++)
        if (bits.p[1 + i / 8] & (0x80 >> (i % 8)))
            c->key_usage |= 1 << i;
    return 0;
}

static int parse_eku(struct der *value, struct x509_cert *c)
{
    struct der seq, oid;
    if (der_expect(value, TAG_SEQUENCE, &seq) < 0)
        return -1;
    c->has_eku = 1;
    while (der_len(&seq)) {
        if (der_expect(&seq, TAG_OID, &oid) < 0)
            return -1;
        if (OID_IS(&oid, 0x2b, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x01) || OID_IS(&oid, 0x55, 0x1d, 0x25, 0x00))
            c->eku_server = 1;
    }
    return 0;
}

static int parse_extensions(struct der *d, struct x509_cert *c)
{
    struct der outer, list;
    if (der_expect(d, TAG_EXPLICIT_3, &outer) < 0 || der_expect(&outer, TAG_SEQUENCE, &list) < 0)
        return -1;
    while (der_len(&list)) {
        struct der ext, oid, crit, value;
        int critical = 0;
        if (der_expect(&list, TAG_SEQUENCE, &ext) < 0 || der_expect(&ext, TAG_OID, &oid) < 0)
            return -1;
        if (der_peek(&ext) == TAG_BOOLEAN) {
            if (der_expect(&ext, TAG_BOOLEAN, &crit) < 0 || der_len(&crit) != 1)
                return -1;
            critical = crit.p[0] != 0;
        }
        if (der_expect(&ext, TAG_OCTET_STRING, &value) < 0)
            return -1;
        int r = 0;
        if (OID_IS(&oid, 0x55, 0x1d, 0x13)) {
            r = parse_basic_constraints(&value, c);
        } else if (OID_IS(&oid, 0x55, 0x1d, 0x0f)) {
            r = parse_key_usage(&value, c);
        } else if (OID_IS(&oid, 0x55, 0x1d, 0x25)) {
            r = parse_eku(&value, c);
        } else if (OID_IS(&oid, 0x55, 0x1d, 0x11)) {
            struct der names;
            r = der_expect(&value, TAG_SEQUENCE, &names);
            c->san = names.p;
            c->san_len = der_len(&names);
        } else if (critical) {
            c->unknown_critical = 1;
        }
        if (r < 0)
            return -1;
    }
    return 0;
}

int x509_parse(struct x509_cert *c, const uint8_t *der, size_t len)
{
    memset(c, 0, sizeof *c);
    c->path_len = -1;
    c->key_usage = -1;
    struct der all = { der, der + len }, cert, tbs, rest, version, serial, name, validity, sig;
    const uint8_t *start;
    uint8_t tag;
    if (der_expect(&all, TAG_SEQUENCE, &cert) < 0 || all.p != all.end)
        return X509_ERR_PARSE;
    c->der = der;
    c->der_len = len;
    if (der_read(&cert, &tag, &tbs, &start) < 0 || tag != TAG_SEQUENCE)
        return X509_ERR_PARSE;
    c->tbs = start;
    c->tbs_len = (size_t)(tbs.end - start);
    const uint8_t *outer_alg, *inner_alg;
    size_t outer_alg_len, inner_alg_len;
    if (parse_sig_alg(&cert, c, &outer_alg, &outer_alg_len) < 0 || bit_string(&cert, &sig) < 0 || cert.p != cert.end)
        return X509_ERR_PARSE;
    c->sig = sig.p;
    c->sig_len = der_len(&sig);

    if (der_peek(&tbs) == TAG_EXPLICIT_0 && der_expect(&tbs, TAG_EXPLICIT_0, &version) < 0)
        return X509_ERR_PARSE;
    if (der_expect(&tbs, TAG_INTEGER, &serial) < 0)
        return X509_ERR_PARSE;
    /* The algorithm inside the signed part must equal the outer one. */
    if (der_read(&tbs, &tag, &rest, &inner_alg) < 0 || tag != TAG_SEQUENCE)
        return X509_ERR_PARSE;
    inner_alg_len = (size_t)(rest.end - inner_alg);
    if (inner_alg_len != outer_alg_len || memcmp(inner_alg, outer_alg, inner_alg_len) != 0)
        return X509_ERR_PARSE;
    if (der_read(&tbs, &tag, &name, &start) < 0 || tag != TAG_SEQUENCE)
        return X509_ERR_PARSE;
    c->issuer = start;
    c->issuer_len = (size_t)(name.end - start);
    if (der_expect(&tbs, TAG_SEQUENCE, &validity) < 0 || parse_time(&validity, &c->not_before) < 0 ||
        parse_time(&validity, &c->not_after) < 0)
        return X509_ERR_PARSE;
    if (der_read(&tbs, &tag, &name, &start) < 0 || tag != TAG_SEQUENCE)
        return X509_ERR_PARSE;
    c->subject = start;
    c->subject_len = (size_t)(name.end - start);
    if (parse_key(&tbs, c) < 0)
        return X509_ERR_PARSE;
    /* issuerUniqueID [1] and subjectUniqueID [2] carry nothing of use. */
    while (der_peek(&tbs) == 0x81 || der_peek(&tbs) == 0x82)
        if (der_read(&tbs, &tag, &rest, NULL) < 0)
            return X509_ERR_PARSE;
    if (der_peek(&tbs) == TAG_EXPLICIT_3 && parse_extensions(&tbs, c) < 0)
        return X509_ERR_PARSE;
    if (tbs.p != tbs.end)
        return X509_ERR_PARSE;
    return X509_OK;
}

int x509_pem_decode(const char *text, size_t len, uint8_t ***ders, size_t **lens, int *skipped)
{
    static const char begin[] = "-----BEGIN CERTIFICATE-----", end[] = "-----END CERTIFICATE-----";
    uint8_t **d = NULL;
    size_t *l = NULL;
    int count = 0, capacity = 0, bad = 0;
    const char *p = text, *stop = text + len;
    for (;;) {
        const char *b = memmem(p, (size_t)(stop - p), begin, sizeof begin - 1);
        if (!b)
            break;
        b += sizeof begin - 1;
        const char *e = memmem(b, (size_t)(stop - b), end, sizeof end - 1);
        if (!e)
            break;
        p = e + sizeof end - 1;
        if (count == capacity) {
            capacity = capacity ? capacity * 2 : 16;
            uint8_t **nd = realloc(d, (size_t)capacity * sizeof *nd);
            size_t *nl = nd ? realloc(l, (size_t)capacity * sizeof *nl) : NULL;
            if (nd)
                d = nd;
            if (!nl) {
                x509_free_ders(d, l, count);
                return -1;
            }
            l = nl;
        }
        uint8_t *buf = malloc((size_t)(e - b) / 4 * 3 + 3);
        if (!buf) {
            x509_free_ders(d, l, count);
            return -1;
        }
        long n = base64_decode(b, (size_t)(e - b), buf);
        if (n <= 0) {
            free(buf);
            bad++;
            continue;
        }
        d[count] = buf;
        l[count++] = (size_t)n;
    }
    *ders = d;
    *lens = l;
    if (skipped)
        *skipped = bad;
    return count;
}

void x509_free_ders(uint8_t **ders, size_t *lens, int count)
{
    for (int i = 0; i < count; i++)
        free(ders[i]);
    free(ders);
    free(lens);
}

int x509_store_add_pem(struct x509_store *s, const char *text, size_t len, int *skipped)
{
    uint8_t **ders;
    size_t *lens;
    int bad = 0, added = 0;
    int n = x509_pem_decode(text, len, &ders, &lens, &bad);
    if (n < 0)
        return -1;
    for (int i = 0; i < n; i++) {
        if (s->count == s->capacity) {
            int capacity = s->capacity ? s->capacity * 2 : 64;
            struct x509_cert *nc = realloc(s->certs, (size_t)capacity * sizeof *nc);
            if (!nc) {
                for (int j = i; j < n; j++)
                    free(ders[j]);
                free(ders);
                free(lens);
                return -1;
            }
            s->certs = nc;
            s->capacity = capacity;
        }
        if (x509_parse(&s->certs[s->count], ders[i], lens[i]) != X509_OK) {
            free(ders[i]);
            bad++;
            continue;
        }
        s->count++;
        added++;
    }
    free(ders);
    free(lens);
    if (skipped)
        *skipped = bad;
    return added;
}

int x509_store_load(struct x509_store *s, const char *path, int *skipped)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -errno;
    struct stat st;
    if (fstat(fd, &st) < 0) {
        int err = errno;
        close(fd);
        return -err;
    }
    size_t size = (size_t)st.st_size, got = 0;
    char *text = malloc(size + 1);
    if (!text) {
        close(fd);
        return -ENOMEM;
    }
    while (got < size) {
        ssize_t r = read(fd, text + got, size - got);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        got += (size_t)r;
    }
    close(fd);
    int n = x509_store_add_pem(s, text, got, skipped);
    free(text);
    return n < 0 ? -ENOMEM : n;
}

void x509_store_free(struct x509_store *s)
{
    for (int i = 0; i < s->count; i++)
        free((void *)s->certs[i].der);
    free(s->certs);
    memset(s, 0, sizeof *s);
}

static enum hash_alg hash_of(enum x509_sig alg)
{
    switch (alg) {
    case X509_SIG_ECDSA_SHA256:
    case X509_SIG_RSA_SHA256:
        return HASH_SHA256;
    case X509_SIG_ECDSA_SHA384:
    case X509_SIG_RSA_SHA384:
        return HASH_SHA384;
    default:
        return HASH_SHA512;
    }
}

int x509_check_signature(const struct x509_cert *c, const struct x509_cert *issuer)
{
    if (!c->sig_supported || !issuer->key_supported)
        return X509_ERR_ALGORITHM;
    enum hash_alg alg = hash_of(c->sig_alg);
    uint8_t digest[HASH_MAX_SIZE];
    hash(alg, c->tbs, c->tbs_len, digest);
    int ecdsa = c->sig_alg <= X509_SIG_ECDSA_SHA512;
    if (ecdsa != (issuer->key_type != X509_KEY_RSA))
        return X509_ERR_ALGORITHM;
    if (!ecdsa)
        return rsa_verify_pkcs1(issuer->key, issuer->key_len, issuer->rsa_e, issuer->rsa_e_len, alg, digest, c->sig,
                                c->sig_len) == 0
                   ? X509_OK
                   : X509_ERR_SIGNATURE;
    /* Ecdsa-Sig-Value ::= SEQUENCE { r INTEGER, s INTEGER } */
    struct der sig = { c->sig, c->sig + c->sig_len }, seq, r, s;
    if (der_expect(&sig, TAG_SEQUENCE, &seq) < 0 || der_expect(&seq, TAG_INTEGER, &r) < 0 ||
        der_expect(&seq, TAG_INTEGER, &s) < 0 || seq.p != seq.end)
        return X509_ERR_SIGNATURE;
    enum ec_curve curve = issuer->key_type == X509_KEY_EC_P256 ? EC_P256 : EC_P384;
    return ecdsa_verify(curve, issuer->key, issuer->key_len, digest, hash_size(alg), r.p, der_len(&r), s.p,
                        der_len(&s)) == 0
               ? X509_OK
               : X509_ERR_SIGNATURE;
}

int x509_check_host(const struct x509_cert *c, const char *host)
{
    size_t hlen = strlen(host);
    if (hlen && host[hlen - 1] == '.')
        hlen--;
    if (!hlen)
        return X509_ERR_HOST;
    uint8_t addr[4];
    int literal = hlen == strlen(host) && inet_pton(AF_INET, host, addr) == 1;
    struct der names = { c->san, c->san + c->san_len }, name;
    uint8_t tag;
    while (der_len(&names)) {
        if (der_read(&names, &tag, &name, NULL) < 0)
            return X509_ERR_HOST;
        const char *n = (const char *)name.p;
        size_t nlen = der_len(&name);
        if (literal) {
            if (tag == TAG_IP_ADDRESS && nlen == 4 && memcmp(n, addr, 4) == 0)
                return X509_OK;
            continue;
        }
        if (tag != TAG_DNS_NAME || nlen == 0 || memchr(n, 0, nlen))
            continue;
        if (nlen > 2 && n[0] == '*' && n[1] == '.') {
            /* The wildcard replaces exactly one label. The rest must have
             * at least two labels. */
            const char *rest = n + 2;
            size_t rlen = nlen - 2;
            const char *dot = memchr(host, '.', hlen);
            if (!memchr(rest, '.', rlen) || !dot || dot == host)
                continue;
            size_t tail = hlen - (size_t)(dot + 1 - host);
            if (tail == rlen && strncasecmp(dot + 1, rest, rlen) == 0)
                return X509_OK;
        } else if (nlen == hlen && strncasecmp(host, n, hlen) == 0) {
            return X509_OK;
        }
    }
    return X509_ERR_HOST;
}

static int check_time(const struct x509_cert *c, int64_t now)
{
    if (now < c->not_before)
        return X509_ERR_NOT_YET_VALID;
    if (now > c->not_after)
        return X509_ERR_EXPIRED;
    return X509_OK;
}

/* The state of the path search. */
struct path {
    const struct x509_cert *chain;
    int count;
    const struct x509_store *store;
    int64_t now;
    uint32_t used;                      /* certificates of the chain on the current path */
    int best_err;                       /* the error at the greatest depth */
    int best_depth;
};

static void note(struct path *p, int err, int depth)
{
    if (depth > p->best_depth) {
        p->best_err = err;
        p->best_depth = depth;
    }
}

static int same_name(const uint8_t *a, size_t alen, const uint8_t *b, size_t blen)
{
    return alen == blen && memcmp(a, b, alen) == 0;
}

/* Checks a certificate of the chain that issues the certificate at
 * depth: below the issuer lie depth intermediates. */
static int check_ca(const struct path *p, const struct x509_cert *ca, int depth)
{
    int r = check_time(ca, p->now);
    if (r != X509_OK)
        return r;
    if (ca->unknown_critical)
        return X509_ERR_CRITICAL;
    if (!ca->is_ca || (ca->path_len >= 0 && depth > ca->path_len))
        return X509_ERR_CA;
    if ((ca->key_usage >= 0 && !(ca->key_usage & X509_KU_KEY_CERT_SIGN)) || (ca->has_eku && !ca->eku_server))
        return X509_ERR_USAGE;
    return X509_OK;
}

/* Extends the path from the certificate c at depth (0 is the leaf) to an
 * anchor of the store. Anchors come before the other certificates of the
 * chain, so a cross-signed certificate is used only when no anchor
 * issued the certificate. */
static int extend(struct path *p, const struct x509_cert *c, int depth)
{
    const struct x509_store *s = p->store;
    for (int i = 0; i < s->count; i++)
        if (s->certs[i].der_len == c->der_len && memcmp(s->certs[i].der, c->der, c->der_len) == 0)
            return X509_OK;
    for (int i = 0; i < s->count; i++) {
        const struct x509_cert *a = &s->certs[i];
        if (!same_name(a->subject, a->subject_len, c->issuer, c->issuer_len))
            continue;
        int r = x509_check_signature(c, a);
        if (r == X509_OK)
            return X509_OK;
        note(p, r, depth + 1);
    }
    if (depth >= MAX_DEPTH)
        return X509_ERR_ISSUER;
    for (int i = 1; i < p->count; i++) {
        const struct x509_cert *ic = &p->chain[i];
        if ((p->used >> i & 1) || !same_name(ic->subject, ic->subject_len, c->issuer, c->issuer_len))
            continue;
        int r = check_ca(p, ic, depth);
        if (r == X509_OK)
            r = x509_check_signature(c, ic);
        if (r != X509_OK) {
            note(p, r, depth + 1);
            continue;
        }
        p->used |= 1u << i;
        if (extend(p, ic, depth + 1) == X509_OK)
            return X509_OK;
        p->used &= ~(1u << i);
    }
    note(p, X509_ERR_ISSUER, depth);
    return X509_ERR_ISSUER;
}

int x509_verify_chain(const struct x509_cert *chain, int count, const struct x509_store *store, const char *host,
                      int64_t now)
{
    if (count < 1)
        return X509_ERR_ISSUER;
    const struct x509_cert *leaf = &chain[0];
    int r = check_time(leaf, now);
    if (r != X509_OK)
        return r;
    if (leaf->unknown_critical)
        return X509_ERR_CRITICAL;
    if (!leaf->key_supported)
        return X509_ERR_ALGORITHM;
    if ((leaf->key_usage >= 0 && !(leaf->key_usage & X509_KU_DIGITAL_SIGNATURE)) ||
        (leaf->has_eku && !leaf->eku_server))
        return X509_ERR_USAGE;
    r = x509_check_host(leaf, host);
    if (r != X509_OK)
        return r;
    struct path p = { chain, count < MAX_CHAIN ? count : MAX_CHAIN, store, now, 1, X509_ERR_ISSUER, -1 };
    if (extend(&p, leaf, 0) == X509_OK)
        return X509_OK;
    return p.best_err;
}

const char *x509_strerror(int err)
{
    switch (err) {
    case X509_OK:
        return "certificate valid";
    case X509_ERR_PARSE:
        return "invalid certificate encoding";
    case X509_ERR_ALGORITHM:
        return "unsupported certificate algorithm";
    case X509_ERR_EXPIRED:
        return "certificate expired";
    case X509_ERR_NOT_YET_VALID:
        return "certificate not yet valid";
    case X509_ERR_HOST:
        return "certificate not valid for the host";
    case X509_ERR_ISSUER:
        return "certificate issuer unknown";
    case X509_ERR_SIGNATURE:
        return "certificate signature invalid";
    case X509_ERR_CA:
        return "certificate issuer is no CA";
    case X509_ERR_USAGE:
        return "certificate key usage forbids the use";
    case X509_ERR_CRITICAL:
        return "certificate has an unknown critical extension";
    default:
        return "certificate error";
    }
}
