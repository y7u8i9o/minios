/* dhcpc: DHCP client (N10, RFC 2131/2132 subset). Acquires a lease for one
 * interface, configures it through /dev/net, writes /etc/resolv.conf,
 * renews at T1 (unicast), rebinds at T2 (broadcast) and drops the address
 * at expiry before discovering again. Retries back off from 4 to 64 s and
 * never block anything else: without a server the client keeps trying in
 * the background and the interface stays unconfigured.
 *   dhcpc [-i IF | -a] [-1] [-f] [-s SERVER] [-p PORT] [-t SECONDS]
 * -1 exits after the first lease (or failure after SECONDS), -f stays in
 * the foreground, -s/-p unicast to a test server instead of broadcasting.
 * -a, used by init's dhcp service, takes the interface from the first
 * "iface NAME dhcp" line of /etc/network, stays in the foreground, and
 * exits with status 0 when there is nothing to do (no such line, or no
 * such interface), so the service simply stops on a machine without a
 * network. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/ipc.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <minios/abi.h>

static const char *interface = "eth0";
static uint32_t server_override; /* -s */
static unsigned short server_port = 67;
static int one_shot, foreground;
static unsigned give_up = 60;
static unsigned char mac[6];
static uint32_t xid;

struct lease {
    uint32_t address, mask, gateway, server, dns[2];
    uint32_t seconds, t1, t2;
    time_t acquired;
    char domain[256]; /* option 15, the search domain; empty when absent */
};
static struct lease lease;
static enum { INIT, BOUND, RENEWING, REBINDING } state = INIT;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* The interface of the first "iface NAME dhcp" line of /etc/network, or
 * NULL. The name is static storage. */
static const char *network_dhcp_interface(void)
{
    static char name[64];
    FILE *f = fopen("/etc/network", "r");
    if (!f)
        return NULL;
    char line[256], kind[16];
    const char *found = NULL;
    while (!found && fgets(line, sizeof line, f)) {
        char *hash = strchr(line, '#');
        if (hash)
            *hash = 0;
        if (sscanf(line, "iface %63s %15s", name, kind) == 2 && strcmp(kind, "dhcp") == 0)
            found = name;
    }
    fclose(f);
    return found;
}

static int read_mac(void)
{
    FILE *f = fopen("/dev/net", "r");
    if (!f)
        return -1;
    char line[512], name[64];
    unsigned m[6];
    int ok = -1;
    while (ok < 0 && fgets(line, sizeof line, f)) {
        if (sscanf(line, "link %63s %x:%x:%x:%x:%x:%x", name, &m[0], &m[1], &m[2], &m[3], &m[4],
                   &m[5]) == 7 &&
            strcmp(name, interface) == 0) {
            for (int i = 0; i < 6; i++)
                mac[i] = (unsigned char)m[i];
            ok = 0;
        }
    }
    fclose(f);
    return ok;
}

static int configure(uint32_t address, uint32_t mask, uint32_t gateway)
{
    int fd = open("/dev/net", O_RDONLY);
    if (fd < 0)
        return -1;
    struct net_config c = {.address = address, .mask = mask, .gateway = gateway};
    strncpy(c.name, interface, sizeof c.name - 1);
    int r = ioctl(fd, NETIOC_CONFIGURE, &c);
    close(fd);
    return r;
}

static void write_resolv(const struct lease *l)
{
    FILE *f = fopen("/etc/resolv.conf", "w");
    if (!f)
        return;
    for (int i = 0; i < 2; i++) {
        if (l && l->dns[i]) {
            struct in_addr in = {.s_addr = htonl(l->dns[i])};
            fprintf(f, "nameserver %s\n", inet_ntoa(in));
        }
    }
    if (l && l->domain[0])
        fprintf(f, "search %s\n", l->domain);
    fclose(f);
}

static void put32(unsigned char *p, uint32_t v)
{
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}
static uint32_t get32(const unsigned char *p)
{
    return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

/* Build a message: type 1 DISCOVER or 3 REQUEST. ciaddr/unicast are used
 * while renewing. Returns the length. */
static size_t build(unsigned char *m, int type, uint32_t ciaddr, uint32_t requested,
                    uint32_t server)
{
    memset(m, 0, 576);
    m[0] = 1; /* BOOTREQUEST */
    m[1] = 1; /* Ethernet */
    m[2] = 6;
    put32(m + 4, xid);
    if (!ciaddr)
        m[10] = 0x80; /* broadcast flag: replies reach us before we have an address */
    put32(m + 12, ciaddr);
    memcpy(m + 28, mac, 6);
    unsigned char *o = m + 236;
    memcpy(o, "\x63\x82\x53\x63", 4);
    o += 4;
    *o++ = 53;
    *o++ = 1;
    *o++ = (unsigned char)type;
    *o++ = 61; /* client identifier */
    *o++ = 7;
    *o++ = 1;
    memcpy(o, mac, 6);
    o += 6;
    if (requested) {
        *o++ = 50;
        *o++ = 4;
        put32(o, requested);
        o += 4;
    }
    if (server) {
        *o++ = 54;
        *o++ = 4;
        put32(o, server);
        o += 4;
    }
    *o++ = 55; /* parameter request list */
    *o++ = 5;
    *o++ = 1;
    *o++ = 3;
    *o++ = 6;
    *o++ = 15;
    *o++ = 51;
    *o++ = 255;
    return (size_t)(o - m) < 300 ? 300 : (size_t)(o - m);
}

/* Option 15 names the domain of the client (RFC 2132 section 3.17). It is
 * kept only when it is a plausible domain name: letters, digits, hyphens
 * and dots, trailing dots and NUL padding removed. */
static void domain_option(const unsigned char *v, unsigned length, char *domain)
{
    while (length && (v[length - 1] == 0 || v[length - 1] == '.'))
        length--;
    domain[0] = 0;
    if (!length || length > 253)
        return;
    for (unsigned i = 0; i < length; i++) {
        unsigned char c = v[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '.'))
            return;
    }
    memcpy(domain, v, length);
    domain[length] = 0;
}

