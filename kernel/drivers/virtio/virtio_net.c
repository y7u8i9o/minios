/* One modern NIC, software checksums, one RX/TX pair. Dedicated RX DMA
 * storage avoids permanently reserving half the packet pool. Each completion
 * is recorded under its queue lock; only netd copies, delivers and refills.
 * The device object is static because its interrupt registration outlives reset. */
#define KLOG_SUBSYS "virtio-net"
#include <drivers/virtio/virtio_net.h>
#include <drivers/virtio/virtio.h>
#include <drivers/pci.h>
#include <net/ipv4.h>
#include <net/worker.h>
#include <net/clock.h>
#include <net/byteorder.h>
#include <mm/memlayout.h>
#include <mm/slab.h>
#include <lib/string.h>
#include <klog.h>
#include <errno.h>
#if CONFIG_TESTS
#include <lib/cmdline.h>
#endif

#define RX_SLOTS 32
#define TX_SLOTS 32
#define HEADER 12 /* modern virtio_net_hdr includes num_buffers, even without merge */
#define FRAME 1514
struct rx_slot {
    uint8_t *data;
    bool posted, done;
    uint32_t len;
};
static struct {
    struct virtio_dev dev;
    struct netif interface;
    struct virtqueue *rx, *tx;
    struct rx_slot slots[RX_SLOTS];
    uint8_t *rx_memory;
    struct pbuf *transmit[TX_SLOTS];
    bool tx_done[TX_SLOTS];
    struct net_timer retry;
    bool ready, registered;
} nic;

bool virtio_net_header_valid(const uint8_t *h, uint32_t len, uint32_t capacity)
{
    if (len < HEADER + 14 || len > capacity || len > HEADER + FRAME)
        return false;
    /* No offloads or merged receives. VirtIO 1.2 specifies num_buffers=1;
     * QEMU 11 without MRG_RXBUF leaves it zero. Accept that observed
     * single-buffer encoding too, never a multi-buffer completion. */
    return h[0] == 0 && h[1] == 0 && h[10] <= 1 && h[11] == 0;
}

static void rx_complete(struct virtqueue *vq, uint16_t head, uint32_t len)
{
    struct rx_slot *s = vq->cookie[head];
    if (!s || !s->posted || s->done) {
        vq->broken = true;
        return;
    }
    s->len = len;
    s->done = true;
    s->posted = false;
}

static void tx_complete(struct virtqueue *vq, uint16_t head, uint32_t len)
{
    /* Cookie is a stable slot, independent of descriptor reuse. */
    bool *done = vq->cookie[head];
    if (!done || *done || len) {
        vq->broken = true;
        return;
    }
    *done = true;
}

static void refill(struct rx_slot *s)
{
    uint16_t id;
    spin_lock(&nic.rx->lock);
    if (!s->posted && !s->done && virtq_alloc_chain(nic.rx, 1, &id) >= 0) {
        nic.rx->desc[id].addr = virt_to_phys(s->data);
        nic.rx->desc[id].len = HEADER + FRAME;
        nic.rx->desc[id].flags = VIRTQ_DESC_F_WRITE;
        s->posted = true;
        virtq_submit(nic.rx, id, s);
    }
    spin_unlock(&nic.rx->lock);
}

/* The shared packet reserve must protect ingress too: an ARP reply can
 * release queued data even when ordinary UDP receive storage is exhausted. */
static bool control_frame(const uint8_t *frame, size_t length)
{
    if (length < 14)
        return false;
    uint16_t type = net_get_be16(frame + 12);
    if (type == 0x0806)
        return true;
    if (type != 0x0800 || length < 34)
        return false;
    if (frame[23] == IPPROTO_ICMP)
        return true;
    if (frame[23] != IPPROTO_TCP || length < 54)
        return false;
    size_t ip_header = (frame[14] & 15) * 4;
    if (ip_header < 20 || length < 14 + ip_header + 20)
        return false;
    const uint8_t *tcp = frame + 14 + ip_header;
    size_t tcp_header = (tcp[12] >> 4) * 4;
    return tcp_header >= 20 && net_get_be16(frame + 16) == ip_header + tcp_header;
}

