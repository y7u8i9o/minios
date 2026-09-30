/* IPv4 resolver (N11): numeric text, /etc/hosts, then DNS over UDP with a
 * TCP retry for truncated answers. Every answer is matched on transaction
 * id, server address and question; names are parsed with bounds and a
 * jump limit so compression loops and out-of-range pointers are rejected.
 *
 * N15 adds a cache of DNS answers in each process, positive entries for
 * the smallest TTL of the records used and negative entries for the TTL
 * that RFC 2308 derives from the SOA record of a negative answer, both
 * capped at CACHE_TTL_MAX, and the search list of /etc/resolv.conf. */
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/ipc.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <strings.h>

#define MAX_SERVERS 3
#define ATTEMPTS 2
#define TIMEOUT_MS 3000
#define MAX_CNAME 8
#define MAX_ANSWERS 8
/* resolv.conf(5): at most six search domains of at most 256 bytes in all,
 * and an ndots option of at most 15. */
#define MAX_SEARCH 6
#define SEARCH_BYTES 256
#define MAX_NDOTS 15
/* The cache holds CACHE_ENTRIES names and replaces the least recently used
 * one when full. Every entry lives at most CACHE_TTL_MAX seconds. */
#define CACHE_ENTRIES 32
#define CACHE_TTL_MAX 3600

const char *gai_strerror(int error)
{
    switch (error) {
    case EAI_AGAIN: return "name server unreachable";
    case EAI_BADFLAGS: return "invalid flags";
    case EAI_FAIL: return "name server failure";
    case EAI_FAMILY: return "address family not supported";
    case EAI_MEMORY: return "out of memory";
    case EAI_NONAME: return "name not known";
    case EAI_SERVICE: return "service not supported";
    case EAI_SOCKTYPE: return "socket type not supported";
    case EAI_SYSTEM: return "system error";
    default: return "unknown resolver error";
    }
}

void freeaddrinfo(struct addrinfo *list)
{
    while (list) {
        struct addrinfo *next = list->ai_next;
        free(list->ai_addr);
        free(list);
        list = next;
    }
}

static int append(struct addrinfo ***tail, uint32_t address, unsigned short port, int socktype,
                  int protocol)
{
    struct addrinfo *ai = calloc(1, sizeof *ai);
    struct sockaddr_in *sin = calloc(1, sizeof *sin);
    if (!ai || !sin) {
        free(ai);
        free(sin);
        return EAI_MEMORY;
    }
    sin->sin_family = AF_INET;
    sin->sin_addr.s_addr = htonl(address);
    sin->sin_port = htons(port);
    ai->ai_family = AF_INET;
    ai->ai_socktype = socktype;
    ai->ai_protocol = protocol;
    ai->ai_addrlen = sizeof *sin;
    ai->ai_addr = (struct sockaddr *)sin;
    **tail = ai;
    *tail = &ai->ai_next;
    return 0;
}

static const char *path_of(const char *variable, const char *fallback)
{
    const char *v = getenv(variable);
    return v && *v ? v : fallback;
}

static int lookup_hosts(const char *name, uint32_t *address)
{
    FILE *f = fopen(path_of("HOSTS_FILE", "/etc/hosts"), "r");
    if (!f)
        return 0;
    char line[512];
    int found = 0;
    while (!found && fgets(line, sizeof line, f)) {
        char *hash = strchr(line, '#');
        if (hash)
            *hash = 0;
        const char *save;
        char *field = strtok_r(line, " \t\r\n", &save);
        struct in_addr in;
        if (!field || !inet_aton(field, &in))
            continue;
        while ((field = strtok_r(NULL, " \t\r\n", &save))) {
            if (strcasecmp(field, name) == 0) {
                *address = ntohl(in.s_addr);
                found = 1;
                break;
            }
        }
    }
    fclose(f);
    return found;
}

/* The parts of /etc/resolv.conf the resolver uses. As in resolv.conf(5),
 * the last "search" or "domain" line gives the search list (a "domain"
 * line is a list of one), and "options ndots:N" sets how many dots make a
 * name be tried as given before the search list (default 1). */
