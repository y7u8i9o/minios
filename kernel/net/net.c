/* Core initialization and the IP entry point. The handler pointer is
 * published with a release store and read with an acquire load on the
 * worker; the default drop counter is a relaxed atomic. */
#define KLOG_SUBSYS "net"
#include <net/net.h>
#include <lib/random.h>
#include <net/worker.h>
#include <net/loopback.h>
#include <net/ipv4.h>
#include <drivers/virtio/virtio_net.h>
#include <drivers/e1000e.h>
#include <debug/panic.h>
#include <klog.h>

static net_input_fn ip_input;
static uint64_t ip_dropped;

void net_ip_input(struct netif *n, struct pbuf *p)
{
    net_input_fn fn = __atomic_load_n(&ip_input, __ATOMIC_ACQUIRE);
    if (fn) {
        fn(n, p);
        return;
    }
    ipv4_input(n, p);
}

void net_ip_discard(struct pbuf *p)
{
    __atomic_fetch_add(&ip_dropped, 1, __ATOMIC_RELAXED);
    pbuf_free(p);
}

void net_set_ip_input(net_input_fn fn)
{
    __atomic_store_n(&ip_input, fn, __ATOMIC_RELEASE);
}

uint64_t net_ip_input_dropped(void)
{
    return __atomic_load_n(&ip_dropped, __ATOMIC_RELAXED);
}

void net_init(void)
{
    if (pbuf_pool_init() < 0)
        panic("net: cannot allocate the packet pool");
    random_init();
    netif_init();
    net_worker_start();
    loopback_init();
    ipv4_init();
    virtio_net_init();
    e1000e_init();
    netdev_init();
}