static int transmit(struct netif *n, struct pbuf *p)
{
    /* Only netd uses the hardware output operation. */
    if (!net_worker_is_current() || !nic.ready) {
        pbuf_free(p);
        return -ENETDOWN;
    }
    if (p->len > FRAME || p->len < 14) {
        pbuf_free(p);
        return -EMSGSIZE;
    }
    bool control = control_frame(p->data, p->len);
    unsigned slot, limit = control ? TX_SLOTS : TX_SLOTS - 4;
    for (slot = 0; slot < limit; slot++)
        if (!nic.transmit[slot])
            break;
    if (slot == limit) {
        pbuf_free(p);
        return -ENOBUFS;
    }
    uint8_t *header = pbuf_push(p, HEADER);
    if (!header) {
        pbuf_free(p);
        return -EMSGSIZE;
    }
    memset(header, 0, HEADER);
    uint16_t id;
    spin_lock(&nic.tx->lock);
    if (virtq_alloc_chain(nic.tx, 1, &id) < 0) {
        spin_unlock(&nic.tx->lock);
        pbuf_free(p);
        return -ENOBUFS;
    }
    nic.transmit[slot] = p;
    nic.tx_done[slot] = false;
    pbuf_transfer(p, PBUF_OWNER_STACK, PBUF_OWNER_DEVICE);
    nic.tx->desc[id].addr = virt_to_phys(p->data);
    nic.tx->desc[id].len = p->len;
    virtq_submit(nic.tx, id, &nic.tx_done[slot]);
    spin_unlock(&nic.tx->lock);
    return 0;
}

static const struct netif_ops operations = {
    .output = transmit,
    .input = ethernet_input,
};
static void retry(struct net_timer *t)
{
    virtio_net_service();
}

int virtio_net_stop(void)
{
    if (!net_worker_is_current())
        return -EINVAL;
    __atomic_store_n(&nic.ready, false, __ATOMIC_RELEASE);
    net_timer_cancel(&nic.retry);
    if (nic.registered) {
        netif_set_up(&nic.interface, false);
        arp_flush(&nic.interface);
    }
    int error = virtio_reset(&nic.dev);
    if (error < 0)
        return error; /* DMA storage remains pinned on reset failure */
    nic.rx = nic.tx = NULL;
    for (unsigned i = 0; i < TX_SLOTS; i++) {
        if (!nic.transmit[i])
            continue;
        pbuf_transfer(nic.transmit[i], PBUF_OWNER_DEVICE, PBUF_OWNER_STACK);
        pbuf_free(nic.transmit[i]);
        nic.transmit[i] = NULL;
    }
    kfree(nic.rx_memory);
    nic.rx_memory = NULL;
    memset(nic.slots, 0, sizeof nic.slots);
    return 0;
}

void virtio_net_service(void)
{
    if (!__atomic_load_n(&nic.ready, __ATOMIC_ACQUIRE))
        return;
    spin_lock(&nic.rx->lock);
    bool broken = nic.rx->broken;
    spin_unlock(&nic.rx->lock);
    spin_lock(&nic.tx->lock);
    broken |= nic.tx->broken;
    spin_unlock(&nic.tx->lock);
    if (broken || (nic.dev.common->device_status & 64)) {
        klog_error("queue/device failure; stopping NIC");
        virtio_net_stop();
        return;
    }
    for (unsigned i = 0; i < TX_SLOTS; i++) {
        spin_lock(&nic.tx->lock);
        struct pbuf *p = nic.tx_done[i] ? nic.transmit[i] : NULL;
        if (p) {
            nic.transmit[i] = NULL;
            nic.tx_done[i] = false;
        }
        spin_unlock(&nic.tx->lock);
        if (p) {
            pbuf_transfer(p, PBUF_OWNER_DEVICE, PBUF_OWNER_STACK);
            pbuf_free(p);
        }
    }
    bool pressure = false;
    for (unsigned i = 0; i < RX_SLOTS; i++) {
        struct rx_slot *s = &nic.slots[i];
        spin_lock(&nic.rx->lock);
        bool done = s->done;
        uint32_t len = s->len;
        spin_unlock(&nic.rx->lock);
        if (done) {
            if (netif_is_up(&nic.interface) &&
                virtio_net_header_valid(s->data, len, HEADER + FRAME)) {
                enum pbuf_class allocation =
                    control_frame(s->data + HEADER, len - HEADER) ? PBUF_CONTROL : PBUF_DATA;
                struct pbuf *p = pbuf_alloc(allocation);
                if (!p) {
                    pressure = true;
                    continue;
                }
                memcpy(pbuf_put(p, len - HEADER), s->data + HEADER, len - HEADER);
                netif_input(&nic.interface, p);
            } else {
                static bool reported;
                if (!reported) {
                    reported = true;
                    klog_warn("RX rejected: len %u header %02x %02x num_buffers %02x %02x",
                              len,
                              s->data[0],
                              s->data[1],
                              s->data[10],
                              s->data[11]);
                }
                atomic_u64_fetch_add_relaxed(&nic.interface.stats.rx_dropped, 1);
            }
            spin_lock(&nic.rx->lock);
            s->done = false;
            spin_unlock(&nic.rx->lock);
        }
        refill(s); /* callback has returned and transport freed its descriptor */
    }
    if (pressure)
        net_timer_arm(&nic.retry, net_clock_ms() + 10);
}

