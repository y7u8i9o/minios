/* N11: the resolver against a scripted DNS server on loopback. The queried
 * name selects the server's behaviour. No NIC is needed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/ipc.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>

static int failures;
#define CHECK(c, text) do { if (!(c)) { printf("netdnstest: FAIL %s (errno %d)\n", text, errno); failures++; } } while (0)

static size_t build_answer(const unsigned char *q, size_t qlen, unsigned char *a, const char *name,
                           int tcp)
{
    /* Copy the header and question, then append records by behaviour. */
    memcpy(a, q, qlen);
    a[2] = 0x81; /* response, recursion desired */
    a[3] = 0x80;
    a[6] = a[7] = 0;
    size_t n = qlen;
    unsigned answers = 0;
    if (strcmp(name, "nx.test") == 0) {
        a[3] = 0x83; /* NXDOMAIN */
    } else if (strcmp(name, "fail.test") == 0) {
        a[3] = 0x82; /* SERVFAIL */
    } else if (strcmp(name, "loop.test") == 0) {
        /* Owner name pointing at itself. */
        a[n++] = 0xc0; a[n++] = (unsigned char)n - 1;
        memcpy(a + n, "\0\1\0\1\0\0\0\x3c\0\4\1\2\3\4", 14); n += 14;
        answers = 1;
    } else if (strcmp(name, "big.test") == 0 && !tcp) {
        a[2] |= 0x02; /* truncated: the client must retry over TCP */
    } else if (strcmp(name, "cname.test") == 0) {
        a[n++] = 0xc0; a[n++] = 12;
        memcpy(a + n, "\0\5\0\1\0\0\0\x3c\0\x0b\4host\4test\0", 21); n += 21;
        /* Then the A record for the target so one round trip resolves it. */
        size_t target = n - 11;
        a[n++] = 0xc0; a[n++] = (unsigned char)target;
        memcpy(a + n, "\0\1\0\1\0\0\0\x3c\0\4\x0a\0\0\x07", 14); n += 14;
        answers = 2;
    } else if (strcmp(name, "chain.test") == 0) {
        /* A CNAME pointing at another CNAME name, without an address. */
        a[n++] = 0xc0; a[n++] = 12;
        memcpy(a + n, "\0\5\0\1\0\0\0\x3c\0\x0c\5chain\4test\0", 22); n += 22;
        answers = 1;
    } else {
        a[n++] = 0xc0; a[n++] = 12;
        unsigned char rr[] = {0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 10, 0, 0, 5};
        if (strcmp(name, "big.test") == 0)
            rr[13] = 9;
        memcpy(a + n, rr, 14); n += 14;
        answers = 1;
    }
    a[6] = answers >> 8;
    a[7] = answers & 255;
    return n;
}

static void question_name(const unsigned char *q, size_t len, char *name)
{
    size_t o = 12, n = 0;
    while (o < len && q[o] && n < 200) {
        if (n) name[n++] = '.';
        memcpy(name + n, q + o + 1, q[o]);
        n += q[o];
        o += q[o] + 1;
    }
    name[n] = 0;
}

static void *udp_server(void *arg)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in name = {.sin_family = AF_INET, .sin_port = htons(53),
                               .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    bind(fd, (struct sockaddr *)&name, sizeof name);
    for (;;) {
        unsigned char q[512], a[512];
        struct sockaddr_in from;
        socklen_t len = sizeof from;
        ssize_t n = recvfrom(fd, q, sizeof q, 0, (struct sockaddr *)&from, &len);
        if (n < 12)
            continue;
        char qname[256];
        question_name(q, (size_t)n, qname);
        if (strcmp(qname, "silent.test") == 0)
            continue;
        if (strcmp(qname, "unrelated.test") == 0) {
            /* First an answer with another id, then the real one. */
            size_t m = build_answer(q, (size_t)n, a, qname, 0);
            a[0] ^= 0x55;
            sendto(fd, a, m, 0, (struct sockaddr *)&from, len);
            a[0] ^= 0x55;
            sendto(fd, a, m, 0, (struct sockaddr *)&from, len);
            continue;
        }
        size_t m = build_answer(q, (size_t)n, a, qname, 0);
        sendto(fd, a, m, 0, (struct sockaddr *)&from, len);
    }
    return NULL;
}

