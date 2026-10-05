/* ntpd: clock synchronisation over SNTP version 4 (RFC 4330, V2 of
 * docs/plan/release-0.6.0.md, docs/design/time.md).
 *
 *   ntpd [-c FILE]       run as the service of init
 *   ntpd -1 [-c FILE]    query once, correct the clock and exit
 *   ntpd -q              print the state of the last query
 *
 * The configuration (/etc/ntp.conf) lists servers as "server NAME" or
 * "server NAME port N", tried in order. Without a server line the service
 * asks pool.ntp.org. A query sends one request and computes the offset of
 * the local clock and the round trip delay from the four time stamps. An
 * offset above 128 ms steps the clock with clock_settime, a smaller one is
 * corrected gradually with adjtime. The service queries every 15 minutes
 * and after a failure every minute. Results go to the system log and to
 * /run/ntpd.state. */
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define CONF "/etc/ntp.conf"
#define STATE "/run/ntpd.state"
#define DEFAULT_SERVER "pool.ntp.org"
#define MAX_SERVERS 8
#define STEP_LIMIT_NS 128000000LL
#define INTERVAL_S 900
#define RETRY_S 60
#define TIMEOUT_MS 5000
/* Seconds from 1900-01-01, the era of NTP, to 1970-01-01. */
#define NTP_UNIX_DELTA 2208988800ULL

struct server {
    char name[128];
    char port[8];
};

struct result {
    char address[INET_ADDRSTRLEN];
    int stratum;
    int64_t offset_ns, delay_ns;
    const char *action;
};

static int once;

static void say(int priority, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (once) {
        printf("ntpd: ");
        vprintf(fmt, ap);
        printf("\n");
        fflush(stdout);
    } else {
        vsyslog(priority, fmt, ap);
    }
    va_end(ap);
}

static int read_conf(const char *path, struct server *servers)
{
    int n = 0;
    FILE *f = fopen(path, "r");
    char line[256];
    while (f && fgets(line, sizeof line, f) && n < MAX_SERVERS) {
        char name[128], word[16], port[8] = "123";
        int fields = sscanf(line, "server %127s %15s %7s", name, word, port);
        if (fields < 1 || name[0] == '#')
            continue;
        if (fields >= 2 && strcmp(word, "port") != 0)
            continue;
        strlcpy(servers[n].name, name, sizeof servers[n].name);
        strlcpy(servers[n].port, fields == 3 ? port : "123", sizeof servers[n].port);
        n++;
    }
    if (f)
        fclose(f);
    if (n == 0) {
        strlcpy(servers[0].name, DEFAULT_SERVER, sizeof servers[0].name);
        strlcpy(servers[0].port, "123", sizeof servers[0].port);
        n = 1;
    }
    return n;
}

static int64_t realtime_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/* An NTP time stamp: seconds since 1900 and a binary fraction, both 32
 * bits, big endian. */
static void put_stamp(uint8_t *p, int64_t ns)
{
    uint64_t sec = (uint64_t)(ns / 1000000000) + NTP_UNIX_DELTA;
    uint64_t frac = ((uint64_t)(ns % 1000000000) << 32) / 1000000000;
    for (int i = 0; i < 4; i++) {
        p[i] = (uint8_t)(sec >> (24 - 8 * i));
        p[4 + i] = (uint8_t)(frac >> (24 - 8 * i));
    }
}

static int64_t get_stamp(const uint8_t *p)
{
    uint64_t sec = (uint64_t)p[0] << 24 | (uint64_t)p[1] << 16 | (uint64_t)p[2] << 8 | p[3];
    uint64_t frac = (uint64_t)p[4] << 24 | (uint64_t)p[5] << 16 | (uint64_t)p[6] << 8 | p[7];
    return ((int64_t)sec - (int64_t)NTP_UNIX_DELTA) * 1000000000 + (int64_t)((frac * 1000000000) >> 32);
}

static int stamp_zero(const uint8_t *p)
{
    for (int i = 0; i < 8; i++)
        if (p[i])
            return 0;
    return 1;
}

/* One request to a server. Returns 0 and the result, or -1 with a message
 * in the log. */
