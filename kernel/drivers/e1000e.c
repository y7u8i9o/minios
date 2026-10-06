/* Driver for the Intel 82574L (e1000e) as emulated by QEMU, following the
 * 82574 GbE Controller Family datasheet.
 *
 * The driver uses a single receive ring and a single transmit ring with
 * legacy descriptors and 2048-byte buffers. Outgoing frames are copied
 * into a per-slot transmit buffer, so the pbuf can be released at once.
 * The interrupt handler only acknowledges the cause and wakes the network
 * worker; all ring processing happens in e1000e_service on the worker. */
#define KLOG_SUBSYS "e1000e"
#include <drivers/e1000e.h>
#include <drivers/pci.h>
#include <drivers/timer.h>
#include <drivers/devinfo.h>
#include <arch/barrier.h>
#include <arch/irq.h>
#include <net/ipv4.h>
#include <net/netif.h>
#include <net/worker.h>
#include <net/clock.h>
#include <mm/memlayout.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <lib/cmdline.h>
#include <lib/string.h>
#include <klog.h>
#include <errno.h>

#define REG_CTRL   0x0000
#define REG_STATUS 0x0008
#define REG_EERD   0x0014
#define REG_ICR    0x00c0
#define REG_IMS    0x00d0
#define REG_IMC    0x00d8
#define REG_RCTL   0x0100
#define REG_TCTL   0x0400
#define REG_TIPG   0x0410
#define REG_RDBAL  0x2800
#define REG_RDBAH  0x2804
#define REG_RDLEN  0x2808
#define REG_RDH    0x2810
#define REG_RDT    0x2818
#define REG_TDBAL  0x3800
#define REG_TDBAH  0x3804
#define REG_TDLEN  0x3808
#define REG_TDH    0x3810
#define REG_TDT    0x3818
#define REG_MTA    0x5200
#define REG_RAL0   0x5400
#define REG_RAH0   0x5404

#define CTRL_SLU  (1u << 6)
#define CTRL_RST  (1u << 26)
#define STATUS_LU (1u << 1)

#define EERD_START      (1u << 0)
#define EERD_DONE       (1u << 1)
#define EERD_ADDR_SHIFT 2
#define EERD_DATA_SHIFT 16

#define ICR_TXDW   (1u << 0)
#define ICR_LSC    (1u << 2)
#define ICR_RXDMT0 (1u << 4)
#define ICR_RXO    (1u << 6)
#define ICR_RXT0   (1u << 7)

#define RCTL_EN    (1u << 1)
#define RCTL_BAM   (1u << 15)
#define RCTL_SECRC (1u << 26)

#define TCTL_EN   (1u << 1)
#define TCTL_PSP  (1u << 3)
#define TCTL_CT   (0x0fu << 4)
#define TCTL_COLD (0x3fu << 12)

#define RX_DD  (1u << 0)
#define RX_EOP (1u << 1)

#define TX_EOP  (1u << 0)
#define TX_IFCS (1u << 1)
#define TX_RS   (1u << 3)
#define TX_DD   (1u << 0)

#define RX_SLOTS 32
#define TX_SLOTS 32
#define BUFFER   2048
#define FRAME    1514

/* Legacy receive descriptor (section 7.1.4 of the datasheet). */
struct rx_desc {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
};

/* Legacy transmit descriptor (section 7.2.10 of the datasheet). */
struct tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t cso;
    uint8_t cmd;
    uint8_t status;
    uint8_t css;
    uint16_t special;
};

/* Controller state. The rings, their indices and link_up are confined to
 * the network worker, which is the only caller of transmit,
 * e1000e_service and e1000e_stop, so they need no lock. The interrupt
 * handler only reads ICR. ready is published with release/acquire
 * ordering. The state is static because the interrupt registration
 * outlives a controller reset. */
static struct {
    struct pci_dev *pci;
    volatile uint8_t *regs;
    struct netif interface;
    struct rx_desc *rx;
    struct tx_desc *tx;
    uintptr_t rx_phys, tx_phys;
    uint8_t *rx_buffer[RX_SLOTS];
    uint8_t *tx_buffer[TX_SLOTS];
    uintptr_t rx_buffer_phys[RX_SLOTS], tx_buffer_phys[TX_SLOTS];
    unsigned rx_next;               /* next receive descriptor to check */
    unsigned tx_tail, tx_clean;     /* next descriptor to fill, oldest in use */
    unsigned tx_used;
    int irq;
    const char *irq_kind;
    struct net_timer poll;
    bool link_up;
    bool mac_from_eeprom;
    bool ready, registered;
} nic;

static inline uint32_t rd32(unsigned off)
{
    return *(volatile uint32_t *)(nic.regs + off);
}

