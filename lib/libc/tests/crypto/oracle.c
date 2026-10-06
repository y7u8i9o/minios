/* The crypto oracle of make check-crypto: reads one operation per line,
 * "NAME ARG ...", with arguments in hexadecimal ("-" for an empty
 * string), and prints the result in hexadecimal, or "ok" and "bad" for a
 * verification. check.py compares the results with the cryptography
 * package of Python. The certificate operations take file paths and
 * print "ok" or the text of x509_strerror. The TLS operations replay the
 * trace of RFC 8448 and connect to the servers of check.py. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <minios/base64.h>
#include <minios/crypto.h>
#include <minios/x509.h>
#include "../../src/net/tls_internal.h"

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

/* "verify HOST NOW CHAIN STORE": verifies the PEM chain against the PEM
 * store at the time NOW. */
static void op_verify(const char *host, const char *now, const char *chain_path, const char *store_path)
{
    struct x509_store store = { 0 };
    if (x509_store_load(&store, store_path, NULL) < 0) {
        printf("no store\n");
        return;
    }
    static char text[1 << 16];
    FILE *f = fopen(chain_path, "r");
    size_t len = f ? fread(text, 1, sizeof text, f) : 0;
    if (f)
        fclose(f);
    uint8_t **ders;
    size_t *lens;
    int n = x509_pem_decode(text, len, &ders, &lens, NULL);
    struct x509_cert chain[16];
    int r = n > 0 && n <= 16 ? X509_OK : X509_ERR_PARSE;
    for (int i = 0; r == X509_OK && i < n; i++)
        r = x509_parse(&chain[i], ders[i], lens[i]);
    if (r == X509_OK)
        r = x509_verify_chain(chain, n, &store, host, strtoll(now, NULL, 10));
    printf("%s\n", r == X509_OK ? "ok" : x509_strerror(r));
    if (n >= 0)
        x509_free_ders(ders, lens, n);
    x509_store_free(&store);
}

/* The records and keys of rfc8448.txt. */
struct trace {
    char name[16][32];
    uint8_t *data[16];
    size_t len[16];
    int count;
};

static int load_trace(const char *path, struct trace *tr)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    static char line[8192];
    tr->count = 0;
    while (fgets(line, sizeof line, f) && tr->count < 16) {
        char *name = strtok(line, " \n"), *hex = strtok(NULL, " \n");
        if (!name || name[0] == '#' || !hex)
            continue;
        snprintf(tr->name[tr->count], sizeof tr->name[0], "%s", name);
        tr->len[tr->count] = strlen(hex) / 2;
        tr->data[tr->count] = malloc(tr->len[tr->count]);
        unhex(hex, tr->data[tr->count]);
        tr->count++;
    }
    fclose(f);
    return 0;
}

static const uint8_t *trace_get(const struct trace *tr, const char *name, size_t *len)
{
    for (int i = 0; i < tr->count; i++)
        if (!strcmp(tr->name[i], name)) {
            *len = tr->len[i];
            return tr->data[i];
        }
    *len = 0;
    return NULL;
}