static int query(const struct server *s, struct result *r)
{
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM }, *ai;
    int e = getaddrinfo(s->name, s->port, &hints, &ai);
    if (e != 0) {
        say(LOG_WARNING, "%s: %s", s->name, gai_strerror(e));
        return -1;
    }
    struct sockaddr_in addr;
    memcpy(&addr, ai->ai_addr, sizeof addr);
    freeaddrinfo(ai);
    inet_ntop(AF_INET, &addr.sin_addr, r->address, sizeof r->address);
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        say(LOG_WARNING, "%s: %s", s->name, strerror(errno));
        if (fd >= 0)
            close(fd);
        return -1;
    }
    /* LI 0, version 4, mode 3 (client). The transmit time stamp returns
     * as the originate time stamp of the answer. */
    uint8_t packet[48] = { 0x23 };
    int64_t t1 = realtime_ns();
    put_stamp(packet + 40, t1);
    uint8_t sent[8];
    memcpy(sent, packet + 40, 8);
    if (send(fd, packet, sizeof packet, 0) != (ssize_t)sizeof packet) {
        say(LOG_WARNING, "%s: %s", s->name, strerror(errno));
        close(fd);
        return -1;
    }
    struct pollfd p = { fd, POLLIN, 0 };
    uint8_t answer[68];
    ssize_t n = poll(&p, 1, TIMEOUT_MS) == 1 ? recv(fd, answer, sizeof answer, 0) : -1;
    int64_t t4 = realtime_ns();
    close(fd);
    if (n < 0) {
        say(LOG_WARNING, "%s (%s): no answer", s->name, r->address);
        return -1;
    }
    int li = answer[0] >> 6, version = answer[0] >> 3 & 7, mode = answer[0] & 7;
    const char *bad = n < 48 ? "short answer"
                      : mode != 4 ? "not a server answer"
                      : version < 3 || version > 4 ? "unknown version"
                      : li == 3 ? "server not synchronized"
                      : answer[1] == 0 || answer[1] > 15 ? "invalid stratum"
                      : memcmp(answer + 24, sent, 8) != 0 ? "answer to another request"
                      : stamp_zero(answer + 40) ? "zero transmit time" : NULL;
    if (bad) {
        say(LOG_WARNING, "%s (%s): %s", s->name, r->address, bad);
        return -1;
    }
    int64_t t2 = get_stamp(answer + 32), t3 = get_stamp(answer + 40);
    r->stratum = answer[1];
    r->offset_ns = ((t2 - t1) + (t3 - t4)) / 2;
    r->delay_ns = (t4 - t1) - (t3 - t2);
    return 0;
}

/* Steps or slews the clock by the offset of r. */
static int correct(struct result *r)
{
    int64_t off = r->offset_ns;
    if (off > STEP_LIMIT_NS || off < -STEP_LIMIT_NS) {
        int64_t now = realtime_ns() + off;
        struct timespec ts = { now / 1000000000, now % 1000000000 };
        r->action = "step";
        return clock_settime(CLOCK_REALTIME, &ts);
    }
    struct timeval delta = { off / 1000000000, (off % 1000000000) / 1000 };
    r->action = "slew";
    return adjtime(&delta, NULL);
}

static void write_state(const struct server *s, const struct result *r, const char *error)
{
    FILE *f = fopen(STATE, "w");
    if (!f)
        return;
    fprintf(f, "server %s\n", s->name);
    if (error) {
        fprintf(f, "error %s\n", error);
    } else {
        fprintf(f, "address %s\nstratum %d\noffset_us %lld\ndelay_us %lld\naction %s\nsynchronized %lld\n", r->address,
                r->stratum, (long long)(r->offset_ns / 1000), (long long)(r->delay_ns / 1000), r->action,
                (long long)(realtime_ns() / 1000000000));
    }
    fclose(f);
}

/* Queries the servers in order until one answers. Returns 0 when the
 * clock was corrected. */
static int synchronize(const struct server *servers, int n)
{
    for (int i = 0; i < n; i++) {
        struct result r = { .action = "none" };
        if (query(&servers[i], &r) < 0) {
            write_state(&servers[i], &r, "no valid answer");
            continue;
        }
        if (correct(&r) < 0) {
            say(LOG_ERR, "cannot correct the clock: %s", strerror(errno));
            write_state(&servers[i], &r, strerror(errno));
            return -1;
        }
        say(LOG_INFO, "%s (%s): stratum %d, offset %lld us, delay %lld us, %s", servers[i].name, r.address,
            r.stratum, (long long)(r.offset_ns / 1000), (long long)(r.delay_ns / 1000), r.action);
        write_state(&servers[i], &r, NULL);
        return 0;
    }
    return -1;
}

static int show_state(void)
{
    FILE *f = fopen(STATE, "r");
    if (!f) {
        fprintf(stderr, "ntpd: no state: %s\n", strerror(errno));
        return 1;
    }
    char line[256];
    while (fgets(line, sizeof line, f))
        fputs(line, stdout);
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    const char *conf = CONF;
    int opt;
    while ((opt = getopt(argc, argv, "1c:q")) != -1) {
        switch (opt) {
        case '1': once = 1; break;
        case 'c': conf = optarg; break;
        case 'q': return show_state();
        default:
            fprintf(stderr, "usage: ntpd [-1] [-c FILE] | ntpd -q\n");
            return 2;
        }
    }
    struct server servers[MAX_SERVERS];
    int n = read_conf(conf, servers);
    if (once)
        return synchronize(servers, n) == 0 ? 0 : 1;
    openlog("ntpd", LOG_PID, LOG_DAEMON);
    for (;;)
        sleep(synchronize(servers, n) == 0 ? INTERVAL_S : RETRY_S);
}