struct resolv_config {
    uint32_t servers[MAX_SERVERS];
    unsigned nservers;
    char search[MAX_SEARCH][SEARCH_BYTES];
    unsigned nsearch;
    unsigned ndots;
};

static void read_config(struct resolv_config *config)
{
    memset(config, 0, sizeof *config);
    config->ndots = 1;
    FILE *f = fopen(path_of("RESOLV_CONF", "/etc/resolv.conf"), "r");
    if (!f)
        return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        const char *save;
        char *key = strtok_r(line, " \t\r\n", &save);
        if (!key)
            continue;
        char *value = strtok_r(NULL, " \t\r\n", &save);
        struct in_addr in;
        if (strcmp(key, "nameserver") == 0 && value && config->nservers < MAX_SERVERS &&
            inet_aton(value, &in)) {
            config->servers[config->nservers++] = ntohl(in.s_addr);
        } else if (strcmp(key, "search") == 0 || strcmp(key, "domain") == 0) {
            config->nsearch = 0;
            size_t total = 0;
            for (; value && config->nsearch < MAX_SEARCH; value = strtok_r(NULL, " \t\r\n", &save)) {
                size_t length = strlen(value);
                while (length && value[length - 1] == '.')
                    value[--length] = 0;
                if (!length || total + length + 1 > SEARCH_BYTES)
                    break;
                memcpy(config->search[config->nsearch++], value, length + 1);
                total += length + 1;
                if (strcmp(key, "domain") == 0)
                    break;
            }
        } else if (strcmp(key, "options") == 0) {
            for (; value; value = strtok_r(NULL, " \t\r\n", &save))
                if (strncmp(value, "ndots:", 6) == 0) {
                    long n = strtol(value + 6, NULL, 10);
                    config->ndots = n < 0 ? 0 : n > MAX_NDOTS ? MAX_NDOTS : (unsigned)n;
                }
        }
    }
    fclose(f);
}

/* The cache of this process. cache_lock protects every entry and
 * cache_clock, so threads of one process may resolve concurrently. Keys
 * are lower-case names without a trailing dot. A negative entry stores
 * EAI_NONAME and no address. A child created by fork starts with a copy. */
struct cache_entry {
    char name[256];
    int error;
    unsigned count;
    uint32_t addresses[MAX_ANSWERS];
    uint64_t expires_ms, used;
};
static struct cache_entry cache[CACHE_ENTRIES];
static uint64_t cache_clock;
static pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void cache_key(const char *name, char *key)
{
    size_t n = 0;
    for (; name[n] && n < 255; n++)
        key[n] = name[n] >= 'A' && name[n] <= 'Z' ? (char)(name[n] - 'A' + 'a') : name[n];
    while (n && key[n - 1] == '.')
        n--;
    key[n] = 0;
}

/* Caller holds cache_lock. Expired entries are dropped when found. */
static struct cache_entry *cache_find(const char *key, uint64_t now)
{
    for (unsigned i = 0; i < CACHE_ENTRIES; i++) {
        struct cache_entry *e = &cache[i];
        if (!e->name[0] || strcmp(e->name, key))
            continue;
        if (now >= e->expires_ms) {
            e->name[0] = 0;
            return NULL;
        }
        return e;
    }
    return NULL;
}

/* Returns 1 with the cached result of name, or 0 when it is not cached. */
static int cache_lookup(const char *name, int *error, uint32_t *addresses, unsigned *count)
{
    char key[256];
    cache_key(name, key);
    pthread_mutex_lock(&cache_lock);
    struct cache_entry *e = cache_find(key, monotonic_ms());
    if (e) {
        e->used = ++cache_clock;
        *error = e->error;
        *count = e->count;
        memcpy(addresses, e->addresses, e->count * sizeof *addresses);
    }
    pthread_mutex_unlock(&cache_lock);
    return e != NULL;
}

/* A TTL of 0 means the answer may be used for this lookup only (RFC 1035
 * section 3.2.1), so nothing is stored. */
