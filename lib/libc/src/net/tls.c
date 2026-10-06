/* The TLS 1.3 client of minios/tls.h (RFC 8446). The handshake runs in
 * tls_connect: ClientHello, an optional HelloRetryRequest, ServerHello,
 * and the encrypted EncryptedExtensions, CertificateRequest,
 * Certificate, CertificateVerify and Finished of the server. The client
 * answers with an empty Certificate when the server requested one, and
 * with its Finished. Both suites use SHA-256, so the transcript hash is
 * SHA-256 from the first message on. After the handshake, tls_read
 * processes NewSessionTicket and KeyUpdate messages of the server. */
#include "tls_internal.h"
#include "netio.h"
#include <arpa/inet.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

enum { STATE_HANDSHAKE, STATE_OPEN, STATE_CLOSED, STATE_FAILED };

enum {
    CT_CHANGE_CIPHER_SPEC = 20,
    CT_ALERT = 21,
    CT_HANDSHAKE = 22,
    CT_APPLICATION_DATA = 23,
};

enum {
    HS_CLIENT_HELLO = 1,
    HS_SERVER_HELLO = 2,
    HS_NEW_SESSION_TICKET = 4,
    HS_ENCRYPTED_EXTENSIONS = 8,
    HS_CERTIFICATE = 11,
    HS_CERTIFICATE_REQUEST = 13,
    HS_CERTIFICATE_VERIFY = 15,
    HS_FINISHED = 20,
    HS_KEY_UPDATE = 24,
    HS_MESSAGE_HASH = 254,
};

enum {
    EXT_SERVER_NAME = 0,
    EXT_SUPPORTED_GROUPS = 10,
    EXT_SIGNATURE_ALGORITHMS = 13,
    EXT_SUPPORTED_VERSIONS = 43,
    EXT_COOKIE = 44,
    EXT_KEY_SHARE = 51,
};

enum {
    ALERT_CLOSE_NOTIFY = 0,
    ALERT_UNEXPECTED_MESSAGE = 10,
    ALERT_BAD_RECORD_MAC = 20,
    ALERT_RECORD_OVERFLOW = 22,
    ALERT_HANDSHAKE_FAILURE = 40,
    ALERT_BAD_CERTIFICATE = 42,
    ALERT_UNSUPPORTED_CERTIFICATE = 43,
    ALERT_CERTIFICATE_EXPIRED = 45,
    ALERT_CERTIFICATE_UNKNOWN = 46,
    ALERT_ILLEGAL_PARAMETER = 47,
    ALERT_UNKNOWN_CA = 48,
    ALERT_DECODE_ERROR = 50,
    ALERT_DECRYPT_ERROR = 51,
    ALERT_PROTOCOL_VERSION = 70,
    ALERT_INTERNAL_ERROR = 80,
    ALERT_USER_CANCELED = 90,
    ALERT_MISSING_EXTENSION = 109,
    ALERT_UNSUPPORTED_EXTENSION = 110,
};

#define NO_ALERT (-1)

/* The signature schemes of the ClientHello. The PKCS#1 v1.5 schemes are
 * valid only for certificates. */
static const uint16_t sig_schemes[] = { 0x0403, 0x0503, 0x0804, 0x0805, 0x0806, 0x0401, 0x0501, 0x0601 };

/* SHA-256 of "HelloRetryRequest", the random of a HelloRetryRequest. */
static const uint8_t hrr_random[32] = {
    0xcf, 0x21, 0xad, 0x74, 0xe5, 0x9a, 0x61, 0x11, 0xbe, 0x1d, 0x8c, 0x02, 0x1e, 0x65, 0xb8, 0x91,
    0xc2, 0xa2, 0x11, 0x16, 0x7a, 0xbb, 0x8c, 0x5e, 0x07, 0x9e, 0x09, 0xe2, 0xc8, 0xa8, 0x33, 0x9c,
};

static const char *alert_name(int a)
{
    switch (a) {
    case ALERT_CLOSE_NOTIFY: return "close_notify";
    case ALERT_UNEXPECTED_MESSAGE: return "unexpected_message";
    case ALERT_BAD_RECORD_MAC: return "bad_record_mac";
    case ALERT_RECORD_OVERFLOW: return "record_overflow";
    case ALERT_HANDSHAKE_FAILURE: return "handshake_failure";
    case ALERT_BAD_CERTIFICATE: return "bad_certificate";
    case ALERT_UNSUPPORTED_CERTIFICATE: return "unsupported_certificate";
    case ALERT_CERTIFICATE_EXPIRED: return "certificate_expired";
    case ALERT_CERTIFICATE_UNKNOWN: return "certificate_unknown";
    case ALERT_ILLEGAL_PARAMETER: return "illegal_parameter";
    case ALERT_UNKNOWN_CA: return "unknown_ca";
    case ALERT_DECODE_ERROR: return "decode_error";
    case ALERT_DECRYPT_ERROR: return "decrypt_error";
    case ALERT_PROTOCOL_VERSION: return "protocol_version";
    case ALERT_INTERNAL_ERROR: return "internal_error";
    case ALERT_USER_CANCELED: return "user_canceled";
    case ALERT_MISSING_EXTENSION: return "missing_extension";
    case ALERT_UNSUPPORTED_EXTENSION: return "unsupported_extension";
    default: return "an unknown alert";
    }
}

/* ---- Reading and writing messages ---- */

/* A reader over received bytes. A read beyond the end sets bad and
 * returns zeros. */
struct reader {
    const uint8_t *p, *end;
    int bad;
};

static uint32_t get(struct reader *r, int n)
{
    if (r->end - r->p < n) {
        r->bad = 1;
        r->p = r->end;
        return 0;
    }
    uint32_t v = 0;
    while (n--)
        v = v << 8 | *r->p++;
    return v;
}

/* The vector with a length of n bytes at the position of r. */
static struct reader sub(struct reader *r, int n)
{
    size_t len = get(r, n);
    struct reader s = { r->p, r->p, r->bad };
    if (r->bad || (size_t)(r->end - r->p) < len) {
        r->bad = s.bad = 1;
        r->p = r->end;
        return s;
    }
    s.end = r->p + len;
    r->p += len;
    return s;
}

static size_t left(const struct reader *r)
{
    return (size_t)(r->end - r->p);
}

