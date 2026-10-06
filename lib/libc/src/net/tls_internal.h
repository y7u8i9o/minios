#pragma once
/* The internals of the TLS client (tls.c). The tests of
 * lib/libc/tests/crypto/ use the options and the record functions to
 * replay the trace of RFC 8448 and to inject records of a server. */
#include <minios/tls.h>
#include <minios/crypto.h>
#include <stdint.h>

#define TLS_MAX_PLAIN 16384                     /* the largest plaintext of a record */
#define TLS_MAX_RECORD (5 + TLS_MAX_PLAIN + 256) /* the largest record with its header */
#define TLS_MAX_HANDSHAKE (1 << 17)             /* the largest handshake message */
#define TLS_MAX_CERTS 16                        /* certificates of the server that are parsed */
#define TLS_MAX_OFFERED 32                      /* extension types of the ClientHello */

enum {
    TLS_AES_128_GCM_SHA256 = 0x1301,
    TLS_CHACHA20_POLY1305_SHA256 = 0x1303,
};

enum {
    TLS_GROUP_SECP256R1 = 0x0017,
    TLS_GROUP_X25519 = 0x001d,
};

/* The keys of one direction of the record layer. */
struct tls_keys {
    uint16_t suite;
    uint8_t secret[32];                 /* the traffic secret, the base of a key update */
    uint8_t key[32];
    uint8_t iv[12];
    uint64_t seq;
    int active;
};

/* Options of the tests. A zeroed structure gives the behaviour of
 * tls_connect. */
struct tls_options {
    const uint8_t *client_hello;        /* sent instead of the ClientHello of the client */
    size_t client_hello_len;
    const uint8_t *x25519_private;      /* the X25519 key of client_hello */
    int skip_chain;                     /* no chain verification; CertificateVerify is still checked */
    int64_t now;                        /* the time of the chain verification, 0 for time() */
    uint16_t suite;                     /* offer only this suite, 0 for both */
    int x25519_share_only;              /* no secp256r1 share in the first ClientHello */
};

struct tls {
    int fd;
    int timeout;
    int state;
    char error[256];
    char host[256];
    const struct x509_store *store;
    struct tls_options opt;
    uint16_t suite;                     /* the suite of the ServerHello */
    uint16_t group;                     /* the group of the key exchange */
    int hrr;                            /* 1 after a HelloRetryRequest */
    struct tls_keys rd, wr;
    struct hash_ctx transcript;
    uint8_t random[32];
    uint8_t x25519_private[32];
    uint8_t p256_private[32];
    uint8_t p256_public[65];
    int sent_x25519, sent_p256;         /* the key shares of the last ClientHello */
    uint16_t offered[TLS_MAX_OFFERED];  /* the extension types of the ClientHello */
    int noffered;
    uint8_t c_hs[32], s_hs[32];         /* handshake traffic secrets */
    int cert_requested;
    uint8_t cert_context[255];
    uint8_t cert_context_len;
    uint8_t *cert_msg;                  /* a copy of the Certificate message; certs point into it */
    struct x509_cert certs[TLS_MAX_CERTS];
    int ncerts;
    uint8_t *hs;                        /* handshake bytes that are not yet processed */
    size_t hs_len, hs_pos, hs_cap;
    uint8_t in[2 * TLS_MAX_RECORD];     /* received bytes, at most one record ahead */
    size_t in_len;
    uint8_t rec[TLS_MAX_RECORD];        /* the content of the current record */
    size_t rec_len, rec_pos;
    uint8_t rec_type;
    uint8_t out[TLS_MAX_RECORD];
};

int tls_connect_options(struct tls **out, int fd, const char *host, const struct x509_store *store, int timeout,
                        const struct tls_options *opt, char *err, size_t err_len);

/* Derives the key and the IV of a traffic secret and resets the
 * sequence number. */
void tls_keys_set(struct tls_keys *k, uint16_t suite, const uint8_t secret[32]);
/* Replaces the traffic secret with its successor (RFC 8446, section
 * 7.2). */
void tls_keys_update(struct tls_keys *k);
/* Encrypts len bytes of the content type into a record at out and
 * returns the length of the record. data may lie at out + 5. */
size_t tls_record_seal(struct tls_keys *k, uint8_t type, const uint8_t *data, size_t len, uint8_t *out);
/* Decrypts a whole record. Returns 0 with the content in out, or -1. */
int tls_record_open(struct tls_keys *k, const uint8_t *rec, size_t len, uint8_t *out, size_t *out_len, uint8_t *type);