static void cache_store(const char *name, int error, const uint32_t *addresses, unsigned count,
                        uint32_t ttl)
{
    if (!ttl)
        return;
    if (ttl > CACHE_TTL_MAX)
        ttl = CACHE_TTL_MAX;
    char key[256];
    cache_key(name, key);
    pthread_mutex_lock(&cache_lock);
    uint64_t now = monotonic_ms();
    struct cache_entry *e = cache_find(key, now);
    for (unsigned i = 0; !e && i < CACHE_ENTRIES; i++)
        if (!cache[i].name[0] || now >= cache[i].expires_ms)
            e = &cache[i];
    if (!e) {
        e = &cache[0];
        for (unsigned i = 1; i < CACHE_ENTRIES; i++)
            if (cache[i].used < e->used)
                e = &cache[i];
    }
    strcpy(e->name, key);
    e->error = error;
    e->count = count;
    memcpy(e->addresses, addresses, count * sizeof *addresses);
    e->expires_ms = now + (uint64_t)ttl * 1000;
    e->used = ++cache_clock;
    pthread_mutex_unlock(&cache_lock);
}

int res_cache_remaining(const char *name)
{
    char key[256];
    cache_key(name, key);
    pthread_mutex_lock(&cache_lock);
    uint64_t now = monotonic_ms();
    struct cache_entry *e = cache_find(key, now);
    int seconds = e ? (int)((e->expires_ms - now + 999) / 1000) : -1;
    pthread_mutex_unlock(&cache_lock);
    return seconds;
}

void res_cache_flush(void)
{
    pthread_mutex_lock(&cache_lock);
    memset(cache, 0, sizeof cache);
    pthread_mutex_unlock(&cache_lock);
}

/* Wire helpers. A name is at most 255 bytes of labels; parsing follows at
 * most 16 compression pointers, each of which must point backwards. */
static size_t encode_name(const char *name, unsigned char *out)
{
    size_t n = 0, total = 0;
    const char *p = name;
    for (;;) {
        const char *dot = strchr(p, '.');
        size_t len = dot ? (size_t)(dot - p) : strlen(p);
        if (len == 0 || len > 63 || total + len + 1 > 255)
            return 0;
        out[n++] = (unsigned char)len;
        memcpy(out + n, p, len);
        n += len;
        total += len + 1;
        if (!dot)
            break;
        p = dot + 1;
        if (!*p)
            break;
    }
    out[n++] = 0;
    return n;
}

static int decode_name(const unsigned char *msg, size_t len, size_t offset, char *out,
                       size_t *next)
{
    size_t o = offset, jumps = 0, n = 0;
    int jumped = 0;
    for (;;) {
        if (o >= len)
            return -1;
        unsigned c = msg[o];
        if ((c & 0xc0) == 0xc0) {
            if (o + 1 >= len || ++jumps > 16)
                return -1;
            size_t target = ((c & 0x3f) << 8) | msg[o + 1];
            if (target >= o)
                return -1; /* forward or self references loop */
            if (!jumped)
                *next = o + 2;
            jumped = 1;
            o = target;
            continue;
        }
        if (c & 0xc0)
            return -1;
        o++;
        if (!c)
            break;
        if (o + c > len || n + c + 1 > 255)
            return -1;
        if (n)
            out[n++] = '.';
        memcpy(out + n, msg + o, c);
        n += c;
        o += c;
    }
    out[n] = 0;
    if (!jumped)
        *next = o;
    return 0;
}

static int random_id(unsigned short *id)
{
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0)
        return -1;
    int ok = read(fd, id, sizeof *id) == (ssize_t)sizeof *id;
    close(fd);
    return ok ? 0 : -1;
}

/* One exchange with one server; returns the answer length or -1. tcp
 * selects the stream transport used after a truncated UDP answer. */