/* A writer into a buffer of fixed size. */
struct writer {
    uint8_t *p;
    size_t len, cap;
    int bad;
};

static void put(struct writer *w, uint32_t v, int n)
{
    if (w->cap - w->len < (size_t)n) {
        w->bad = 1;
        return;
    }
    while (n--)
        w->p[w->len++] = (uint8_t)(v >> (8 * n));
}

static void put_bytes(struct writer *w, const void *data, size_t n)
{
    if (w->cap - w->len < n) {
        w->bad = 1;
        return;
    }
    memcpy(w->p + w->len, data, n);
    w->len += n;
}

/* Reserves a length field of n bytes. close_len fills it with the length
 * of the bytes written after it. */
static size_t open_len(struct writer *w, int n)
{
    size_t at = w->len;
    put(w, 0, n);
    return at;
}

static void close_len(struct writer *w, size_t at, int n)
{
    if (w->bad)
        return;
    size_t len = w->len - at - (size_t)n;
    for (int i = 0; i < n; i++)
        w->p[at + (size_t)i] = (uint8_t)(len >> (8 * (n - 1 - i)));
}

/* ---- Key schedule (RFC 8446, section 7.1) ---- */

static void expand_label(const uint8_t secret[32], const char *label, const uint8_t *context, size_t context_len,
                         uint8_t *out, size_t len)
{
    uint8_t info[2 + 1 + 255 + 1 + 255];
    size_t label_len = strlen(label), n = 0;
    info[n++] = (uint8_t)(len >> 8);
    info[n++] = (uint8_t)len;
    info[n++] = (uint8_t)(6 + label_len);
    memcpy(info + n, "tls13 ", 6);
    n += 6;
    memcpy(info + n, label, label_len);
    n += label_len;
    info[n++] = (uint8_t)context_len;
    if (context_len)
        memcpy(info + n, context, context_len);
    n += context_len;
    hkdf_expand(HASH_SHA256, secret, info, n, out, len);
}

static void derive_secret(const uint8_t secret[32], const char *label, const uint8_t hash[32], uint8_t out[32])
{
    expand_label(secret, label, hash, 32, out, 32);
}

static void transcript_hash(const struct tls *t, uint8_t out[32])
{
    struct hash_ctx c = t->transcript;
    hash_final(&c, out);
}

void tls_keys_set(struct tls_keys *k, uint16_t suite, const uint8_t secret[32])
{
    k->suite = suite;
    memmove(k->secret, secret, 32);
    expand_label(k->secret, "key", NULL, 0, k->key, suite == TLS_AES_128_GCM_SHA256 ? 16 : 32);
    expand_label(k->secret, "iv", NULL, 0, k->iv, 12);
    k->seq = 0;
    k->active = 1;
}

void tls_keys_update(struct tls_keys *k)
{
    uint8_t next[32];
    expand_label(k->secret, "traffic upd", NULL, 0, next, 32);
    tls_keys_set(k, k->suite, next);
    crypto_wipe(next, sizeof next);
}

/* The nonce of a record: the IV with the sequence number in its last
 * eight bytes. */
static void record_nonce(const struct tls_keys *k, uint8_t nonce[12])
{
    memcpy(nonce, k->iv, 12);
    for (int i = 0; i < 8; i++)
        nonce[11 - i] ^= (uint8_t)(k->seq >> (8 * i));
}

size_t tls_record_seal(struct tls_keys *k, uint8_t type, const uint8_t *data, size_t len, uint8_t *out)
{
    size_t inner = len + 1, total = inner + AEAD_TAG_SIZE;
    memmove(out + 5, data, len);
    out[5 + len] = type;
    out[0] = CT_APPLICATION_DATA;
    out[1] = 3;
    out[2] = 3;
    out[3] = (uint8_t)(total >> 8);
    out[4] = (uint8_t)total;
    uint8_t nonce[12];
    record_nonce(k, nonce);
    if (k->suite == TLS_AES_128_GCM_SHA256)
        aes128_gcm_seal(k->key, nonce, out, 5, out + 5, inner, out + 5);
    else
        chacha20_poly1305_seal(k->key, nonce, out, 5, out + 5, inner, out + 5);
    k->seq++;
    return 5 + total;
}

int tls_record_open(struct tls_keys *k, const uint8_t *rec, size_t len, uint8_t *out, size_t *out_len, uint8_t *type)
{
    if (len < 5 + 1 + AEAD_TAG_SIZE)
        return -1;
    size_t clen = len - 5 - AEAD_TAG_SIZE;
    uint8_t nonce[12];
    record_nonce(k, nonce);
    int r = k->suite == TLS_AES_128_GCM_SHA256
                ? aes128_gcm_open(k->key, nonce, rec, 5, rec + 5, clen, rec + 5 + clen, out)
                : chacha20_poly1305_open(k->key, nonce, rec, 5, rec + 5, clen, rec + 5 + clen, out);
    if (r < 0)
        return -1;
    k->seq++;
    /* TLSInnerPlaintext: the content, its type, and zero padding. */
    while (clen && out[clen - 1] == 0)
        clen--;
    if (!clen)
        return -1;
    *type = out[clen - 1];
    *out_len = clen - 1;
    return 0;
}

/* ---- Records ---- */

static void send_alert(struct tls *t, int level, int alert);

/* Records a failure of the protocol, sends the alert unless it is
 * NO_ALERT, and returns -EPROTO. */
static int fail(struct tls *t, int alert, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static int fail(struct tls *t, int alert, const char *fmt, ...)
{
    if (t->state == STATE_FAILED)
        return -EPROTO;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t->error, sizeof t->error, fmt, ap);
    va_end(ap);
    /* The state changes first, so a failed write of the alert does not
     * replace the reason. */
    t->state = STATE_FAILED;
    if (alert != NO_ALERT)
        send_alert(t, 2, alert);
    return -EPROTO;
}

/* Records a failure of the socket and returns err. */
static int io_fail(struct tls *t, int err, const char *what)
{
    if (t->state != STATE_FAILED) {
        if (err == -ETIMEDOUT)
            snprintf(t->error, sizeof t->error, "%s: no data for %d seconds", what, t->timeout);
        else
            snprintf(t->error, sizeof t->error, "%s: %s", what, strerror(-err));
        t->state = STATE_FAILED;
    }
    return err;
}

