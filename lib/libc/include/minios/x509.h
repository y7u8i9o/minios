#pragma once
/* X.509 certificates for the TLS client (docs/plan/tls.md, T3): DER
 * parsing, a trust store from a PEM file, and the verification of a
 * server chain with its validity times, CA constraints, key usages and
 * host name. The functions use the primitives of minios/crypto.h. */
#include <stddef.h>
#include <stdint.h>
#include <minios/crypto.h>

enum x509_key {
    X509_KEY_EC_P256,
    X509_KEY_EC_P384,
    X509_KEY_RSA,
};

enum x509_sig {
    X509_SIG_ECDSA_SHA256,
    X509_SIG_ECDSA_SHA384,
    X509_SIG_ECDSA_SHA512,
    X509_SIG_RSA_SHA256,
    X509_SIG_RSA_SHA384,
    X509_SIG_RSA_SHA512,
};

/* Bits of key_usage, in the order of RFC 5280, section 4.2.1.3. */
#define X509_KU_DIGITAL_SIGNATURE 0x001
#define X509_KU_KEY_CERT_SIGN     0x020

/* The verification results. Every error is negative. */
enum x509_error {
    X509_OK = 0,
    X509_ERR_PARSE = -1,        /* the DER encoding is invalid */
    X509_ERR_ALGORITHM = -2,    /* a key or signature algorithm is not supported */
    X509_ERR_EXPIRED = -3,
    X509_ERR_NOT_YET_VALID = -4,
    X509_ERR_HOST = -5,         /* the leaf certificate is not valid for the host */
    X509_ERR_ISSUER = -6,       /* no path leads to a certificate of the trust store */
    X509_ERR_SIGNATURE = -7,
    X509_ERR_CA = -8,           /* an issuer is no CA, or a path length is exceeded */
    X509_ERR_USAGE = -9,        /* a key usage or extended key usage forbids the use */
    X509_ERR_CRITICAL = -10,    /* a critical extension is unknown */
};

/* A parsed certificate. The pointers lead into the DER buffer that
 * x509_parse received. The buffer must outlive the structure. */
struct x509_cert {
    const uint8_t *der;
    size_t der_len;
    const uint8_t *tbs;             /* the signed part, with tag and length */
    size_t tbs_len;
    const uint8_t *issuer;          /* the DER encoding of the issuer Name */
    size_t issuer_len;
    const uint8_t *subject;
    size_t subject_len;
    int64_t not_before;             /* seconds since 1970 in UTC */
    int64_t not_after;
    int key_supported;              /* 0 when the key algorithm is not supported */
    enum x509_key key_type;
    const uint8_t *key;             /* EC: the uncompressed point. RSA: the modulus */
    size_t key_len;
    const uint8_t *rsa_e;
    size_t rsa_e_len;
    int sig_supported;              /* 0 when the signature algorithm is not supported */
    enum x509_sig sig_alg;
    const uint8_t *sig;             /* the content of the signature bit string */
    size_t sig_len;
    int is_ca;                      /* basicConstraints cA */
    int path_len;                   /* basicConstraints pathLenConstraint, -1 without */
    int key_usage;                  /* X509_KU_* bits, -1 without the extension */
    int has_eku;                    /* 1 when extKeyUsage is present */
    int eku_server;                 /* extKeyUsage contains serverAuth or anyExtendedKeyUsage */
    int unknown_critical;           /* a critical extension is unknown */
    const uint8_t *san;             /* the content of the subjectAltName sequence */
    size_t san_len;
};

/* Parses one DER certificate. Returns 0 or X509_ERR_PARSE. */
int x509_parse(struct x509_cert *c, const uint8_t *der, size_t len);

/* The trust anchors. Each certificate owns its DER buffer. */
struct x509_store {
    struct x509_cert *certs;
    int count;
    int capacity;
};

/* Adds the CERTIFICATE blocks of PEM text to the store. Returns the
 * number of added certificates, or -1 when memory runs out. Blocks that
 * do not decode or parse are skipped and counted in *skipped, when
 * skipped is not NULL. */
int x509_store_add_pem(struct x509_store *s, const char *text, size_t len, int *skipped);
/* Reads a PEM file such as /etc/ssl/cert.pem into the store. Returns the
 * number of added certificates, or a negative errno value. */
int x509_store_load(struct x509_store *s, const char *path, int *skipped);
void x509_store_free(struct x509_store *s);

/* Decodes the CERTIFICATE blocks of PEM text into DER buffers. On
 * success *ders receives an array of count buffers from malloc, and
 * *lens their lengths. Returns count or -1. x509_free_ders frees the
 * array and the buffers. */
int x509_pem_decode(const char *text, size_t len, uint8_t ***ders, size_t **lens, int *skipped);
void x509_free_ders(uint8_t **ders, size_t *lens, int count);

/* Verifies a server chain at the time now (seconds since 1970).
 * chain[0] is the leaf certificate. The other certificates may come in
 * any order and may include certificates that the path does not use.
 * The path ends at a certificate of the store, which counts as a name
 * and a key without further checks. Returns X509_OK or the error of the
 * path that came closest to an anchor. */
int x509_verify_chain(const struct x509_cert *chain, int count, const struct x509_store *store, const char *host,
                      int64_t now);

/* Checks that the leaf certificate is valid for host: a DNS name of the
 * subjectAltName, with a wildcard only as the whole leftmost label, or
 * an IPv4 address for an address literal. Returns X509_OK or
 * X509_ERR_HOST. */
int x509_check_host(const struct x509_cert *c, const char *host);

/* Verifies that the key of issuer signed the certificate c. Returns
 * X509_OK, X509_ERR_ALGORITHM or X509_ERR_SIGNATURE. */
int x509_check_signature(const struct x509_cert *c, const struct x509_cert *issuer);

/* Verifies the signature sig over digest with the key of c. An ECDSA
 * signature is the DER encoding of Ecdsa-Sig-Value. For an RSA key, pss
 * selects RSA-PSS with a salt of the hash size, and 0 selects PKCS#1
 * v1.5. Returns X509_OK, X509_ERR_ALGORITHM or X509_ERR_SIGNATURE. */
int x509_verify_digest(const struct x509_cert *c, enum hash_alg alg, int pss, const uint8_t *digest,
                       const uint8_t *sig, size_t sig_len);

/* The text of a result of x509_verify_chain, such as "certificate
 * expired". */
const char *x509_strerror(int err);
