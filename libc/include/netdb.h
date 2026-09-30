#pragma once
/* Name resolution (N11): an IPv4 subset of getaddrinfo. Names come from
 * numeric text, /etc/hosts and the DNS servers of /etc/resolv.conf, with
 * its search list (N15). DNS answers are cached in the process for their
 * TTL, at most one hour; negative answers are cached as RFC 2308 allows.
 * Unsupported: AF_INET6, AI_CANONNAME, service names (numeric ports only),
 * getnameinfo and reverse lookups. See docs/design/network.md. */
#include <sys/socket.h>
#include <netinet/in.h>

struct addrinfo {
    int ai_flags;
    int ai_family;
    int ai_socktype;
    int ai_protocol;
    socklen_t ai_addrlen;
    struct sockaddr *ai_addr;
    char *ai_canonname;
    struct addrinfo *ai_next;
};

#define AI_PASSIVE     1
#define AI_CANONNAME   2
#define AI_NUMERICHOST 4
#define AI_NUMERICSERV 8

#define EAI_AGAIN    2   /* server unreachable or timed out */
#define EAI_BADFLAGS 3
#define EAI_FAIL     4   /* server failure or malformed answer */
#define EAI_FAMILY   5
#define EAI_MEMORY   6
#define EAI_NONAME   8   /* no such name */
#define EAI_SERVICE  9
#define EAI_SOCKTYPE 10
#define EAI_SYSTEM   11

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                struct addrinfo **result);
void freeaddrinfo(struct addrinfo *list);
const char *gai_strerror(int error);

/* The resolver reads RESOLV_CONF and HOSTS_FILE from the environment when
 * set; tests use them to point at scratch files. */

/* These MiniOS extensions serve the resolver cache of this process.
 * res_cache_remaining returns the seconds that the cached answer for name
 * has left, or -1 when there is none, and res_cache_flush removes every
 * cached answer. */
int res_cache_remaining(const char *name);
void res_cache_flush(void);