static void *tcp_server(void *arg)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in name = {.sin_family = AF_INET, .sin_port = htons(53),
                               .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    bind(fd, (struct sockaddr *)&name, sizeof name);
    listen(fd, 4);
    for (;;) {
        int c = accept(fd, NULL, NULL);
        if (c < 0)
            continue;
        unsigned char q[512], a[514];
        if (read(c, q, 2) == 2) {
            size_t qlen = (size_t)q[0] << 8 | q[1];
            size_t got = 0;
            while (got < qlen) {
                ssize_t n = read(c, q + got, qlen - got);
                if (n <= 0) break;
                got += (size_t)n;
            }
            char qname[256];
            question_name(q, qlen, qname);
            size_t m = build_answer(q, qlen, a + 2, qname, 1);
            a[0] = m >> 8;
            a[1] = m & 255;
            write(c, a, m + 2);
        }
        close(c);
    }
    return NULL;
}

static int resolve(const char *name, uint32_t *address)
{
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *ai;
    int error = getaddrinfo(name, "80", &hints, &ai);
    if (!error) {
        *address = ntohl(((struct sockaddr_in *)ai->ai_addr)->sin_addr.s_addr);
        freeaddrinfo(ai);
    }
    return error;
}

int main(void)
{
    FILE *f = fopen("/tmp/resolv.test", "w");
    if (!f) { mkdir("/tmp", 0755); f = fopen("/tmp/resolv.test", "w"); }
    fprintf(f, "nameserver 127.0.0.1\n");
    fclose(f);
    f = fopen("/tmp/hosts.test", "w");
    fprintf(f, "# comment\n192.168.7.7 printer.local  printer\n");
    fclose(f);
    setenv("RESOLV_CONF", "/tmp/resolv.test", 1);
    setenv("HOSTS_FILE", "/tmp/hosts.test", 1);
    uint32_t a;

    /* Without any server: numeric, localhost and hosts entries resolve. */
    CHECK(resolve("10.1.2.3", &a) == 0 && a == 0x0a010203, "numeric address");
    CHECK(resolve("localhost", &a) == 0 && a == 0x7f000001, "localhost");
    CHECK(resolve("printer", &a) == 0 && a == 0xc0a80707, "hosts alias");
    CHECK(resolve("PRINTER.local", &a) == 0 && a == 0xc0a80707, "hosts name, case-insensitive");
    struct addrinfo hints = {.ai_family = AF_INET, .ai_flags = AI_NUMERICHOST}, *ai;
    CHECK(getaddrinfo("printer", "80", &hints, &ai) == EAI_NONAME, "AI_NUMERICHOST rejects names");
    CHECK(getaddrinfo("1.2.3.4", "http", NULL, &ai) == EAI_SERVICE, "service names unsupported");
    hints.ai_flags = 0;
    hints.ai_family = 10;
    CHECK(getaddrinfo("1.2.3.4", NULL, &hints, &ai) == EAI_FAMILY, "IPv6 unsupported");
    CHECK(resolve("silent.test", &a) == EAI_AGAIN, "unavailable server times out");
    struct in_addr in;
    CHECK(inet_aton("1.2.3.4", &in) && in.s_addr == htonl(0x01020304), "inet_aton");
    CHECK(!inet_aton("1.2.3.256", &in) && !inet_aton("1.2.3", &in) && !inet_aton("1.2.3.4x", &in),
          "inet_aton rejects malformed text");
    char text[16];
    CHECK(inet_ntop(AF_INET, &in, text, sizeof text) && strcmp(text, "1.2.3.4") == 0, "inet_ntop");

    pthread_t udp, tcp;
    pthread_create(&udp, NULL, udp_server, NULL);
    pthread_create(&tcp, NULL, tcp_server, NULL);
    usleep(100000);
    CHECK(resolve("host.test", &a) == 0 && a == 0x0a000005, "positive answer");
    CHECK(resolve("nx.test", &a) == EAI_NONAME, "negative answer");
    CHECK(resolve("fail.test", &a) == EAI_FAIL, "server failure");
    CHECK(resolve("loop.test", &a) == EAI_FAIL, "compression loop rejected");
    CHECK(resolve("big.test", &a) == 0 && a == 0x0a000009, "truncation falls back to TCP");
    CHECK(resolve("cname.test", &a) == 0 && a == 0x0a000007, "CNAME followed");
    CHECK(resolve("chain.test", &a) == EAI_FAIL, "CNAME chain bounded");
    CHECK(resolve("unrelated.test", &a) == 0 && a == 0x0a000005, "unrelated transaction ignored");
    printf("netdnstest: %d failures\n", failures);
    return failures ? 1 : 0;
}
