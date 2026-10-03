/* N11: the resolver against a scripted DNS server on loopback. The queried
 * name selects the server's behaviour. No NIC is needed. With the argument
 * "cache" the program checks the cache, negative caching and the search
 * list of N15 instead; the server counts the queries for every name. */
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

/* counts records the queries received per name; the server threads write it
 * under counts_lock. */
static pthread_mutex_t counts_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    char name[96];
    int count;
} counts[96];

static void count_query(const char *name)
{
    pthread_mutex_lock(&counts_lock);
    unsigned i = 0;
    while (i < 95 && counts[i].name[0] && strcmp(counts[i].name, name))
        i++;
    if (!counts[i].name[0])
        snprintf(counts[i].name, sizeof counts[i].name, "%s", name);
    counts[i].count++;
    pthread_mutex_unlock(&counts_lock);
}

static int queries(const char *name)
{
    pthread_mutex_lock(&counts_lock);
    int n = 0;
    for (unsigned i = 0; i < 96 && counts[i].name[0]; i++)
        if (strcmp(counts[i].name, name) == 0)
            n = counts[i].count;
    pthread_mutex_unlock(&counts_lock);
    return n;
}

static int ends_with(const char *name, const char *suffix)
{
    size_t n = strlen(name), m = strlen(suffix);
    return n > m && strcmp(name + n - m, suffix) == 0;
}

/* add_soa appends an SOA record to the authority section, whose TTL and
 * MINIMUM give the negative caching time of RFC 2308. */
static size_t add_soa(unsigned char *a, size_t n, uint32_t ttl, uint32_t minimum)
{
    unsigned char rr[] = {0xc0, 12, 0, 6, 0, 1, ttl >> 24, ttl >> 16, ttl >> 8, ttl, 0, 30,
                          2, 'n', 's', 0, 4, 'h', 'o', 's', 't', 0,
                          0, 0, 0, 1, 0, 0, 0, 60, 0, 0, 0, 60, 0, 0, 0, 60,
                          minimum >> 24, minimum >> 16, minimum >> 8, minimum};
    memcpy(a + n, rr, sizeof rr);
    a[8] = 0;
    a[9] = 1;
    return n + sizeof rr;
}

/* add_address appends one A record for the question name with the given
 * TTL and address. */
static size_t add_address(unsigned char *a, size_t n, uint32_t ttl, uint32_t address)
{
    unsigned char rr[] = {0xc0, 12, 0, 1, 0, 1, ttl >> 24, ttl >> 16, ttl >> 8, ttl, 0, 4,
                          address >> 24, address >> 16, address >> 8, address};
    memcpy(a + n, rr, sizeof rr);
    return n + sizeof rr;
}

/* cache_answer builds the answers of the N15 names, which cover TTLs,
 * negative answers with and without SOA and the domains of the search
 * list, and returns 0 for any other name. */
static size_t cache_answer(unsigned char *a, size_t n, const char *name, unsigned *answers)
{
    if (strcmp(name, "ttl2.test") == 0)
        return *answers = 1, add_address(a, n, 2, 0x0a000102);
    if (strcmp(name, "ttl0.test") == 0)
        return *answers = 1, add_address(a, n, 0, 0x0a000100);
    if (strcmp(name, "long.test") == 0)
        return *answers = 1, add_address(a, n, 86400, 0x0a000103);
    if (strncmp(name, "fill", 4) == 0)
        return *answers = 1, add_address(a, n, 60, 0x0a000200 + (uint32_t)atoi(name + 4));
    if (strcmp(name, "cname2.test") == 0) {
        /* The answer is a CNAME with TTL 1 to host.test, whose address has
         * TTL 60. */
        a[n++] = 0xc0; a[n++] = 12;
        memcpy(a + n, "\0\5\0\1\0\0\0\1\0\x0b\4host\4test\0", 21); n += 21;
        size_t target = n - 11;
        unsigned char rr[] = {0xc0, (unsigned char)target, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 10, 0, 0, 5};
        memcpy(a + n, rr, sizeof rr);
        *answers = 2;
        return n + sizeof rr;
    }
    if (strcmp(name, "nxsoa.test") == 0) {
        a[3] = 0x83;
        return add_soa(a, n, 5, 2);
    }
    if (strcmp(name, "nxlong.test") == 0) {
        a[3] = 0x83;
        return add_soa(a, n, 86400, 86400);
    }
    if (strcmp(name, "nodata.test") == 0)
        return add_soa(a, n, 2, 30);
    if (strcmp(name, "www.example.test") == 0)
        return *answers = 1, add_address(a, n, 60, 0x0a00000b);
    if (strcmp(name, "only.other.test") == 0)
        return *answers = 1, add_address(a, n, 60, 0x0a00000c);
    if (strcmp(name, "absolute.test") == 0)
        return *answers = 1, add_address(a, n, 60, 0x0a00000d);
    if (strcmp(name, "dotted.name") == 0)
        return *answers = 1, add_address(a, n, 60, 0x0a00000e);
    if (ends_with(name, ".example.test") || ends_with(name, ".other.test")) {
        a[3] = 0x83;
        return add_soa(a, n, 60, 60);
    }
    return 0;
}
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
    size_t cached = cache_answer(a, n, name, &answers);
    if (cached) {
        a[6] = answers >> 8;
        a[7] = answers & 255;
        return cached;
    }
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
        count_query(qname);
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

