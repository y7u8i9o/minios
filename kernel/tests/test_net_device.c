/* Split-ring fault injection and live NIC ownership tests (N03). */
#include <tests/ktest.h>
#include <console.h>
#include <net/ipv4.h>
#include <net/worker.h>
#include <net/clock.h>
#include <net/byteorder.h>
#include <net/checksum.h>
#include <ipc/socket.h>
#include <drivers/virtio/virtio.h>
#include <drivers/virtio/virtio_net.h>
#include <drivers/e1000e.h>
#include <drivers/timer.h>
#include <lib/string.h>
#include <errno.h>

#include "net_helpers.h"

static const uint8_t peer_mac[6] = {2, 0, 0, 0, 0, 1};

static unsigned completions;
static void completed(struct virtqueue *vq, uint16_t id, uint32_t len)
{
    completions++;
}
static void test_virtqueue(void)
{
    static struct virtqueue q;
    static struct virtq_desc desc[VIRTQ_MAX_SIZE];
    static struct virtq_avail avail;
    static struct virtq_used used;
    static uint16_t notify;
    q.size = VIRTQ_MAX_SIZE;
    q.desc = desc;
    q.avail = &avail;
    q.used = &used;
    q.notify = &notify;
    q.num_free = q.size;
    q.complete = completed;
    spinlock_init(&q.lock, "net_test_queue");
    for (unsigned i = 0; i < q.size; i++)
        desc[i].next = i + 1;
    spin_lock(&q.lock);
    for (unsigned i = 0; i < 66000; i++) {
        uint16_t id;
        ktest_assert(virtq_alloc_chain(&q, 1, &id) >= 0, "allocate across wrap");
        virtq_submit(&q, id, &notify);
        used.ring[used.idx % q.size] = (struct virtq_used_elem){id, 0};
        used.idx++;
        virtq_poll_locked(&q);
    }
    ktest_assert(completions == 66000 && q.num_free == q.size, "16-bit ring wrap accounting");
    uint16_t heads[VIRTQ_MAX_SIZE];
    for (unsigned i = 0; i < q.size; i++) {
        virtq_alloc_chain(&q, 1, &heads[i]);
        virtq_submit(&q, heads[i], &notify);
    }
    uint16_t id;
    ktest_assert(virtq_alloc_chain(&q, 1, &id) == -ENOSPC, "descriptor exhaustion");
    for (unsigned i = 0; i < q.size; i++) {
        used.ring[used.idx % q.size] = (struct virtq_used_elem){heads[i], 0};
        used.idx++;
    }
    virtq_poll_locked(&q);
    ktest_assert(q.num_free == q.size, "burst completions restore descriptors");
    used.ring[used.idx % q.size] = (struct virtq_used_elem){65536, 0};
    used.idx++;
    virtq_poll_locked(&q);
    ktest_assert(q.broken && q.bad_used == 1 && q.num_free == q.size,
                 "full-width invalid descriptor ID");
    q.broken = false;
    used.ring[used.idx % q.size] = (struct virtq_used_elem){heads[0], 0};
    used.idx++;
    virtq_poll_locked(&q);
    ktest_assert(q.broken && q.bad_used == 2, "duplicate completion rejected");
    q.broken = false;
    used.idx += q.size + 1;
    virtq_poll_locked(&q);
    ktest_assert(q.broken && q.bad_used == 3, "used index overrun");
    spin_unlock(&q.lock);
    uint8_t h[26] = {0};
    h[10] = 1;
    ktest_assert(virtio_net_header_valid(h, 26, 1526), "minimal RX frame");
    ktest_assert(!virtio_net_header_valid(h, 25, 1526) && !virtio_net_header_valid(h, 1600, 1526),
                 "short/oversized completions");
    h[10] = 0;
    ktest_assert(virtio_net_header_valid(h, 26, 1526), "QEMU non-merged RX compatibility");
    h[10] = 2;
    ktest_assert(!virtio_net_header_valid(h, 26, 1526), "merged RX rejected");
    h[10] = 1;
    h[0] = 1;
    ktest_assert(!virtio_net_header_valid(h, 26, 1526), "unnegotiated checksum offload rejected");
    h[0] = 0;
    h[1] = 1;
    ktest_assert(!virtio_net_header_valid(h, 26, 1526), "unnegotiated segmentation rejected");
    kprintf("net_virtqueue: ok\n");
}
KTEST_DEFINE("net_virtqueue", test_virtqueue);