/* Sends len bytes of the content type in records of at most
 * TLS_MAX_PLAIN bytes, encrypted once the client has write keys. */
static int send_records(struct tls *t, uint8_t type, const uint8_t *data, size_t len)
{
    do {
        size_t n = len < TLS_MAX_PLAIN ? len : TLS_MAX_PLAIN, size;
        if (t->wr.active) {
            size = tls_record_seal(&t->wr, type, data, n, t->out);
        } else {
            /* The record version of the ClientHello is 0x0301 for old
             * servers (RFC 8446, section 5.1). */
            t->out[0] = type;
            t->out[1] = 3;
            t->out[2] = type == CT_HANDSHAKE ? 1 : 3;
            t->out[3] = (uint8_t)(n >> 8);
            t->out[4] = (uint8_t)n;
            memcpy(t->out + 5, data, n);
            size = 5 + n;
        }
        int r = net_send(t->fd, t->out, size, t->timeout);
        if (r < 0)
            return io_fail(t, r, "write");
        data += n;
        len -= n;
    } while (len);
    return 0;
}

static void send_alert(struct tls *t, int level, int alert)
{
    uint8_t a[2] = { (uint8_t)level, (uint8_t)alert };
    send_records(t, CT_ALERT, a, 2);
}

/* Reads the next record into t->rec. Change cipher spec records of
 * middlebox compatibility are skipped, and so is the alert
 * user_canceled. Returns 0, 1 at the end of the stream or after
 * close_notify, or a negative errno. */
static int read_record(struct tls *t)
{
    for (;;) {
        size_t len;
        for (;;) {
            if (t->in_len >= 5) {
                len = (size_t)t->in[3] << 8 | t->in[4];
                if (len > TLS_MAX_RECORD - 5)
                    return fail(t, ALERT_RECORD_OVERFLOW, "the server sent a record of %zu bytes", len);
                if (t->in_len >= 5 + len)
                    break;
            }
            ssize_t n = net_recv(t->fd, t->in + t->in_len, sizeof t->in - t->in_len, t->timeout);
            if (n < 0)
                return io_fail(t, (int)n, "read");
            if (n == 0)
                return 1;
            t->in_len += (size_t)n;
        }
        uint8_t type = t->in[0];
        int r = 0;
        if (type == CT_CHANGE_CIPHER_SPEC) {
            if (len != 1 || t->in[5] != 1 || t->state != STATE_HANDSHAKE)
                r = fail(t, ALERT_UNEXPECTED_MESSAGE, "unexpected change_cipher_spec record");
            t->rec_len = 0;
            t->rec_type = 0;
        } else if (t->rd.active) {
            if (type != CT_APPLICATION_DATA)
                r = fail(t, ALERT_UNEXPECTED_MESSAGE, "unencrypted record of type %d", type);
            else if (tls_record_open(&t->rd, t->in, 5 + len, t->rec, &t->rec_len, &t->rec_type) < 0)
                r = fail(t, ALERT_BAD_RECORD_MAC, "a record of the server does not decrypt");
            else if (t->rec_len > TLS_MAX_PLAIN)
                r = fail(t, ALERT_RECORD_OVERFLOW, "the server sent a record of %zu bytes", t->rec_len);
        } else {
            if (type != CT_HANDSHAKE && type != CT_ALERT)
                r = fail(t, ALERT_UNEXPECTED_MESSAGE, "unexpected record of type %d", type);
            else if (len > TLS_MAX_PLAIN)
                r = fail(t, ALERT_RECORD_OVERFLOW, "the server sent a record of %zu bytes", len);
            memcpy(t->rec, t->in + 5, len < TLS_MAX_PLAIN ? len : TLS_MAX_PLAIN);
            t->rec_len = len;
            t->rec_type = type;
        }
        memmove(t->in, t->in + 5 + len, t->in_len - 5 - len);
        t->in_len -= 5 + len;
        t->rec_pos = 0;
        if (r < 0)
            return r;
        if (type == CT_CHANGE_CIPHER_SPEC)
            continue;
        if (t->rec_type == CT_ALERT) {
            if (t->rec_len != 2)
                return fail(t, ALERT_DECODE_ERROR, "malformed alert");
            if (t->rec[1] == ALERT_CLOSE_NOTIFY)
                return 1;
            if (t->rec[1] == ALERT_USER_CANCELED)
                continue;
            t->state = STATE_FAILED;
            snprintf(t->error, sizeof t->error, "the server sent the alert %s", alert_name(t->rec[1]));
            return -EPROTO;
        }
        if (t->rec_type != CT_APPLICATION_DATA && t->rec_type != CT_HANDSHAKE)
            return fail(t, ALERT_UNEXPECTED_MESSAGE, "unexpected record of type %d", t->rec_type);
        if (t->rec_len == 0 && t->rec_type != CT_APPLICATION_DATA)
            return fail(t, ALERT_UNEXPECTED_MESSAGE, "empty handshake record");
        return 0;
    }
}

/* Appends the handshake record of t->rec to the handshake buffer. */
static int append_handshake(struct tls *t)
{
    if (t->hs_pos == t->hs_len)
        t->hs_pos = t->hs_len = 0;
    if (t->hs_pos) {
        memmove(t->hs, t->hs + t->hs_pos, t->hs_len - t->hs_pos);
        t->hs_len -= t->hs_pos;
        t->hs_pos = 0;
    }
    if (t->hs_len + t->rec_len > t->hs_cap) {
        size_t cap = t->hs_cap ? t->hs_cap : 4096;
        while (cap < t->hs_len + t->rec_len)
            cap *= 2;
        if (cap > TLS_MAX_HANDSHAKE + 4 + TLS_MAX_PLAIN)
            return fail(t, ALERT_DECODE_ERROR, "handshake message too long");
        uint8_t *hs = realloc(t->hs, cap);
        if (!hs)
            return fail(t, ALERT_INTERNAL_ERROR, "out of memory");
        t->hs = hs;
        t->hs_cap = cap;
    }
    memcpy(t->hs + t->hs_len, t->rec, t->rec_len);
    t->hs_len += t->rec_len;
    return 0;
}

/* Returns 1 and the next complete handshake message of the buffer in
 * *msg and *len, with its header, or 0 when the buffer has no complete
 * message. */