static inline void wr32(unsigned off, uint32_t v)
{
    *(volatile uint32_t *)(nic.regs + off) = v;
}

static void e1000e_irq(struct trapframe *tf, void *arg)
{
    /* Reading ICR clears the pending causes and deasserts the interrupt. */
    if (rd32(REG_ICR))
        net_worker_kick();
}

/* Read one 16-bit EEPROM word through EERD, giving up after 10 ms. */
static int eeprom_read(unsigned index, uint16_t *word)
{
    wr32(REG_EERD, EERD_START | (uint32_t)index << EERD_ADDR_SHIFT);
    uint64_t end = timer_ms() + 10;
    for (;;) {
        uint32_t v = rd32(REG_EERD);
        if (v & EERD_DONE) {
            *word = (uint16_t)(v >> EERD_DATA_SHIFT);
            return 0;
        }
        if (timer_ms() >= end)
            return -ETIMEDOUT;
        cpu_relax();
    }
}

/* The MAC address is stored in the first three EEPROM words. If the EEPROM
 * does not respond, fall back to receive address register 0, which the
 * controller loads from the EEPROM at reset. */
static void read_mac(uint8_t *mac)
{
    uint16_t w[3];
    if (eeprom_read(0, &w[0]) == 0 && eeprom_read(1, &w[1]) == 0 && eeprom_read(2, &w[2]) == 0) {
        for (unsigned i = 0; i < 3; i++) {
            mac[2 * i] = (uint8_t)w[i];
            mac[2 * i + 1] = (uint8_t)(w[i] >> 8);
        }
        nic.mac_from_eeprom = true;
        return;
    }
    uint32_t lo = rd32(REG_RAL0), hi = rd32(REG_RAH0);
    for (unsigned i = 0; i < 4; i++)
        mac[i] = (uint8_t)(lo >> (8 * i));
    mac[4] = (uint8_t)hi;
    mac[5] = (uint8_t)(hi >> 8);
}

/* Without an interrupt, this timer wakes the worker every 5 ms so that
 * e1000e_service still runs. */
static void poll_timer(struct net_timer *t)
{
    if (__atomic_load_n(&nic.ready, __ATOMIC_ACQUIRE))
        net_timer_arm(&nic.poll, net_clock_ms() + 5);
}

static void update_link(void)
{
    bool up = (rd32(REG_STATUS) & STATUS_LU) != 0;
    if (up == nic.link_up)
        return;
    nic.link_up = up;
    klog_info("%s: link %s", nic.interface.name, up ? "up" : "down");
}

static int transmit(struct netif *n, struct pbuf *p)
{
    if (!net_worker_is_current() || !__atomic_load_n(&nic.ready, __ATOMIC_ACQUIRE)) {
        pbuf_free(p);
        return -ENETDOWN;
    }
    if (p->len > FRAME || p->len < 14) {
        pbuf_free(p);
        return -EMSGSIZE;
    }
    e1000e_service();
    if (nic.tx_used == TX_SLOTS) {
        pbuf_free(p);
        return -ENOBUFS;
    }
    unsigned slot = nic.tx_tail;
    memcpy(nic.tx_buffer[slot], p->data, p->len);
    struct tx_desc *d = &nic.tx[slot];
    d->addr = nic.tx_buffer_phys[slot];
    d->length = (uint16_t)p->len;
    d->cso = 0;
    d->cmd = TX_EOP | TX_IFCS | TX_RS;
    d->status = 0;
    d->css = 0;
    d->special = 0;
    pbuf_free(p);
    nic.tx_tail = (slot + 1) % TX_SLOTS;
    nic.tx_used++;
    wmb();
    wr32(REG_TDT, nic.tx_tail);
    return 0;
}

static const struct netif_ops operations = {
    .output = transmit,
    .input = ethernet_input,
};