static int raw_batch(struct net_request *r)
{
    struct netif *n = netif_find("eth0");
    for (unsigned i = 0; i < 16; i++) {
        struct pbuf *p = pbuf_alloc(PBUF_DATA);
        ktest_assert(p, "raw buffer");
        uint8_t *h = pbuf_put(p, 60);
        memset(h, 0xa5, 60);
        memcpy(h, n->hwaddr, 6);
        memcpy(h + 6, peer_mac, 6);
        net_put_be16(h + 12, 0x88b5);
        ktest_assert(netif_output(n, p) == 0, "raw transmit");
    }
    return 0;
}
static int stop_virtio(struct net_request *r)
{
    return virtio_net_stop();
}
static int stop_e1000e(struct net_request *r)
{
    return e1000e_stop();
}

/* Send 40 batches of 16 raw frames through eth0 to the echo peer, waiting
 * for each batch to come back, then reset the controller with stop and
 * verify that every packet buffer was returned to the pool. */
static void raw_frames(const char *name, int (*stop)(struct net_request *))
{
    struct netif *n = netif_find("eth0");
    ktest_assert(n && netif_is_up(n), "NIC published");
    struct pbuf_stats before, after;
    pbuf_get_stats(&before);
    uint64_t base = atomic_u64_load_relaxed(&n->stats.rx_packets);
    for (unsigned batch = 0; batch < 40; batch++) {
        struct net_request r;
        net_request_init(&r, raw_batch);
        net_request_run(&r);
        uint64_t deadline = timer_ms() + 3000;
        while (atomic_u64_load_relaxed(&n->stats.rx_packets) < base + 16 * (batch + 1) &&
               timer_ms() < deadline)
            sleep_ms(1);
        ktest_assert(atomic_u64_load_relaxed(&n->stats.rx_packets) >= base + 16 * (batch + 1),
                     "raw RX batch %u",
                     batch);
    }
    struct net_request r;
    net_request_init(&r, stop);
    ktest_assert(net_request_run(&r) == 0 && !netif_is_up(n), "reset stops DMA and interface");
    net_worker_drain();
    pbuf_get_stats(&after);
    ktest_assert(after.free == before.free,
                 "reset returns all packet ownership %u/%u",
                 after.free,
                 before.free);
    kprintf("%s: 640 raw frames, reset, ok\n", name);
}

static void test_nic(void)
{
    raw_frames("net_nic", stop_virtio);
}
KTEST_DEFINE("net_nic", test_nic);

/* The net_e1000e case attaches an 82574L with the harness MAC address and
 * no virtio-net device, so the e1000e becomes eth0. */
static void test_e1000e(void)
{
    static const uint8_t harness_mac[6] = {0x52, 0x54, 0x00, 0x4d, 0x49, 0x4f};
    struct netif *n = netif_find("eth0");
    ktest_assert(n && e1000e_present() && strcmp(n->driver, "e1000e") == 0, "eth0 is the e1000e");
    ktest_assert(e1000e_mac_from_eeprom(), "MAC address read from the EEPROM");
    ktest_assert(memcmp(n->hwaddr, harness_mac, 6) == 0,
                 "MAC address %02x:%02x:%02x:%02x:%02x:%02x",
                 n->hwaddr[0], n->hwaddr[1], n->hwaddr[2], n->hwaddr[3], n->hwaddr[4], n->hwaddr[5]);
    uint64_t deadline = timer_ms() + 3000;
    while (!e1000e_link_up() && timer_ms() < deadline)
        sleep_ms(10);
    ktest_assert(e1000e_link_up(), "link up");
    kprintf("net_e1000e: eth0 link up, MAC from the EEPROM\n");
    raw_frames("net_e1000e", stop_e1000e);
}
KTEST_DEFINE("net_e1000e", test_e1000e);

static void test_nic_failure(void)
{
    ktest_assert(!netif_find("eth0"), "failed NIC was not published");
    ktest_assert(virtio_net_test_clean(), "all partial queue and DMA allocations released");
    struct pbuf_stats stats;
    pbuf_get_stats(&stats);
    ktest_assert(stats.free == stats.total, "failure preserves the packet pool");
    kprintf("net_nic_failure: cleanup ok\n");
}
KTEST_DEFINE("net_nic_failure", test_nic_failure);