/* Validate a reply for our transaction and MAC. Returns the message type
 * (2 OFFER, 5 ACK, 6 NAK) with the lease fields, or 0 for anything else. */
static int parse(const unsigned char *m, size_t len, struct lease *l)
{
    if (len < 240 || m[0] != 2 || m[1] != 1 || m[2] != 6 || get32(m + 4) != xid ||
        memcmp(m + 28, mac, 6) || memcmp(m + 236, "\x63\x82\x53\x63", 4))
        return 0;
    memset(l, 0, sizeof *l);
    l->address = get32(m + 16);
    int type = 0;
    size_t o = 240;
    while (o < len) {
        unsigned code = m[o++];
        if (code == 255)
            break;
        if (code == 0)
            continue;
        if (o >= len)
            return 0;
        unsigned olen = m[o++];
        if (o + olen > len)
            return 0;
        const unsigned char *v = m + o;
        switch (code) {
        case 53: if (olen == 1) type = v[0]; break;
        case 1: if (olen == 4) l->mask = get32(v); break;
        case 3: if (olen >= 4) l->gateway = get32(v); break;
        case 6:
            if (olen >= 4) l->dns[0] = get32(v);
            if (olen >= 8) l->dns[1] = get32(v + 4);
            break;
        case 15: domain_option(v, olen, l->domain); break;
        case 51: if (olen == 4) l->seconds = get32(v); break;
        case 54: if (olen == 4) l->server = get32(v); break;
        case 58: if (olen == 4) l->t1 = get32(v); break;
        case 59: if (olen == 4) l->t2 = get32(v); break;
        default: break;
        }
        o += olen;
    }
    if (type == 6)
        return 6;
    if ((type == 2 || type == 5) && (!l->address || !l->mask || !l->server || !l->seconds ||
                                     l->seconds < 10 || (l->address >> 24) == 127))
        return 0; /* offer/ack without the fields we need: ignore */
    if (type == 2 || type == 5) {
        if (!l->t1 || l->t1 >= l->seconds)
            l->t1 = l->seconds / 2;
        if (!l->t2 || l->t2 >= l->seconds || l->t2 <= l->t1)
            l->t2 = l->seconds / 8 * 7;
    }
    return type;
}