void e1000e_service(void)
{
    if (!__atomic_load_n(&nic.ready, __ATOMIC_ACQUIRE))
        return;
    if (nic.irq < 0)
        rd32(REG_ICR);
    update_link();
    while (nic.tx_used) {
        struct tx_desc *d = &nic.tx[nic.tx_clean];
        if (!(__atomic_load_n(&d->status, __ATOMIC_ACQUIRE) & TX_DD))
            break;
        nic.tx_clean = (nic.tx_clean + 1) % TX_SLOTS;
        nic.tx_used--;
    }
    bool returned = false;
    for (unsigned count = 0; count < RX_SLOTS; count++) {
        unsigned i = nic.rx_next;
        struct rx_desc *d = &nic.rx[i];
        uint8_t status = __atomic_load_n(&d->status, __ATOMIC_ACQUIRE);
        if (!(status & RX_DD))
            break;
        rmb();
        uint32_t len = d->length;
        /* With a 1500-byte MTU every valid frame fits in one buffer, so a
         * frame spanning several descriptors is dropped. */
        if ((status & RX_EOP) && !d->errors && len >= 14 && len <= FRAME && netif_is_up(&nic.interface)) {
            struct pbuf *p = pbuf_alloc(PBUF_DATA);
            if (p) {
                memcpy(pbuf_put(p, len), nic.rx_buffer[i], len);
                netif_input(&nic.interface, p);
            } else {
                atomic_u64_fetch_add_relaxed(&nic.interface.stats.rx_dropped, 1);
            }
        } else {
            atomic_u64_fetch_add_relaxed(&nic.interface.stats.rx_dropped, 1);
        }
        d->status = 0;
        d->errors = 0;
        nic.rx_next = (i + 1) % RX_SLOTS;
        returned = true;
    }
    if (returned) {
        /* RDT points at the last descriptor the controller may fill, i.e.
         * the one just before rx_next. */
        wmb();
        wr32(REG_RDT, (nic.rx_next + RX_SLOTS - 1) % RX_SLOTS);
    }
}

static void free_memory(void)
{
    pmm_free_dma_page(nic.rx);
    pmm_free_dma_page(nic.tx);
    nic.rx = NULL;
    nic.tx = NULL;
    /* Buffers are allocated two per page; the even slot owns the page. */
    for (unsigned i = 0; i < RX_SLOTS; i += 2)
        pmm_free_dma_page(nic.rx_buffer[i]);
    for (unsigned i = 0; i < TX_SLOTS; i += 2)
        pmm_free_dma_page(nic.tx_buffer[i]);
    memset(nic.rx_buffer, 0, sizeof nic.rx_buffer);
    memset(nic.tx_buffer, 0, sizeof nic.tx_buffer);
}

static int alloc_memory(void)
{
    nic.rx = pmm_alloc_dma_page(&nic.rx_phys);
    nic.tx = pmm_alloc_dma_page(&nic.tx_phys);
    if (!nic.rx || !nic.tx)
        return -ENOMEM;
    for (unsigned i = 0; i < RX_SLOTS; i += 2) {
        uintptr_t phys;
        uint8_t *page = pmm_alloc_dma_page(&phys);
        if (!page)
            return -ENOMEM;
        nic.rx_buffer[i] = page;
        nic.rx_buffer[i + 1] = page + BUFFER;
        nic.rx_buffer_phys[i] = phys;
        nic.rx_buffer_phys[i + 1] = phys + BUFFER;
    }
    for (unsigned i = 0; i < TX_SLOTS; i += 2) {
        uintptr_t phys;
        uint8_t *page = pmm_alloc_dma_page(&phys);
        if (!page)
            return -ENOMEM;
        nic.tx_buffer[i] = page;
        nic.tx_buffer[i + 1] = page + BUFFER;
        nic.tx_buffer_phys[i] = phys;
        nic.tx_buffer_phys[i + 1] = phys + BUFFER;
    }
    return 0;
}

/* Reset the controller, which also stops all DMA, and wait up to 10 ms
 * for the reset bit to clear. */
static int reset(void)
{
    wr32(REG_IMC, 0xffffffffu);
    wr32(REG_RCTL, 0);
    wr32(REG_TCTL, 0);
    wr32(REG_CTRL, rd32(REG_CTRL) | CTRL_RST);
    uint64_t end = timer_ms() + 10;
    sleep_ms(1);
    while (rd32(REG_CTRL) & CTRL_RST) {
        if (timer_ms() >= end)
            return -ETIMEDOUT;
        sleep_ms(1);
    }
    wr32(REG_IMC, 0xffffffffu);
    rd32(REG_ICR);
    return 0;
}

static void setup_rings(void)
{
    for (unsigned i = 0; i < RX_SLOTS; i++) {
        memset(&nic.rx[i], 0, sizeof nic.rx[i]);
        nic.rx[i].addr = nic.rx_buffer_phys[i];
    }
    memset(nic.tx, 0, TX_SLOTS * sizeof *nic.tx);
    nic.rx_next = 0;
    nic.tx_tail = nic.tx_clean = nic.tx_used = 0;

    for (unsigned i = 0; i < 128; i++)
        wr32(REG_MTA + 4 * i, 0);
    wr32(REG_RDBAL, (uint32_t)nic.rx_phys);
    wr32(REG_RDBAH, (uint32_t)((uint64_t)nic.rx_phys >> 32));
    wr32(REG_RDLEN, RX_SLOTS * sizeof(struct rx_desc));
    wr32(REG_RDH, 0);
    wr32(REG_RDT, RX_SLOTS - 1);
    wr32(REG_TDBAL, (uint32_t)nic.tx_phys);
    wr32(REG_TDBAH, (uint32_t)((uint64_t)nic.tx_phys >> 32));
    wr32(REG_TDLEN, TX_SLOTS * sizeof(struct tx_desc));
    wr32(REG_TDH, 0);
    wr32(REG_TDT, 0);
    /* Recommended inter-packet gap for the copper interface. */
    wr32(REG_TIPG, 8 | 8u << 10 | 6u << 20);
    wr32(REG_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT | TCTL_COLD);
    /* RCTL.BSIZE = 0 selects 2048-byte buffers. */
    wr32(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);
    wr32(REG_CTRL, rd32(REG_CTRL) | CTRL_SLU);
}