static int buffered_message(struct tls *t, const uint8_t **msg, size_t *len)
{
    size_t avail = t->hs_len - t->hs_pos;
    if (avail < 4)
        return 0;
    const uint8_t *m = t->hs + t->hs_pos;
    size_t body = (size_t)m[1] << 16 | (size_t)m[2] << 8 | m[3];
    if (body > TLS_MAX_HANDSHAKE)
        return fail(t, ALERT_DECODE_ERROR, "handshake message of %zu bytes", body);
    if (avail < 4 + body)
        return 0;
    *msg = m;
    *len = 4 + body;
    t->hs_pos += 4 + body;
    return 1;
}

/* Reads the next handshake message of the handshake. The message lies in
 * the handshake buffer until the next call. */
static int next_message(struct tls *t, const uint8_t **msg, size_t *len)
{
    for (;;) {
        int r = buffered_message(t, msg, len);
        if (r != 0)
            return r < 0 ? r : 0;
        r = read_record(t);
        if (r < 0)
            return r;
        if (r == 1)
            return fail(t, NO_ALERT, "the server closed the connection during the handshake");
        if (t->rec_type != CT_HANDSHAKE)
            return fail(t, ALERT_UNEXPECTED_MESSAGE, "application data during the handshake");
        if ((r = append_handshake(t)) < 0)
            return r;
    }
}

/* Handshake messages must not span a change of the keys (RFC 8446,
 * section 5.1). */
static int check_key_change(struct tls *t)
{
    if (t->hs_pos != t->hs_len)
        return fail(t, ALERT_UNEXPECTED_MESSAGE, "a handshake message spans a key change");
    return 0;
}

/* ---- The handshake ---- */

static int random_bytes(struct tls *t, void *buf, size_t len)
{
    if (getentropy(buf, len) < 0)
        return fail(t, NO_ALERT, "no random numbers: %s", strerror(errno));
    return 0;
}

static int host_is_address(const char *host)
{
    uint8_t a[4];
    return inet_pton(AF_INET, host, a) == 1;
}

/* Builds the ClientHello. After a HelloRetryRequest, group selects the
 * only key share and cookie the cookie of the server. */
static int build_client_hello(struct tls *t, struct writer *w, uint16_t group, const uint8_t *cookie,
                              size_t cookie_len)
{
    put(w, HS_CLIENT_HELLO, 1);
    size_t msg = open_len(w, 3);
    put(w, 0x0303, 2);
    put_bytes(w, t->random, 32);
    put(w, 0, 1);                       /* legacy_session_id */
    size_t suites = open_len(w, 2);
    if (!t->opt.suite || t->opt.suite == TLS_CHACHA20_POLY1305_SHA256)
        put(w, TLS_CHACHA20_POLY1305_SHA256, 2);
    if (!t->opt.suite || t->opt.suite == TLS_AES_128_GCM_SHA256)
        put(w, TLS_AES_128_GCM_SHA256, 2);
    close_len(w, suites, 2);
    put(w, 0x0100, 2);                  /* legacy_compression_methods: null */
    size_t exts = open_len(w, 2);

    if (!host_is_address(t->host)) {
        put(w, EXT_SERVER_NAME, 2);
        size_t e = open_len(w, 2), list = open_len(w, 2);
        put(w, 0, 1);                   /* host_name */
        size_t name = open_len(w, 2);
        put_bytes(w, t->host, strlen(t->host));
        close_len(w, name, 2);
        close_len(w, list, 2);
        close_len(w, e, 2);
    }

    put(w, EXT_SUPPORTED_GROUPS, 2);
    size_t e = open_len(w, 2), list = open_len(w, 2);
    put(w, TLS_GROUP_X25519, 2);
    put(w, TLS_GROUP_SECP256R1, 2);
    close_len(w, list, 2);
    close_len(w, e, 2);

    put(w, EXT_SIGNATURE_ALGORITHMS, 2);
    e = open_len(w, 2);
    list = open_len(w, 2);
    for (size_t i = 0; i < sizeof sig_schemes / sizeof sig_schemes[0]; i++)
        put(w, sig_schemes[i], 2);
    close_len(w, list, 2);
    close_len(w, e, 2);

    put(w, EXT_SUPPORTED_VERSIONS, 2);
    put(w, 3, 2);
    put(w, 2, 1);
    put(w, 0x0304, 2);

    if (cookie_len) {
        put(w, EXT_COOKIE, 2);
        e = open_len(w, 2);
        size_t c = open_len(w, 2);
        put_bytes(w, cookie, cookie_len);
        close_len(w, c, 2);
        close_len(w, e, 2);
    }

    /* The key shares. x25519 and p256 tell which shares to send. */
    int x = group ? group == TLS_GROUP_X25519 : 1;
    int p = group ? group == TLS_GROUP_SECP256R1 : !t->opt.x25519_share_only;
    put(w, EXT_KEY_SHARE, 2);
    e = open_len(w, 2);
    list = open_len(w, 2);
    if (x) {
        uint8_t pub[32];
        x25519_base(pub, t->x25519_private);
        put(w, TLS_GROUP_X25519, 2);
        put(w, 32, 2);
        put_bytes(w, pub, 32);
    }
    if (p) {
        put(w, TLS_GROUP_SECP256R1, 2);
        put(w, 65, 2);
        put_bytes(w, t->p256_public, 65);
    }
    close_len(w, list, 2);
    close_len(w, e, 2);
    t->sent_x25519 = x;
    t->sent_p256 = p;

    close_len(w, exts, 2);
    close_len(w, msg, 3);
    if (w->bad)
        return fail(t, NO_ALERT, "the ClientHello does not fit");
    return 0;
}

static int send_handshake(struct tls *t, const uint8_t *msg, size_t len)
{
    hash_update(&t->transcript, msg, len);
    return send_records(t, CT_HANDSHAKE, msg, len);
}

/* Records the extension types of a ClientHello, which EncryptedExtensions
 * may answer. */
static void note_offered(struct tls *t, const uint8_t *msg, size_t len)
{
    struct reader r = { msg + 4, msg + len, 0 };
    get(&r, 2 + 32);
    sub(&r, 1);
    sub(&r, 2);
    sub(&r, 1);
    struct reader exts = sub(&r, 2);
    t->noffered = 0;
    while (!exts.bad && left(&exts) && t->noffered < TLS_MAX_OFFERED) {
        uint16_t type = (uint16_t)get(&exts, 2);
        sub(&exts, 2);
        t->offered[t->noffered++] = type;
    }
}