static void write_resolv(const char *extra)
{
    FILE *f = fopen("/tmp/resolv.test", "w");
    fprintf(f, "nameserver 127.0.0.1\n%s", extra);
    fclose(f);
    res_cache_flush();
}

/* The N15 checks show that the cache honours TTLs with an upper bound,
 * caches negative answers only with an SOA record and for the RFC 2308
 * time and evicts the least recently used name, and that the search list
 * is applied by the ndots rule. */
static int cache_tests(void)
{
    uint32_t a;
    CHECK(resolve("ttl2.test", &a) == 0 && a == 0x0a000102 && queries("ttl2.test") == 1,
          "first lookup queries the server");
    CHECK(resolve("TTL2.test", &a) == 0 && a == 0x0a000102 && queries("ttl2.test") == 1,
          "second lookup, in other case, is answered from the cache");
    int left = res_cache_remaining("ttl2.test");
    CHECK(left >= 1 && left <= 2, "cached for the record's TTL of 2 s");
    usleep(2200000);
    CHECK(resolve("ttl2.test", &a) == 0 && queries("ttl2.test") == 2, "expired entry queried again");

    CHECK(resolve("ttl0.test", &a) == 0 && resolve("ttl0.test", &a) == 0 &&
          queries("ttl0.test") == 2 && res_cache_remaining("ttl0.test") == -1,
          "TTL 0 is not cached");
    CHECK(resolve("long.test", &a) == 0 && res_cache_remaining("long.test") > 3590 &&
          res_cache_remaining("long.test") <= 3600, "a TTL of one day is capped at one hour");
    CHECK(resolve("cname2.test", &a) == 0 && a == 0x0a000005 &&
          res_cache_remaining("cname2.test") == 1, "a chain lives as long as its shortest record");

    CHECK(resolve("nxsoa.test", &a) == EAI_NONAME && resolve("nxsoa.test", &a) == EAI_NONAME &&
          queries("nxsoa.test") == 1, "NXDOMAIN with SOA is cached");
    left = res_cache_remaining("nxsoa.test");
    CHECK(left >= 1 && left <= 2, "negative entry lives min(SOA TTL, MINIMUM) = 2 s");
    usleep(2200000);
    CHECK(resolve("nxsoa.test", &a) == EAI_NONAME && queries("nxsoa.test") == 2,
          "expired negative entry queried again");
    CHECK(resolve("nx.test", &a) == EAI_NONAME && resolve("nx.test", &a) == EAI_NONAME &&
          queries("nx.test") == 2, "NXDOMAIN without SOA is not cached");
    CHECK(resolve("nxlong.test", &a) == EAI_NONAME && res_cache_remaining("nxlong.test") <= 3600 &&
          res_cache_remaining("nxlong.test") > 3590, "negative lifetime capped at one hour");
    CHECK(resolve("nodata.test", &a) == EAI_NONAME && resolve("nodata.test", &a) == EAI_NONAME &&
          queries("nodata.test") == 1, "NODATA with SOA is cached");
    CHECK(resolve("fail.test", &a) == EAI_FAIL && resolve("fail.test", &a) == EAI_FAIL &&
          queries("fail.test") == 2, "server failure is not cached");

    res_cache_flush();
    char name[32];
    for (int i = 0; i < 40; i++) {
        snprintf(name, sizeof name, "fill%d.test", i);
        CHECK(resolve(name, &a) == 0 && a == 0x0a000200u + (uint32_t)i, "fill the cache");
    }
    CHECK(res_cache_remaining("fill0.test") == -1 && res_cache_remaining("fill7.test") == -1 &&
          res_cache_remaining("fill8.test") > 0 && res_cache_remaining("fill39.test") > 0,
          "32 entries retained, the least recently used replaced");

    write_resolv("search example.test other.test\n");
    CHECK(resolve("www", &a) == 0 && a == 0x0a00000b && queries("www.example.test") == 1 &&
          !queries("www"), "a short name is tried with the first search domain");
    CHECK(resolve("only", &a) == 0 && a == 0x0a00000c && queries("only.example.test") == 1 &&
          queries("only.other.test") == 1, "the next domain follows a negative answer");
    CHECK(resolve("absolute.test.", &a) == 0 && a == 0x0a00000d && queries("absolute.test") == 1 &&
          !queries("absolute.test.example.test"), "a trailing dot disables the search list");
    CHECK(resolve("dotted.name", &a) == 0 && a == 0x0a00000e && queries("dotted.name") == 1 &&
          !queries("dotted.name.example.test"), "a name with ndots dots is tried as given first");
    write_resolv("search example.test other.test\noptions ndots:3\n");
    CHECK(resolve("dotted.name", &a) == 0 && a == 0x0a00000e &&
          queries("dotted.name.example.test") == 1 && queries("dotted.name.other.test") == 1 &&
          queries("dotted.name") == 2, "with ndots:3 the search list comes first");
    write_resolv("domain example.test\n");
    CHECK(resolve("www", &a) == 0 && a == 0x0a00000b, "a domain line is a search list of one");
    printf("netdnstest: %d failures\n", failures);
    return failures ? 1 : 0;
}

int main(int argc, char **argv)
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
    if (argc > 1 && strcmp(argv[1], "cache") == 0)
        return cache_tests();
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
