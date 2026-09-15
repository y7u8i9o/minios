/* IPv4 resolver (N11): numeric text, /etc/hosts, then DNS over UDP with a
 * TCP retry for truncated answers. Every answer is matched on transaction
 * id, server address and question; names are parsed with bounds and a
 * jump limit so compression loops and out-of-range pointers are rejected. */
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/ipc.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <strings.h>

#define MAX_SERVERS 3
#define ATTEMPTS 2
#define TIMEOUT_MS 3000
#define MAX_CNAME 8
#define MAX_ANSWERS 8

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

static unsigned read_servers(uint32_t *servers)
{
    FILE *f = fopen(path_of("RESOLV_CONF", "/etc/resolv.conf"), "r");
    unsigned n = 0;
    if (!f)
        return 0;
    char line[256];
    while (n < MAX_SERVERS && fgets(line, sizeof line, f)) {
        const char *save;
        char *key = strtok_r(line, " \t\r\n", &save);
        char *value = key ? strtok_r(NULL, " \t\r\n", &save) : NULL;
        struct in_addr in;
        if (key && value && strcmp(key, "nameserver") == 0 && inet_aton(value, &in))
            servers[n++] = ntohl(in.s_addr);
    }
    fclose(f);
    return n;
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

/* Returns 0 with addresses filled, or an EAI code. */
static int query_dns(const char *name, uint32_t *addresses, unsigned *count)
{
    uint32_t servers[MAX_SERVERS];
    unsigned nservers = read_servers(servers);
    if (!nservers)
        return EAI_AGAIN;
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
            for (unsigned s = 0; s < nservers && alen < 0; s++) {
                alen = exchange(servers[s], query, qlen, answer, sizeof answer, 0);
                if (alen >= 12 && (answer[2] & 0x02)) {
                    alen = exchange(servers[s], query, qlen, answer, sizeof answer, 1);
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
        if (rcode == 3)
            return EAI_NONAME;
        if (rcode)
            return EAI_FAIL;
        unsigned answers = answer[6] << 8 | answer[7];
        char cname[256] = "";
        *count = 0;
        /* Two passes: the first learns a CNAME for the queried name, the
         * second collects addresses for the name or its target, so a
         * response carrying both is resolved in one round trip. */
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
                unsigned rdlen = answer[next + 8] << 8 | answer[next + 9];
                size_t rdata = next + 10;
                if (rdata + rdlen > (size_t)alen)
                    return EAI_FAIL;
                if (class == 1 && pass == 0 && type == 5 && !cname[0] &&
                    strcasecmp(owner, current) == 0) {
                    size_t unused;
                    if (decode_name(answer, (size_t)alen, rdata, cname, &unused) < 0)
                        return EAI_FAIL;
                }
                if (class == 1 && pass == 1 && type == 1 && rdlen == 4 && *count < MAX_ANSWERS &&
                    (strcasecmp(owner, current) == 0 || (cname[0] && strcasecmp(owner, cname) == 0)))
                    addresses[(*count)++] = (uint32_t)answer[rdata] << 24 |
                                            answer[rdata + 1] << 16 | answer[rdata + 2] << 8 |
                                            answer[rdata + 3];
                o = rdata + rdlen;
            }
        }
        if (*count)
            return 0;
        if (!cname[0])
            return EAI_NONAME;
        strcpy(current, cname);
    }
    return EAI_FAIL;
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
            int error = query_dns(node, addresses, &count);
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