static ssize_t exchange(uint32_t server, const unsigned char *query, size_t qlen,
                        unsigned char *answer, size_t alen, int tcp)
{
    int fd = socket(AF_INET, tcp ? SOCK_STREAM : SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_in name = {.sin_family = AF_INET, .sin_port = htons(53),
                               .sin_addr.s_addr = htonl(server)};
    ssize_t result = -1;
    if (connect(fd, (struct sockaddr *)&name, sizeof name) < 0)
        goto out;
    if (tcp) {
        unsigned char prefix[2] = {qlen >> 8, qlen & 255};
        if (write(fd, prefix, 2) != 2 || write(fd, query, qlen) != (ssize_t)qlen)
            goto out;
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        size_t got = 0, want = 2;
        unsigned char lenbuf[2];
        while (got < want) {
            if (poll(&pfd, 1, TIMEOUT_MS) != 1)
                goto out;
            ssize_t n = read(fd, got < 2 ? lenbuf + got : answer + got - 2,
                             got < 2 ? 2 - got : want - got);
            if (n <= 0)
                goto out;
            got += (size_t)n;
            if (got == 2) {
                size_t total = (size_t)lenbuf[0] << 8 | lenbuf[1];
                if (total > alen)
                    goto out;
                want = 2 + total;
            }
        }
        result = (ssize_t)(got - 2);
    } else {
        if (send(fd, query, qlen, 0) != (ssize_t)qlen)
            goto out;
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        for (;;) {
            if (poll(&pfd, 1, TIMEOUT_MS) != 1)
                goto out;
            /* A connected UDP socket only receives from the queried server;
             * an answer with another id or question is ignored and the
             * wait continues within the same timeout budget. */
            ssize_t n = recv(fd, answer, alen, 0);
            if (n < 0)
                goto out;
            if (n >= 12 && !memcmp(answer, query, 2) && (answer[2] & 0x80)) {
                result = n;
                break;
            }
        }
    }
out:
    close(fd);
    return result;
}

static uint32_t get32(const unsigned char *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* RFC 2308 section 5: a negative answer may be cached for the smaller of
 * the TTL of the SOA record in its authority section and the SOA MINIMUM
 * field. Returns that TTL, or 0 when the answer carries no usable SOA
 * record, since negative answers without one must not be cached. offset is
 * the start of the authority section. */
static uint32_t negative_ttl(const unsigned char *answer, size_t alen, size_t offset)
{
    unsigned authorities = answer[8] << 8 | answer[9];
    for (unsigned i = 0; i < authorities; i++) {
        char owner[256];
        size_t next;
        if (decode_name(answer, alen, offset, owner, &next) < 0 || next + 10 > alen)
            return 0;
        unsigned type = answer[next] << 8 | answer[next + 1];
        unsigned class = answer[next + 2] << 8 | answer[next + 3];
        uint32_t ttl = get32(answer + next + 4);
        unsigned rdlen = answer[next + 8] << 8 | answer[next + 9];
        size_t rdata = next + 10;
        if (rdata + rdlen > alen)
            return 0;
        if (type == 6 && class == 1) {
            char skip[256];
            size_t o;
            if (decode_name(answer, rdata + rdlen, rdata, skip, &o) < 0 ||
                decode_name(answer, rdata + rdlen, o, skip, &o) < 0 || o + 20 != rdata + rdlen)
                return 0;
            uint32_t minimum = get32(answer + o + 16);
            if (ttl & 0x80000000u)
                ttl = 0; /* RFC 2181 section 8: a TTL above 2^31 - 1 counts as 0 */
            return ttl < minimum ? ttl : minimum;
        }
        offset = rdata + rdlen;
    }
    return 0;
}

/* Returns 0 with addresses filled, or an EAI code. ttl receives the time
 * the result may be cached: the smallest TTL of the records used on
 * success, the RFC 2308 TTL for EAI_NONAME, 0 otherwise. */
static int query_dns(const struct resolv_config *config, const char *name, uint32_t *addresses,
                     unsigned *count, uint32_t *ttl)
{
    *ttl = 0;
    if (!config->nservers)
        return EAI_AGAIN;
    uint32_t lifetime = UINT32_MAX;
    char current[256];
    strncpy(current, name, sizeof current - 1);
    current[sizeof current - 1] = 0;
    for (unsigned chain = 0; chain <= MAX_CNAME; chain++) {
        unsigned char query[512], answer[4096];
        unsigned short id;
        if (random_id(&id) < 0)
            return EAI_SYSTEM;
        memset(query, 0, 12);
        query[0] = id >> 8;
        query[1] = id & 255;
        query[2] = 1; /* recursion desired */
        query[5] = 1; /* one question */
        size_t nlen = encode_name(current, query + 12);
        if (!nlen)
            return EAI_NONAME;
        size_t qlen = 12 + nlen;
        query[qlen++] = 0;
        query[qlen++] = 1; /* A */
        query[qlen++] = 0;
        query[qlen++] = 1; /* IN */
        ssize_t alen = -1;
        int last = EAI_AGAIN;
        for (unsigned attempt = 0; attempt < ATTEMPTS && alen < 0; attempt++) {
            for (unsigned s = 0; s < config->nservers && alen < 0; s++) {
                alen = exchange(config->servers[s], query, qlen, answer, sizeof answer, 0);
                if (alen >= 12 && (answer[2] & 0x02)) {
                    alen = exchange(config->servers[s], query, qlen, answer, sizeof answer, 1);
                    if (alen < 0)
                        last = EAI_FAIL;
                }
            }
        }
        if (alen < 12)
            return last;
        /* The question section must echo ours before any answer counts. */
        if (memcmp(answer + 12, query + 12, qlen - 12) != 0 || (answer[4] << 8 | answer[5]) != 1)
            return EAI_FAIL;
        unsigned rcode = answer[3] & 15;
        if (rcode && rcode != 3)
            return EAI_FAIL;
        unsigned answers = answer[6] << 8 | answer[7];
        char cname[256] = "";
        *count = 0;
        size_t authority = qlen;
        /* Two passes: the first learns a CNAME for the queried name, the
         * second collects addresses for the name or its target, so a
         * response carrying both is resolved in one round trip. The TTL of
         * every record used bounds the lifetime of the result. */
        for (int pass = 0; pass < 2; pass++) {
            size_t o = qlen;
            for (unsigned i = 0; i < answers; i++) {
                char owner[256];
                size_t next;
                if (decode_name(answer, (size_t)alen, o, owner, &next) < 0 ||
                    next + 10 > (size_t)alen)
                    return EAI_FAIL;
                unsigned type = answer[next] << 8 | answer[next + 1];
                unsigned class = answer[next + 2] << 8 | answer[next + 3];
                uint32_t record_ttl = get32(answer + next + 4);
                unsigned rdlen = answer[next + 8] << 8 | answer[next + 9];
                size_t rdata = next + 10;
                if (rdata + rdlen > (size_t)alen)
                    return EAI_FAIL;
                if (record_ttl & 0x80000000u)
                    record_ttl = 0;
                if (class == 1 && pass == 0 && type == 5 && !cname[0] &&
                    strcasecmp(owner, current) == 0) {
                    size_t unused;
                    if (decode_name(answer, (size_t)alen, rdata, cname, &unused) < 0)
                        return EAI_FAIL;
                    if (record_ttl < lifetime)
                        lifetime = record_ttl;
                }
                if (class == 1 && pass == 1 && type == 1 && rdlen == 4 && *count < MAX_ANSWERS &&
                    (strcasecmp(owner, current) == 0 || (cname[0] && strcasecmp(owner, cname) == 0))) {
                    addresses[(*count)++] = get32(answer + rdata);
                    if (record_ttl < lifetime)
                        lifetime = record_ttl;
                }
                o = rdata + rdlen;
            }
            authority = o;
        }
        if (rcode == 3 || (!*count && !cname[0])) {
            /* NXDOMAIN, or NODATA: no address and no alias for the name. */
            uint32_t negative = negative_ttl(answer, (size_t)alen, authority);
            *ttl = negative < lifetime ? negative : lifetime;
            return EAI_NONAME;
        }
        if (*count) {
            *ttl = lifetime;
            return 0;
        }
        strcpy(current, cname);
    }
    return EAI_FAIL;
}

/* One name as a DNS query, through the cache. */
static int lookup_dns(const struct resolv_config *config, const char *name, uint32_t *addresses,
                      unsigned *count)
{
    int error;
    if (cache_lookup(name, &error, addresses, count))
        return error;
    uint32_t ttl;
    error = query_dns(config, name, addresses, count, &ttl);
    if (!error || error == EAI_NONAME)
        cache_store(name, error, addresses, error ? 0 : *count, ttl);
    return error;
}

/* The names tried for node, in order (resolv.conf(5)): a name with a
 * trailing dot is absolute and tried alone; a name with at least ndots
 * dots is tried as given and then with each search domain; a name with
 * fewer dots is tried with each search domain first and as given last.
 * Only "no such name" moves on to the next candidate; any other failure
 * ends the lookup. */
static int resolve_dns(const char *node, uint32_t *addresses, unsigned *count)
{
    struct resolv_config config;
    read_config(&config);
    size_t length = strlen(node);
    if (!length || length > 253 + (node[length - 1] == '.'))
        return EAI_NONAME;
    if (node[length - 1] == '.') {
        char absolute[256];
        memcpy(absolute, node, length - 1);
        absolute[length - 1] = 0;
        return lookup_dns(&config, absolute, addresses, count);
    }
    unsigned dots = 0;
    for (const char *p = node; *p; p++)
        dots += *p == '.';
    int error = EAI_NONAME;
    int first = dots >= config.ndots;
    if (first) {
        error = lookup_dns(&config, node, addresses, count);
        if (error != EAI_NONAME)
            return error;
    }
    for (unsigned i = 0; i < config.nsearch; i++) {
        char candidate[512];
        if ((size_t)snprintf(candidate, sizeof candidate, "%s.%s", node, config.search[i]) > 253)
            continue;
        error = lookup_dns(&config, candidate, addresses, count);
        if (error != EAI_NONAME)
            return error;
    }
    if (!first)
        error = lookup_dns(&config, node, addresses, count);
    return error;
}

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                struct addrinfo **result)
{
    *result = NULL;
    int flags = hints ? hints->ai_flags : 0;
    int family = hints ? hints->ai_family : AF_UNSPEC;
    int socktype = hints ? hints->ai_socktype : 0;
    if (flags & ~(AI_PASSIVE | AI_NUMERICHOST | AI_NUMERICSERV | AI_CANONNAME))
        return EAI_BADFLAGS;
    if (family != AF_UNSPEC && family != AF_INET)
        return EAI_FAMILY;
    if (socktype && socktype != SOCK_STREAM && socktype != SOCK_DGRAM)
        return EAI_SOCKTYPE;
    if (!node && !service)
        return EAI_NONAME;
    unsigned short port = 0;
    if (service) {
        char *end;
        unsigned long v = strtoul(service, &end, 10);
        if (!*service || *end || v > 65535)
            return EAI_SERVICE; /* only numeric services */
        port = (unsigned short)v;
    }
    uint32_t addresses[MAX_ANSWERS];
    unsigned count = 0;
    if (!node) {
        addresses[count++] = flags & AI_PASSIVE ? INADDR_ANY : INADDR_LOOPBACK;
    } else {
        struct in_addr in;
        if (inet_aton(node, &in)) {
            addresses[count++] = ntohl(in.s_addr);
        } else if (flags & AI_NUMERICHOST) {
            return EAI_NONAME;
        } else if (strcasecmp(node, "localhost") == 0) {
            addresses[count++] = INADDR_LOOPBACK;
        } else if (lookup_hosts(node, &addresses[0])) {
            count = 1;
        } else {
            int error = resolve_dns(node, addresses, &count);
            if (error)
                return error;
        }
    }
    struct addrinfo **tail = result;
    for (unsigned i = 0; i < count; i++) {
        int error = 0;
        if (!socktype || socktype == SOCK_STREAM)
            error = append(&tail, addresses[i], port, SOCK_STREAM, IPPROTO_TCP);
        if (!error && (!socktype || socktype == SOCK_DGRAM))
            error = append(&tail, addresses[i], port, SOCK_DGRAM, IPPROTO_UDP);
        if (error) {
            freeaddrinfo(*result);
            *result = NULL;
            return error;
        }
    }
    return 0;
}