static int read_exact(int fd, uint8_t *buf, size_t len)
{
    while (len) {
        ssize_t n = read(fd, buf, len);
        if (n <= 0)
            return -1;
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

/* Reads one record that the client wrote. */
static size_t read_record(int fd, uint8_t *buf)
{
    if (read_exact(fd, buf, 5) < 0)
        return 0;
    size_t len = (size_t)buf[3] << 8 | buf[4];
    return read_exact(fd, buf + 5, len) < 0 ? 0 : 5 + len;
}

static int send_trace(int fd, const struct trace *tr, const char *name)
{
    size_t len;
    const uint8_t *d = trace_get(tr, name, &len);
    return d && write(fd, d, len) == (ssize_t)len ? 0 : -1;
}

/* Reads the next record of the client and compares it with the trace. */
static int expect_trace(int fd, const struct trace *tr, const char *name)
{
    static uint8_t buf[TLS_MAX_RECORD];
    size_t len, got = read_record(fd, buf);
    const uint8_t *d = trace_get(tr, name, &len);
    return d && got == len && memcmp(buf, d, len) == 0 ? 0 : -1;
}

/* "rfc8448 PATH [keyupdate]": the client receives the records of the
 * server of RFC 8448, section 3, and must send the records of the
 * client. With keyupdate, the server then sends a KeyUpdate that
 * requests an update, and the data before and after it must decrypt. */
static const char *op_rfc8448(const char *path, int key_update)
{
    static char err[300];
    struct trace tr;
    if (load_trace(path, &tr) < 0)
        return "no trace";
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
        return "socketpair";
    size_t ch_len, priv_len;
    const uint8_t *ch = trace_get(&tr, "client_hello", &ch_len), *priv = trace_get(&tr, "client_private", &priv_len);
    if (send_trace(sv[1], &tr, "server_hello_record") < 0 || send_trace(sv[1], &tr, "server_handshake_record") < 0)
        return "writing the server records";
    struct tls_options o = { .client_hello = ch, .client_hello_len = ch_len, .x25519_private = priv, .skip_chain = 1 };
    struct tls *t;
    char reason[256];
    if (tls_connect_options(&t, sv[0], "server", NULL, 5, &o, reason, sizeof reason) < 0) {
        snprintf(err, sizeof err, "handshake: %s", reason);
        return err;
    }
    if (expect_trace(sv[1], &tr, "client_hello_record") < 0)
        return "the ClientHello record differs";
    if (expect_trace(sv[1], &tr, "client_finished_record") < 0)
        return "the Finished record of the client differs";
    if (send_trace(sv[1], &tr, "server_ticket_record") < 0 || send_trace(sv[1], &tr, "server_app_record") < 0)
        return "writing the server records";
    uint8_t data[50], got[64];
    for (int i = 0; i < 50; i++)
        data[i] = (uint8_t)i;
    if (tls_read(t, got, sizeof got) != 50 || memcmp(got, data, 50) != 0)
        return "the application data of the server differs";
    if (tls_write(t, data, 50) != 50 || expect_trace(sv[1], &tr, "client_app_record") < 0)
        return "the application data record of the client differs";
    if (!key_update) {
        if (send_trace(sv[1], &tr, "server_alert_record") < 0 || tls_read(t, got, sizeof got) != 0)
            return "close_notify of the server not seen";
        tls_close(t);
        if (expect_trace(sv[1], &tr, "client_alert_record") < 0)
            return "the close_notify record of the client differs";
        close(sv[0]);
        close(sv[1]);
        return "ok";
    }
    static uint8_t buf[TLS_MAX_RECORD], plain[TLS_MAX_RECORD];
    struct tls_keys server = t->rd, client = t->wr;
    static const uint8_t update[5] = { 24, 0, 0, 1, 1 }, after[] = "after the update", reply[] = "reply";
    size_t len = tls_record_seal(&server, 22, update, sizeof update, buf);
    if (write(sv[1], buf, len) != (ssize_t)len)
        return "writing the KeyUpdate";
    tls_keys_update(&server);
    len = tls_record_seal(&server, 23, after, sizeof after, buf);
    if (write(sv[1], buf, len) != (ssize_t)len)
        return "writing the data after the KeyUpdate";
    if (tls_read(t, got, sizeof got) != (ssize_t)sizeof after || memcmp(got, after, sizeof after) != 0)
        return "the data after the KeyUpdate of the server differs";
    size_t plen;
    uint8_t type;
    len = read_record(sv[1], buf);
    if (!len || tls_record_open(&client, buf, len, plain, &plen, &type) < 0 || type != 22 || plen != 5 ||
        memcmp(plain, "\x18\x00\x00\x01\x00", 5) != 0)
        return "the client did not answer with KeyUpdate";
    tls_keys_update(&client);
    if (tls_write(t, reply, sizeof reply) != (ssize_t)sizeof reply)
        return "write after the KeyUpdate";
    len = read_record(sv[1], buf);
    if (!len || tls_record_open(&client, buf, len, plain, &plen, &type) < 0 || type != 23 || plen != sizeof reply ||
        memcmp(plain, reply, plen) != 0)
        return "the data of the client after the KeyUpdate differs";
    tls_close(t);
    len = read_record(sv[1], buf);
    if (!len || tls_record_open(&client, buf, len, plain, &plen, &type) < 0 || type != 21 || plen != 2 ||
        plain[0] != 1 || plain[1] != 0)
        return "close_notify of the client after the KeyUpdate differs";
    close(sv[0]);
    close(sv[1]);
    return "ok";
}

/* "tls PORT HOST STORE SUITE X25519ONLY BYTES": connects to the echo
 * server of check.py on 127.0.0.1, sends BYTES bytes and reads them
 * back. Prints "ok SUITE GROUP HRR" or "fail: REASON". */
static void op_tls(char **w)
{
    struct x509_store store = { 0 };
    if (strcmp(w[3], "-") && x509_store_load(&store, w[3], NULL) < 0) {
        printf("fail: no store\n");
        return;
    }
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)atoi(w[1])) };
    a.sin_addr.s_addr = htonl(0x7f000001);
    if (connect(s, (struct sockaddr *)&a, sizeof a) < 0) {
        printf("fail: connect\n");
        close(s);
        x509_store_free(&store);
        return;
    }
    struct tls_options o = { .suite = (uint16_t)strtol(w[4], NULL, 16), .x25519_share_only = atoi(w[5]) };
    struct tls *t;
    char reason[256];
    if (tls_connect_options(&t, s, w[2], &store, 10, &o, reason, sizeof reason) < 0) {
        printf("fail: %s\n", reason);
        close(s);
        x509_store_free(&store);
        return;
    }
    size_t n = (size_t)atol(w[6]);
    uint8_t *out = malloc(n + 1), *in = malloc(n + 1);
    for (size_t i = 0; i < n; i++)
        out[i] = (uint8_t)(i * 7 + i / 251);
    const char *bad = NULL;
    if (tls_write(t, out, n) != (ssize_t)n)
        bad = tls_error(t);
    for (size_t got = 0; !bad && got < n;) {
        ssize_t r = tls_read(t, in + got, n - got);
        if (r <= 0)
            bad = r ? tls_error(t) : "early close";
        else
            got += (size_t)r;
    }
    if (!bad && memcmp(in, out, n) != 0)
        bad = "the echo differs";
    if (bad)
        printf("fail: %s\n", bad);
    else
        printf("ok %04x %04x %d\n", t->suite, t->group, t->hrr);
    tls_close(t);
    close(s);
    free(out);
    free(in);
    x509_store_free(&store);
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
        } else if (!strcmp(op, "b64")) {
            const char *t = strcmp(w[1], "-") ? w[1] : "";
            long len = base64_decode(t, strlen(t), out);
            if (len < 0)
                printf("bad\n");
            else
                print_hex(out, (size_t)len);
        } else if (!strcmp(op, "store")) {
            struct x509_store store = { 0 };
            int skipped = 0;
            int added = x509_store_load(&store, w[1], &skipped);
            printf("%d %d\n", added, skipped);
            x509_store_free(&store);
        } else if (!strcmp(op, "verify")) {
            op_verify(w[1], w[2], w[3], w[4]);
        } else if (!strcmp(op, "rfc8448")) {
            printf("%s\n", op_rfc8448(w[1], n > 2));
        } else if (!strcmp(op, "tls")) {
            op_tls(w);
        } else {
            printf("unknown\n");
        }
        (void)e;
        fflush(stdout);
    }
    return 0;
}