/* Boot-only fault points exercise the actual partial-initialization cleanup.
 * They are absent from production builds and never alter a published NIC. */
static bool initialization_fault(const char *stage)
{
#if CONFIG_TESTS
    char requested[16];
    return cmdline_lookup("net_fault", requested, sizeof requested) && !strcmp(stage, requested);
#else
    return false;
#endif
}

#if CONFIG_TESTS
bool virtio_net_test_clean(void)
{
    return !nic.registered && !nic.ready && !nic.rx_memory && !nic.dev.queues[0] &&
           !nic.dev.queues[1];
}
#endif

void virtio_net_init(void)
{
    struct pci_dev *pci = NULL;
    for (size_t i = 0; i < pci_count(); i++) {
        struct pci_dev *p = pci_device(i);
        if (p->vendor == VIRTIO_VENDOR && (p->device == 0x1041 || p->device == 0x1000)) {
            pci = p;
            break;
        }
    }
    if (!pci)
        return;
    net_timer_init(&nic.retry, retry);
    if (virtio_pci_setup(pci, &nic.dev) < 0 || virtio_negotiate(&nic.dev, 1ULL << 5) < 0)
        goto fail;
    if (!(nic.dev.features & (1ULL << 5)) || !nic.dev.device_cfg || nic.dev.device_cfg_len < 6)
        goto fail;
    /* STATUS is not negotiated: administrative up means link assumed up,
     * as permitted by VirtIO. No status configuration reads or offloads. */
    uint8_t generation;
    unsigned attempts = 0;
    do {
        if (++attempts > 100)
            goto fail;
        generation = nic.dev.common->config_generation;
        for (unsigned i = 0; i < 6; i++)
            nic.interface.hwaddr[i] = nic.dev.device_cfg[i];
    } while (generation != nic.dev.common->config_generation);
    static const uint8_t zero[6];
    if ((nic.interface.hwaddr[0] & 1) || !memcmp(nic.interface.hwaddr, zero, 6))
        goto fail;
    nic.rx = virtio_queue_setup(&nic.dev, 0, rx_complete);
    if (initialization_fault("rx_queue"))
        goto fail;
    nic.tx = virtio_queue_setup(&nic.dev, 1, tx_complete);
    if (!nic.rx || !nic.tx)
        goto fail;
    if (initialization_fault("tx_queue"))
        goto fail;
    nic.rx_memory = kzalloc(RX_SLOTS * (HEADER + FRAME));
    if (!nic.rx_memory)
        goto fail;
    for (unsigned i = 0; i < RX_SLOTS; i++) {
        nic.slots[i].data = nic.rx_memory + i * (HEADER + FRAME);
        refill(&nic.slots[i]);
    }
    memcpy(nic.interface.name, "eth0", 5);
    nic.interface.flags = NETIF_ETHERNET;
    nic.interface.mtu = 1500;
    nic.interface.ops = &operations;
    nic.dev.work_notify = net_worker_kick;
    if (initialization_fault("buffers"))
        goto fail;
    if (virtio_start(&nic.dev) < 0)
        goto fail;
    pci->driver = "virtio-net";
    if (initialization_fault("started"))
        goto fail;
    nic.interface.driver = "virtio-net";
    if (netif_register(&nic.interface) < 0)
        goto fail;
    nic.registered = true;
    netif_set_up(&nic.interface, true);
    /* Attach the interface without an address, so limited broadcasts
     * (DHCP discovery) have a route before any configuration exists. */
    net_configure(&nic.interface, 0, 0, 0);
    __atomic_store_n(&nic.ready, true, __ATOMIC_RELEASE);
    net_worker_kick();
    klog_info("eth0 ready, MAC %02x:%02x:%02x:%02x:%02x:%02x, RX/TX %u/%u",
              nic.interface.hwaddr[0],
              nic.interface.hwaddr[1],
              nic.interface.hwaddr[2],
              nic.interface.hwaddr[3],
              nic.interface.hwaddr[4],
              nic.interface.hwaddr[5],
              nic.rx->size,
              nic.tx->size);
    return;
fail:
    /* Before publication the worker cannot access the queues. A reset
     * timeout intentionally pins DMA memory instead of freeing it unsafely. */
    if (virtio_reset(&nic.dev) == 0) {
        kfree(nic.rx_memory);
        nic.rx_memory = NULL;
        nic.rx = nic.tx = NULL;
    }
    klog_error("initialization failed");
}
