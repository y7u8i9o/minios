#pragma once
#include <net/netif.h>

void tcp_init(void);
void tcp_input(struct netif *interface, struct pbuf *packet);
/* Worker-only error delivery. Quotes include an IPv4 header and eight TCP bytes. */
void tcp_icmp_error(const uint8_t *quote, size_t length, int error);

struct tcp_stats {
    uint64_t active_opens;
    uint64_t passive_opens;
    uint64_t established;
    uint64_t invalid;
    uint64_t resets;
    uint64_t retransmits;
    uint64_t timeouts;
    uint64_t backlog_drops;
    uint64_t suppressed;
    /* RFC 7323 (N13): connections that negotiated window scaling and
     * timestamps, segments dropped by PAWS or for a missing timestamp, and
     * round-trip samples taken from echoed timestamps. */
    uint64_t window_scaling;
    uint64_t timestamps;
    uint64_t paws_rejected;
    uint64_t timestamp_missing;
    uint64_t timestamp_samples;
    unsigned connections;
    unsigned half_open;
    unsigned accepted;
    unsigned time_wait;
    unsigned endpoints;
};
/* The snapshot is valid only on success. Queue pressure and cancellation
 * return a negative errno without modifying the caller's output. */
int tcp_get_stats(struct tcp_stats *out);

/* Tests install deterministic generators on the worker. NULL restores the
 * entropy-backed generators, which fail closed while random_ready() is false. */
void tcp_set_generators(uint32_t (*sequence)(void), uint16_t (*port)(void));