/* The fields of a ServerHello or a HelloRetryRequest. */
struct server_hello {
    int hrr;
    uint16_t suite;
    uint16_t group;
    const uint8_t *key;
    size_t key_len;
    const uint8_t *cookie;
    size_t cookie_len;
};

static int parse_server_hello(struct tls *t, const uint8_t *body, size_t len, struct server_hello *sh)
{
    memset(sh, 0, sizeof *sh);
    struct reader r = { body, body + len, 0 };
    uint16_t version = (uint16_t)get(&r, 2);
    const uint8_t *random = r.p;
    get(&r, 32);
    struct reader session = sub(&r, 1);
    sh->suite = (uint16_t)get(&r, 2);
    uint8_t compression = (uint8_t)get(&r, 1);
    struct reader exts = sub(&r, 2);
    if (r.bad || left(&r) || left(&session))
        return fail(t, r.bad ? ALERT_DECODE_ERROR : ALERT_ILLEGAL_PARAMETER, "malformed ServerHello");
    sh->hrr = memcmp(random, hrr_random, 32) == 0;
    if (version != 0x0303 || compression != 0)
        return fail(t, ALERT_ILLEGAL_PARAMETER, "malformed ServerHello");
    int offered = t->opt.suite ? sh->suite == t->opt.suite
                               : sh->suite == TLS_CHACHA20_POLY1305_SHA256 || sh->suite == TLS_AES_128_GCM_SHA256;
    if (!offered)
        return fail(t, ALERT_ILLEGAL_PARAMETER, "the server selected the suite 0x%04x", sh->suite);
    int have_version = 0, have_share = 0;
    uint32_t seen = 0;
    while (left(&exts)) {
        uint16_t type = (uint16_t)get(&exts, 2);
        struct reader data = sub(&exts, 2);
        if (exts.bad)
            break;
        uint32_t bit = type == EXT_SUPPORTED_VERSIONS ? 1 : type == EXT_KEY_SHARE ? 2 : type == EXT_COOKIE ? 4 : 0;
        if (!bit || (bit == 4 && !sh->hrr))
            return fail(t, ALERT_UNSUPPORTED_EXTENSION, "the ServerHello contains the extension %u", type);
        if (seen & bit)
            return fail(t, ALERT_ILLEGAL_PARAMETER, "the ServerHello repeats the extension %u", type);
        seen |= bit;
        if (type == EXT_SUPPORTED_VERSIONS) {
            if (get(&data, 2) != 0x0304 || left(&data))
                return fail(t, ALERT_ILLEGAL_PARAMETER, "the server selected another version than TLS 1.3");
            have_version = 1;
        } else if (type == EXT_KEY_SHARE) {
            sh->group = (uint16_t)get(&data, 2);
            if (!sh->hrr) {
                struct reader key = sub(&data, 2);
                sh->key = key.p;
                sh->key_len = left(&key);
            }
            if (data.bad || left(&data))
                return fail(t, ALERT_DECODE_ERROR, "malformed key_share extension");
            have_share = 1;
        } else {
            struct reader cookie = sub(&data, 2);
            sh->cookie = cookie.p;
            sh->cookie_len = left(&cookie);
            if (data.bad || left(&data) || !sh->cookie_len)
                return fail(t, ALERT_DECODE_ERROR, "malformed cookie extension");
        }
    }
    if (exts.bad)
        return fail(t, ALERT_DECODE_ERROR, "malformed ServerHello extensions");
    if (!have_version)
        return fail(t, ALERT_PROTOCOL_VERSION, "the server does not support TLS 1.3");
    if (!have_share && !sh->hrr)
        return fail(t, ALERT_MISSING_EXTENSION, "the ServerHello has no key share");
    return 0;
}

/* Processes a HelloRetryRequest: the transcript starts again with the
 * hash of the first ClientHello (RFC 8446, section 4.4.1), and the
 * second ClientHello carries the share and the cookie that the server
 * asked for. */
static int retry_hello(struct tls *t, const uint8_t *msg, size_t len, const struct server_hello *sh)
{
    if (t->hrr)
        return fail(t, ALERT_UNEXPECTED_MESSAGE, "a second HelloRetryRequest");
    if (t->opt.client_hello)
        return fail(t, ALERT_INTERNAL_ERROR, "a HelloRetryRequest for a fixed ClientHello");
    t->hrr = 1;
    uint16_t group = 0;
    if (sh->group) {
        int supported = sh->group == TLS_GROUP_X25519 || sh->group == TLS_GROUP_SECP256R1;
        int sent = sh->group == TLS_GROUP_X25519 ? t->sent_x25519 : t->sent_p256;
        if (!supported || sent)
            return fail(t, ALERT_ILLEGAL_PARAMETER, "the HelloRetryRequest asks for the group 0x%04x", sh->group);
        group = sh->group;
    } else if (!sh->cookie_len) {
        return fail(t, ALERT_ILLEGAL_PARAMETER, "the HelloRetryRequest changes nothing");
    }
    t->suite = sh->suite;
    uint8_t first[32], header[4] = { HS_MESSAGE_HASH, 0, 0, 32 };
    transcript_hash(t, first);
    hash_init(&t->transcript, HASH_SHA256);
    hash_update(&t->transcript, header, 4);
    hash_update(&t->transcript, first, 32);
    hash_update(&t->transcript, msg, len);
    uint8_t buf[1024];
    struct writer w = { buf, 0, sizeof buf, 0 };
    int r = build_client_hello(t, &w, group, sh->cookie, sh->cookie_len);
    if (r < 0)
        return r;
    return send_handshake(t, buf, w.len);
}