static int open_socket(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof on);
    struct sockaddr_in name = {.sin_family = AF_INET, .sin_port = htons(68)};
    if (bind(fd, (struct sockaddr *)&name, sizeof name) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int transmit(int fd, const unsigned char *m, size_t len, uint32_t to)
{
    struct sockaddr_in name = {.sin_family = AF_INET, .sin_port = htons(server_port),
                               .sin_addr.s_addr = htonl(server_override ? server_override : to)};
    return sendto(fd, m, len, 0, (struct sockaddr *)&name, sizeof name) == (ssize_t)len ? 0 : -1;
}

/* Wait up to timeout for a reply of one of the wanted types. */
static int await(int fd, unsigned timeout_ms, int want_a, int want_b, struct lease *l)
{
    uint64_t deadline = now_ms() + timeout_ms;
    for (;;) {
        uint64_t now = now_ms();
        if (now >= deadline)
            return 0;
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        if (poll(&pfd, 1, (int)(deadline - now)) != 1)
            return 0;
        unsigned char m[1500];
        ssize_t n = recv(fd, m, sizeof m, 0);
        if (n <= 0)
            continue;
        int type = parse(m, (size_t)n, l);
        if (type == want_a || type == want_b)
            return type;
    }
}

static void report(const char *what, const struct lease *l)
{
    struct in_addr a = {.s_addr = htonl(l->address)}, s = {.s_addr = htonl(l->server)};
    char server[16];
    strcpy(server, inet_ntoa(s));
    printf("dhcpc: %s %s from %s lease %u s\n", what, inet_ntoa(a), server, l->seconds);
    fflush(stdout);
}

static void bind_lease(const struct lease *l)
{
    lease = *l;
    lease.acquired = time(NULL);
    if (configure(l->address, l->mask, l->gateway) < 0) {
        fprintf(stderr, "dhcpc: configure %s: %s\n", interface, strerror(errno));
        state = INIT;
        return;
    }
    write_resolv(l);
    state = BOUND;
}

static void drop_lease(void)
{
    if (state != INIT) {
        printf("dhcpc: lease expired, address removed\n");
        fflush(stdout);
        configure(0, 0, 0);
        write_resolv(NULL);
    }
    state = INIT;
    memset(&lease, 0, sizeof lease);
}

/* Discover and request: returns 0 when bound. */
static int acquire(int fd, unsigned timeout_ms)
{
    unsigned char m[576];
    struct lease offer, ack;
    xid = (uint32_t)rand() ^ (uint32_t)now_ms();
    if (transmit(fd, m, build(m, 1, 0, 0, 0), INADDR_BROADCAST) < 0)
        return -1;
    if (await(fd, timeout_ms, 2, 2, &offer) != 2)
        return -1;
    if (transmit(fd, m, build(m, 3, 0, offer.address, offer.server), INADDR_BROADCAST) < 0)
        return -1;
    int type = await(fd, timeout_ms, 5, 6, &ack);
    if (type != 5 || ack.address != offer.address)
        return -1;
    bind_lease(&ack);
    report("bound", &ack);
    return 0;
}

/* Renew (unicast to the server) or rebind (broadcast). */
static int refresh(int fd, unsigned timeout_ms, int broadcast)
{
    unsigned char m[576];
    struct lease ack;
    if (transmit(fd, m, build(m, 3, lease.address, 0, 0),
                 broadcast ? INADDR_BROADCAST : lease.server) < 0)
        return -1;
    int type = await(fd, timeout_ms, 5, 6, &ack);
    if (type == 6) {
        printf("dhcpc: lease declined by server\n");
        drop_lease();
        return -1;
    }
    if (type != 5)
        return -1;
    if (ack.address != lease.address || ack.mask != lease.mask || ack.gateway != lease.gateway ||
        memcmp(ack.dns, lease.dns, sizeof ack.dns) || strcmp(ack.domain, lease.domain))
        bind_lease(&ack);
    else {
        lease.seconds = ack.seconds;
        lease.t1 = ack.t1;
        lease.t2 = ack.t2;
        lease.acquired = time(NULL);
        state = BOUND;
    }
    report("renewed", &ack);
    return 0;
}

int main(int argc, char **argv)
{
    int automatic = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0 && i + 1 < argc)
            interface = argv[++i];
        else if (strcmp(argv[i], "-a") == 0)
            automatic = foreground = 1;
        else if (strcmp(argv[i], "-1") == 0)
            one_shot = 1;
        else if (strcmp(argv[i], "-f") == 0)
            foreground = 1;
        else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc)
            give_up = (unsigned)atoi(argv[++i]);
        else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
            server_port = (unsigned short)atoi(argv[++i]);
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            struct in_addr in;
            if (!inet_aton(argv[++i], &in)) {
                fprintf(stderr, "dhcpc: bad server %s\n", argv[i]);
                return 2;
            }
            server_override = ntohl(in.s_addr);
        } else {
            fprintf(stderr, "usage: dhcpc [-i IF | -a] [-1] [-f] [-s SERVER] [-p PORT] [-t SECONDS]\n");
            return 2;
        }
    }
    if (automatic) {
        interface = network_dhcp_interface();
        if (!interface) {
            printf("dhcpc: no dhcp interface in /etc/network\n");
            return 0;
        }
    }
    if (read_mac() < 0) {
        fprintf(stderr, "dhcpc: no interface %s\n", interface);
        return automatic ? 0 : 1;
    }
    if (!one_shot && !foreground) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("dhcpc: fork");
            return 1;
        }
        if (pid > 0)
            return 0;
        setpgid(0, 0);
    }
    int fd = open_socket();
    if (fd < 0) {
        fprintf(stderr, "dhcpc: socket: %s\n", strerror(errno));
        return 1;
    }
    srand((unsigned)now_ms() ^ mac[5]);
    uint64_t start = now_ms();
    unsigned backoff = 4;
    for (;;) {
        if (state == INIT) {
            if (acquire(fd, 2000) == 0) {
                backoff = 4;
                if (one_shot)
                    return 0;
                continue;
            }
            if (one_shot && now_ms() - start >= (uint64_t)give_up * 1000) {
                fprintf(stderr, "dhcpc: no lease within %u s\n", give_up);
                return 1;
            }
            sleep(backoff);
            backoff = backoff < 64 ? backoff * 2 : 64;
            continue;
        }
        time_t age = time(NULL) - lease.acquired;
        if (age >= (time_t)lease.seconds) {
            drop_lease();
            continue;
        }
        if (age >= (time_t)lease.t2) {
            state = REBINDING;
            if (refresh(fd, 2000, 1) < 0)
                sleep(lease.seconds - age > 60 ? 60 : 2);
        } else if (age >= (time_t)lease.t1) {
            state = RENEWING;
            if (refresh(fd, 2000, 0) < 0)
                sleep(lease.t2 - age > 60 ? 60 : 2);
        } else {
            unsigned wait = (unsigned)(lease.t1 - age);
            sleep(wait > 60 ? 60 : wait);
        }
    }
}
