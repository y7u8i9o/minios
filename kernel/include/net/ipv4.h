#pragma once
#include <net/netif.h>
#include <minios/abi.h>

/* IPv4 addresses in this internal API are host-order numbers (0x7f000001).
 * Configuration, route lookup and protocol entry points run only on netd.
 * net_configure is the kernel control boundary and marshals a copied request. */
#define IPV4_LOOPBACK 0x7f000001u
#define IPV4_HEADER 20
#define IPV4_MAX_PACKET (PBUF_SIZE - PBUF_HEADROOM)
#define IPV4_REASSEMBLY_SLOTS 8
#define IPV4_REASSEMBLY_MS 30000
struct net_route {
    struct netif *netif;
    uint32_t source, next_hop;
};
/* Worker-owned counters; readers take a snapshot through a worker request. */
struct ipv4_stats {
    uint64_t invalid;
    uint64_t fragments;
    uint64_t options;
    uint64_t reassembled, fragment_invalid, fragment_full, fragment_expired;
    unsigned reassembly_active, reassembly_high_water;
    uint64_t pmtu_updates, pmtu_rejected;
    uint64_t wrong_destination;
    uint64_t no_route;
    uint64_t too_big;

    uint64_t arp_requests;
    uint64_t arp_timeouts;
    uint64_t arp_full;

    uint64_t icmp_echo;
    uint64_t icmp_echo_reply;
    uint64_t icmp_errors;
    uint64_t icmp_suppressed;

    uint64_t udp_invalid;
    uint64_t udp_no_port;
    uint64_t udp_full;
};
extern struct ipv4_stats net_ip_stats; /* worker owned; snapshot via request */
int net_configure(struct netif *n, uint32_t address, uint32_t mask, uint32_t gateway);
int net_route_lookup(uint32_t destination, struct net_route *route);
bool ipv4_local(uint32_t address);
bool ipv4_unicast(uint32_t address);
void ipv4_init(void);
void ipv4_input(struct netif *n, struct pbuf *p);
/* Consumes p on every result; source 0 selects the route's source. */
int ipv4_output(struct pbuf *p, uint32_t source, uint32_t destination, uint8_t protocol);
void ethernet_input(struct netif *n, struct pbuf *p);
int ethernet_output(struct netif *n, struct pbuf *p, const uint8_t *mac, uint16_t type);
int arp_output(struct netif *n, struct pbuf *p, uint32_t next_hop);
void arp_input(struct netif *n, struct pbuf *p);
void arp_flush(struct netif *n);
void icmp_input(struct netif *n, struct pbuf *p);
void icmp_error(struct pbuf *original, uint8_t code);
uint32_t ipv4_address(struct netif *n);
uint32_t ipv4_netmask(struct netif *n);
void udp_init(void);
void udp_input(struct netif *n, struct pbuf *p);
void udp_output_error(const struct pbuf *p, int error);
void udp_icmp_error(const uint8_t *quote, size_t len, int error);
uint16_t udp_checksum(uint32_t source, uint32_t destination, const void *data, size_t len);

/* Fragment input consumes packet, returning a complete owned packet or NULL. */
struct pbuf *ipv4_reassemble(struct netif *interface, struct pbuf *packet);
void ipv4_reassembly_flush(struct netif *interface);
int ipv4_link_output(struct netif *interface, struct pbuf *packet, const uint8_t *mac);
unsigned ipv4_path_mtu(uint32_t destination, unsigned interface_mtu);
void ipv4_path_lower(uint32_t destination, unsigned mtu);
void ipv4_path_flush(void);
void ipv4_note_output(const struct pbuf *packet);
bool ipv4_validate_quote(const uint8_t *quote, size_t length);
void ipv4_path_feedback(const uint8_t *quote, size_t length, unsigned mtu);
void tcp_path_changed(uint32_t destination, unsigned mtu);
#define IPV4_BROADCAST 0xffffffffu
uint32_t ipv4_gateway(struct netif *n);
/* Text snapshots for /dev/net; worker context only. */
size_t ipv4_format_config(char *buf, size_t size);
size_t arp_format(char *buf, size_t size);
/* One echo request: blocks the caller up to timeout_ms; 0 on reply. */
int icmp_echo(uint32_t destination, uint16_t sequence, size_t size, unsigned timeout_ms,
              uint32_t *rtt_ms, uint8_t *ttl);
void icmp_echo_error(const uint8_t *quote, size_t length, int error);
void netdev_init(void);
void tcp_interface_changed(struct netif *interface, int error);