/* The shared secret of the key exchange. */
static int shared_secret(struct tls *t, const struct server_hello *sh, uint8_t shared[32])
{
    t->group = sh->group;
    if (sh->group == TLS_GROUP_X25519 && t->sent_x25519) {
        if (sh->key_len != 32 || x25519(shared, t->x25519_private, sh->key) < 0)
            return fail(t, ALERT_ILLEGAL_PARAMETER, "invalid X25519 share of the server");
        return 0;
    }
    if (sh->group == TLS_GROUP_SECP256R1 && t->sent_p256) {
        if (sh->key_len != 65 || ec_p256_shared(shared, t->p256_private, sh->key) < 0)
            return fail(t, ALERT_ILLEGAL_PARAMETER, "invalid secp256r1 share of the server");
        return 0;
    }
    return fail(t, ALERT_ILLEGAL_PARAMETER, "the server selected the group 0x%04x without a share", sh->group);
}

/* The handshake secrets after the ServerHello, and the keys of the
 * encrypted handshake messages. Returns the handshake secret. */
static void handshake_keys(struct tls *t, const uint8_t shared[32], uint8_t hs_secret[32])
{
    uint8_t zeros[32] = { 0 }, early[32], empty[32], derived[32], th[32];
    hkdf_extract(HASH_SHA256, NULL, 0, zeros, 32, early);
    hash(HASH_SHA256, "", 0, empty);
    derive_secret(early, "derived", empty, derived);
    hkdf_extract(HASH_SHA256, derived, 32, shared, 32, hs_secret);
    transcript_hash(t, th);
    derive_secret(hs_secret, "c hs traffic", th, t->c_hs);
    derive_secret(hs_secret, "s hs traffic", th, t->s_hs);
    tls_keys_set(&t->rd, t->suite, t->s_hs);
    tls_keys_set(&t->wr, t->suite, t->c_hs);
    crypto_wipe(early, sizeof early);
    crypto_wipe(derived, sizeof derived);
}

/* EncryptedExtensions may contain only extensions that the ClientHello
 * offered, and none of the extensions that belong to the ClientHello
 * and the ServerHello alone. */
static int process_encrypted_extensions(struct tls *t, const uint8_t *body, size_t len)
{
    struct reader r = { body, body + len, 0 };
    struct reader exts = sub(&r, 2);
    while (!exts.bad && left(&exts)) {
        uint16_t type = (uint16_t)get(&exts, 2);
        sub(&exts, 2);
        int offered = 0;
        for (int i = 0; i < t->noffered; i++)
            offered |= t->offered[i] == type;
        if (type == EXT_SIGNATURE_ALGORITHMS || type == EXT_SUPPORTED_VERSIONS || type == EXT_COOKIE ||
            type == EXT_KEY_SHARE)
            offered = 0;
        if (!exts.bad && !offered)
            return fail(t, ALERT_UNSUPPORTED_EXTENSION, "EncryptedExtensions contains the extension %u", type);
    }
    if (r.bad || exts.bad || left(&r))
        return fail(t, ALERT_DECODE_ERROR, "malformed EncryptedExtensions");
    return 0;
}

static int process_certificate_request(struct tls *t, const uint8_t *body, size_t len)
{
    struct reader r = { body, body + len, 0 };
    struct reader context = sub(&r, 1);
    struct reader exts = sub(&r, 2);
    while (!exts.bad && left(&exts)) {
        get(&exts, 2);
        sub(&exts, 2);
    }
    if (r.bad || exts.bad || left(&r))
        return fail(t, ALERT_DECODE_ERROR, "malformed CertificateRequest");
    t->cert_requested = 1;
    t->cert_context_len = (uint8_t)left(&context);
    memcpy(t->cert_context, context.p, left(&context));
    return 0;
}

static int alert_of(int x509_error)
{
    switch (x509_error) {
    case X509_ERR_EXPIRED:
    case X509_ERR_NOT_YET_VALID:
        return ALERT_CERTIFICATE_EXPIRED;
    case X509_ERR_ISSUER:
        return ALERT_UNKNOWN_CA;
    case X509_ERR_ALGORITHM:
        return ALERT_UNSUPPORTED_CERTIFICATE;
    case X509_ERR_HOST:
    case X509_ERR_USAGE:
        return ALERT_CERTIFICATE_UNKNOWN;
    default:
        return ALERT_BAD_CERTIFICATE;
    }
}

static int process_certificate(struct tls *t, const uint8_t *body, size_t len)
{
    free(t->cert_msg);
    t->cert_msg = malloc(len ? len : 1);
    if (!t->cert_msg)
        return fail(t, ALERT_INTERNAL_ERROR, "out of memory");
    memcpy(t->cert_msg, body, len);
    struct reader r = { t->cert_msg, t->cert_msg + len, 0 };
    struct reader context = sub(&r, 1);
    struct reader list = sub(&r, 3);
    if (r.bad || left(&r))
        return fail(t, ALERT_DECODE_ERROR, "malformed Certificate");
    if (left(&context))
        return fail(t, ALERT_ILLEGAL_PARAMETER, "the Certificate of the server has a context");
    t->ncerts = 0;
    for (int i = 0; left(&list); i++) {
        struct reader cert = sub(&list, 3);
        sub(&list, 2);                  /* the extensions of the entry */
        if (list.bad || !left(&cert))
            return fail(t, ALERT_DECODE_ERROR, "malformed Certificate");
        if (t->ncerts == TLS_MAX_CERTS)
            continue;
        if (x509_parse(&t->certs[t->ncerts], cert.p, left(&cert)) == X509_OK)
            t->ncerts++;
        else if (i == 0)
            return fail(t, ALERT_BAD_CERTIFICATE, "the certificate of the server does not parse");
    }
    if (!t->ncerts)
        return fail(t, ALERT_DECODE_ERROR, "the server sent no certificate");
    if (t->opt.skip_chain)
        return 0;
    static const struct x509_store empty;
    int64_t now = t->opt.now ? t->opt.now : (int64_t)time(NULL);
    int v = x509_verify_chain(t->certs, t->ncerts, t->store ? t->store : &empty, t->host, now);
    if (v != X509_OK)
        return fail(t, alert_of(v), "%s", x509_strerror(v));
    return 0;
}

/* CertificateVerify: the signature of the leaf key over the transcript
 * (RFC 8446, section 4.4.3). */