/* Use MSI when available and fall back to polling otherwise. The kernel
 * option e1000e=poll forces polling, which lets a test cover that path. */
static void setup_interrupt(void)
{
    nic.irq = -1;
    nic.irq_kind = "no";
    char opt[8];
    bool poll = cmdline_lookup("e1000e", opt, sizeof opt) && strcmp(opt, "poll") == 0;
    int irq = poll ? -1 : irq_alloc();
    if (irq < 0)
        return;
    irq_register((unsigned)irq, e1000e_irq, NULL);
    if (pci_msi_enable(nic.pci, (unsigned)irq) == 0) {
        nic.irq = irq;
        nic.irq_kind = "msi";
    }
}

int e1000e_stop(void)
{
    if (!net_worker_is_current())
        return -EINVAL;
    __atomic_store_n(&nic.ready, false, __ATOMIC_RELEASE);
    net_timer_cancel(&nic.poll);
    if (nic.registered) {
        netif_set_up(&nic.interface, false);
        arp_flush(&nic.interface);
    }
    int error = reset();
    if (error < 0)
        return error; /* the device may still DMA, so leave the memory allocated */
    free_memory();
    return 0;
}

bool e1000e_present(void)
{
    return nic.registered;
}

bool e1000e_mac_from_eeprom(void)
{
    return nic.mac_from_eeprom;
}

bool e1000e_link_up(void)
{
    return nic.link_up;
}

void e1000e_init(void)
{
    struct pci_dev *pci = pci_find(0x8086, 0x10d3);
    if (!pci || pci->driver || pci_bar_size(pci, 0) == 0 || pci->bar_is_io[0])
        return;
    nic.pci = pci;
    net_timer_init(&nic.poll, poll_timer);
    pci_enable_bus_master(pci);
    nic.regs = vmm_map_mmio(pci->bar[0], ALIGN_UP(pci_bar_size(pci, 0), PAGE_SIZE), VM_KERNEL_RW | VM_NOCACHE);
    if (!nic.regs || reset() < 0) {
        klog_error("controller does not reset");
        return;
    }
    read_mac(nic.interface.hwaddr);
    static const uint8_t zero[6];
    if ((nic.interface.hwaddr[0] & 1) || !memcmp(nic.interface.hwaddr, zero, 6)) {
        klog_error("invalid MAC address");
        return;
    }
    if (alloc_memory() < 0) {
        free_memory();
        klog_error("no memory for the rings");
        return;
    }
    setup_rings();
    setup_interrupt();
    netif_free_name("eth", nic.interface.name);
    nic.interface.flags = NETIF_ETHERNET;
    nic.interface.mtu = 1500;
    nic.interface.ops = &operations;
    nic.interface.driver = "e1000e";
    if (net_worker_add_service(e1000e_service) < 0 || netif_register(&nic.interface) < 0) {
        reset();
        free_memory();
        klog_error("cannot register the interface");
        return;
    }
    pci->driver = "e1000e";
    nic.registered = true;
    netif_set_up(&nic.interface, true);
    net_configure(&nic.interface, 0, 0, 0);
    __atomic_store_n(&nic.ready, true, __ATOMIC_RELEASE);
    if (nic.irq >= 0)
        wr32(REG_IMS, ICR_TXDW | ICR_LSC | ICR_RXDMT0 | ICR_RXO | ICR_RXT0);
    else
        net_timer_arm(&nic.poll, net_clock_ms() + 5);
    net_worker_kick();
    klog_info("%s ready, MAC %02x:%02x:%02x:%02x:%02x:%02x from the %s, %s interrupt, RX/TX %u/%u",
              nic.interface.name,
              nic.interface.hwaddr[0],
              nic.interface.hwaddr[1],
              nic.interface.hwaddr[2],
              nic.interface.hwaddr[3],
              nic.interface.hwaddr[4],
              nic.interface.hwaddr[5],
              nic.mac_from_eeprom ? "EEPROM" : "address register",
              nic.irq_kind,
              RX_SLOTS,
              TX_SLOTS);
}