static int process_certificate_verify(struct tls *t, const uint8_t *body, size_t len, const uint8_t th[32])
{
    struct reader r = { body, body + len, 0 };
    uint16_t scheme = (uint16_t)get(&r, 2);
    struct reader sig = sub(&r, 2);
    if (r.bad || left(&r))
        return fail(t, ALERT_DECODE_ERROR, "malformed CertificateVerify");
    const struct x509_cert *leaf = &t->certs[0];
    enum hash_alg alg;
    int pss = 0, key_ok;
    switch (scheme) {
    case 0x0403:
        alg = HASH_SHA256;
        key_ok = leaf->key_type == X509_KEY_EC_P256;
        break;
    case 0x0503:
        alg = HASH_SHA384;
        key_ok = leaf->key_type == X509_KEY_EC_P384;
        break;
    case 0x0804:
    case 0x0805:
    case 0x0806:
        alg = scheme == 0x0804 ? HASH_SHA256 : scheme == 0x0805 ? HASH_SHA384 : HASH_SHA512;
        pss = 1;
        key_ok = leaf->key_type == X509_KEY_RSA;
        break;
    default:
        return fail(t, ALERT_ILLEGAL_PARAMETER, "the server signed with the scheme 0x%04x", scheme);
    }
    if (!key_ok)
        return fail(t, ALERT_ILLEGAL_PARAMETER, "the scheme 0x%04x does not fit the key of the server", scheme);
    static const char label[] = "TLS 1.3, server CertificateVerify";
    uint8_t content[64 + sizeof label + 32], digest[HASH_MAX_SIZE];
    memset(content, 0x20, 64);
    memcpy(content + 64, label, sizeof label);      /* with the terminating zero byte */
    memcpy(content + 64 + sizeof label, th, 32);
    hash(alg, content, sizeof content, digest);
    if (x509_verify_digest(leaf, alg, pss, digest, sig.p, left(&sig)) != X509_OK)
        return fail(t, ALERT_DECRYPT_ERROR, "the handshake signature of the server is invalid");
    return 0;
}

/* The verify_data of a Finished message for the traffic secret. */
static void finished_mac(const uint8_t secret[32], const uint8_t th[32], uint8_t out[32])
{
    uint8_t key[32];
    expand_label(secret, "finished", NULL, 0, key, 32);
    hmac(HASH_SHA256, key, 32, th, 32, out);
    crypto_wipe(key, sizeof key);
}

static int handshake(struct tls *t)
{
    uint8_t buf[1024];
    hash_init(&t->transcript, HASH_SHA256);
    int r;
    if (t->opt.client_hello) {
        memcpy(t->x25519_private, t->opt.x25519_private, 32);
        t->sent_x25519 = 1;
        note_offered(t, t->opt.client_hello, t->opt.client_hello_len);
        r = send_handshake(t, t->opt.client_hello, t->opt.client_hello_len);
    } else {
        if ((r = random_bytes(t, t->random, 32)) < 0 || (r = random_bytes(t, t->x25519_private, 32)) < 0)
            return r;
        /* A P-256 key must lie below the order of the group. */
        do {
            if ((r = random_bytes(t, t->p256_private, 32)) < 0)
                return r;
        } while (ec_p256_public(t->p256_public, t->p256_private) < 0);
        struct writer w = { buf, 0, sizeof buf, 0 };
        if ((r = build_client_hello(t, &w, 0, NULL, 0)) < 0)
            return r;
        note_offered(t, buf, w.len);
        r = send_handshake(t, buf, w.len);
    }
    if (r < 0)
        return r;

    /* ServerHello, after at most one HelloRetryRequest. */
    const uint8_t *msg;
    size_t len;
    struct server_hello sh;
    for (;;) {
        if ((r = next_message(t, &msg, &len)) < 0)
            return r;
        if (msg[0] != HS_SERVER_HELLO)
            return fail(t, ALERT_UNEXPECTED_MESSAGE, "expected ServerHello, received message %d", msg[0]);
        if ((r = parse_server_hello(t, msg + 4, len - 4, &sh)) < 0)
            return r;
        if (!sh.hrr)
            break;
        if ((r = retry_hello(t, msg, len, &sh)) < 0)
            return r;
    }
    if (t->hrr && sh.suite != t->suite)
        return fail(t, ALERT_ILLEGAL_PARAMETER, "the ServerHello changes the suite of the HelloRetryRequest");
    t->suite = sh.suite;
    uint8_t shared[32], hs_secret[32];
    if ((r = shared_secret(t, &sh, shared)) < 0)
        return r;
    hash_update(&t->transcript, msg, len);
    if ((r = check_key_change(t)) < 0)
        return r;
    handshake_keys(t, shared, hs_secret);
    crypto_wipe(shared, sizeof shared);

    /* The encrypted messages of the server. */
    if ((r = next_message(t, &msg, &len)) < 0)
        return r;
    if (msg[0] != HS_ENCRYPTED_EXTENSIONS)
        return fail(t, ALERT_UNEXPECTED_MESSAGE, "expected EncryptedExtensions, received message %d", msg[0]);
    if ((r = process_encrypted_extensions(t, msg + 4, len - 4)) < 0)
        return r;
    hash_update(&t->transcript, msg, len);
    if ((r = next_message(t, &msg, &len)) < 0)
        return r;
    if (msg[0] == HS_CERTIFICATE_REQUEST) {
        if ((r = process_certificate_request(t, msg + 4, len - 4)) < 0)
            return r;
        hash_update(&t->transcript, msg, len);
        if ((r = next_message(t, &msg, &len)) < 0)
            return r;
    }
    if (msg[0] != HS_CERTIFICATE)
        return fail(t, ALERT_UNEXPECTED_MESSAGE, "expected Certificate, received message %d", msg[0]);
    if ((r = process_certificate(t, msg + 4, len - 4)) < 0)
        return r;
    hash_update(&t->transcript, msg, len);
    if ((r = next_message(t, &msg, &len)) < 0)
        return r;
    if (msg[0] != HS_CERTIFICATE_VERIFY)
        return fail(t, ALERT_UNEXPECTED_MESSAGE, "expected CertificateVerify, received message %d", msg[0]);
    uint8_t th[32], mac[32];
    transcript_hash(t, th);
    if ((r = process_certificate_verify(t, msg + 4, len - 4, th)) < 0)
        return r;
    hash_update(&t->transcript, msg, len);
    if ((r = next_message(t, &msg, &len)) < 0)
        return r;
    if (msg[0] != HS_FINISHED)
        return fail(t, ALERT_UNEXPECTED_MESSAGE, "expected Finished, received message %d", msg[0]);
    transcript_hash(t, th);
    finished_mac(t->s_hs, th, mac);
    if (len != 4 + 32 || crypto_memcmp(mac, msg + 4, 32) != 0)
        return fail(t, ALERT_DECRYPT_ERROR, "the Finished message of the server is invalid");
    hash_update(&t->transcript, msg, len);
    if ((r = check_key_change(t)) < 0)
        return r;

    /* The application secrets use the transcript up to the Finished
     * message of the server. */
    uint8_t zeros[32] = { 0 }, empty[32], derived[32], master[32], c_ap[32], s_ap[32];
    hash(HASH_SHA256, "", 0, empty);
    derive_secret(hs_secret, "derived", empty, derived);
    hkdf_extract(HASH_SHA256, derived, 32, zeros, 32, master);
    transcript_hash(t, th);
    derive_secret(master, "c ap traffic", th, c_ap);
    derive_secret(master, "s ap traffic", th, s_ap);
    tls_keys_set(&t->rd, t->suite, s_ap);

    /* The answer of the client: an empty Certificate when the server
     * asked for one, and Finished. */
    struct writer w = { buf, 0, sizeof buf, 0 };
    if (t->cert_requested) {
        put(&w, HS_CERTIFICATE, 1);
        size_t m = open_len(&w, 3);
        put(&w, t->cert_context_len, 1);
        put_bytes(&w, t->cert_context, t->cert_context_len);
        put(&w, 0, 3);
        close_len(&w, m, 3);
        hash_update(&t->transcript, buf, w.len);
    }
    size_t fin = w.len;
    transcript_hash(t, th);
    finished_mac(t->c_hs, th, mac);
    put(&w, HS_FINISHED, 1);
    put(&w, 32, 3);
    put_bytes(&w, mac, 32);
    hash_update(&t->transcript, buf + fin, w.len - fin);
    r = send_records(t, CT_HANDSHAKE, buf, w.len);
    tls_keys_set(&t->wr, t->suite, c_ap);
    crypto_wipe(hs_secret, sizeof hs_secret);
    crypto_wipe(master, sizeof master);
    crypto_wipe(c_ap, sizeof c_ap);
    crypto_wipe(s_ap, sizeof s_ap);
    if (r < 0)
        return r;
    t->state = STATE_OPEN;
    return 0;
}

int tls_connect_options(struct tls **out, int fd, const char *host, const struct x509_store *store, int timeout,
                        const struct tls_options *opt, char *err, size_t err_len)
{
    *out = NULL;
    struct tls *t = calloc(1, sizeof *t);
    if (!t) {
        snprintf(err, err_len, "out of memory");
        return -ENOMEM;
    }
    t->fd = fd;
    t->timeout = timeout;
    t->store = store;
    t->state = STATE_HANDSHAKE;
    if (opt)
        t->opt = *opt;
    if (strlen(host) >= sizeof t->host || !*host) {
        snprintf(err, err_len, "invalid host name");
        free(t);
        return -EINVAL;
    }
    strcpy(t->host, host);
    int r = handshake(t);
    if (r < 0) {
        snprintf(err, err_len, "%s", t->error);
        t->state = STATE_FAILED;
        tls_close(t);
        return r;
    }
    *out = t;
    return 0;
}

int tls_connect(struct tls **out, int fd, const char *host, const struct x509_store *store, int timeout, char *err,
                size_t err_len)
{
    return tls_connect_options(out, fd, host, store, timeout, NULL, err, err_len);
}

/* ---- After the handshake ---- */

/* Processes the complete handshake messages after the handshake. */
static int post_handshake(struct tls *t)
{
    const uint8_t *msg;
    size_t len;
    int r;
    while ((r = buffered_message(t, &msg, &len)) > 0) {
        if (msg[0] == HS_NEW_SESSION_TICKET)
            continue;                   /* the client does not resume sessions */
        if (msg[0] != HS_KEY_UPDATE)
            return fail(t, ALERT_UNEXPECTED_MESSAGE, "unexpected handshake message %d", msg[0]);
        if (len != 5 || msg[4] > 1)
            return fail(t, len != 5 ? ALERT_DECODE_ERROR : ALERT_ILLEGAL_PARAMETER, "malformed KeyUpdate");
        int requested = msg[4];
        if ((r = check_key_change(t)) < 0)
            return r;
        tls_keys_update(&t->rd);
        if (requested) {
            static const uint8_t update[5] = { HS_KEY_UPDATE, 0, 0, 1, 0 };
            if ((r = send_records(t, CT_HANDSHAKE, update, sizeof update)) < 0)
                return r;
            tls_keys_update(&t->wr);
        }
    }
    return r;
}

ssize_t tls_read(struct tls *t, void *buf, size_t len)
{
    for (;;) {
        if (t->state == STATE_FAILED)
            return -EPROTO;
        if (t->state == STATE_CLOSED)
            return 0;
        if (t->rec_type == CT_APPLICATION_DATA && t->rec_pos < t->rec_len) {
            size_t n = t->rec_len - t->rec_pos < len ? t->rec_len - t->rec_pos : len;
            memcpy(buf, t->rec + t->rec_pos, n);
            t->rec_pos += n;
            return (ssize_t)n;
        }
        int r = read_record(t);
        if (r < 0)
            return r;
        if (r == 1) {
            t->state = STATE_CLOSED;
            return 0;
        }
        if (t->rec_type == CT_HANDSHAKE) {
            if ((r = append_handshake(t)) < 0 || (r = post_handshake(t)) < 0)
                return r;
            t->rec_len = 0;
        }
    }
}

ssize_t tls_write(struct tls *t, const void *buf, size_t len)
{
    if (t->state != STATE_OPEN && t->state != STATE_CLOSED)
        return -EPROTO;
    int r = send_records(t, CT_APPLICATION_DATA, buf, len);
    return r < 0 ? r : (ssize_t)len;
}

const char *tls_error(const struct tls *t)
{
    return t->error;
}

void tls_close(struct tls *t)
{
    if (!t)
        return;
    if (t->state == STATE_OPEN || t->state == STATE_CLOSED)
        send_alert(t, 1, ALERT_CLOSE_NOTIFY);
    free(t->cert_msg);
    free(t->hs);
    crypto_wipe(t, sizeof *t);
    free(t);
}
