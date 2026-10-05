/* NVMe controllers (R2 of docs/plan/release-0.5.0.md, D3 of
 * docs/plan/drivers.md, docs/design/nvme.md), written from the NVM Express
 * Base Specification 2.0 and the NVM Command Set Specification 1.0. The
 * driver resets and enables the controller with an admin queue pair,
 * identifies the controller and its active namespaces, creates one I/O
 * queue pair and registers each namespace as a block device. One command
 * is outstanding per controller at a time. Data passes through bounce
 * pages, which a PRP list describes to the controller. */
#define KLOG_SUBSYS "nvme"
#include <drivers/nvme.h>
#include <drivers/pci.h>
#include <drivers/timer.h>
#include <drivers/devinfo.h>
#include <block/blockdev.h>
#include <arch/barrier.h>
#include <arch/irq.h>
#include <mm/memlayout.h>
#include <mm/pmm.h>
#include <mm/slab.h>
#include <mm/vmm.h>
#include <sched/wait.h>
#include <sync/spinlock.h>
#include <sync/mutex.h>
#include <lib/cmdline.h>
#include <lib/printf.h>
#include <lib/string.h>
#include <klog.h>
#include <errno.h>

/* Controller registers. */
#define REG_CAP         0x00
#define REG_VS          0x08
#define REG_INTMS       0x0c
#define REG_CC          0x14
#define REG_CSTS        0x1c
#define REG_AQA         0x24
#define REG_ASQ         0x28
#define REG_ACQ         0x30
#define REG_DOORBELLS   0x1000

#define CC_EN           (1u << 0)
#define CC_IOSQES       (6u << 16)      /* submission queue entries of 2^6 bytes */
#define CC_IOCQES       (4u << 20)      /* completion queue entries of 2^4 bytes */
#define CSTS_RDY        (1u << 0)
#define CSTS_CFS        (1u << 1)

/* Admin commands. */
#define ADM_CREATE_SQ   0x01
#define ADM_CREATE_CQ   0x05
#define ADM_IDENTIFY    0x06
#define ADM_SET_FEATURES 0x09
#define FEAT_NUM_QUEUES 0x07
#define CNS_NAMESPACE   0x00
#define CNS_CONTROLLER  0x01
#define CNS_ACTIVE_NS   0x02

/* NVM commands. */
#define NVM_FLUSH       0x00
#define NVM_WRITE       0x01
#define NVM_READ        0x02

#define QUEUE_ENTRIES   64              /* a submission queue of 64 entries fills one page */
#define BOUNCE_PAGES    32              /* the largest transfer, 128 KiB */
#define ADMIN_TIMEOUT_MS 5000
#define IO_TIMEOUT_MS   30000
#define MAX_CONTROLLERS 4
#define MAX_NAMESPACES  16

struct sqe {
    uint8_t opcode;
    uint8_t flags;
    uint16_t cid;
    uint32_t nsid;
    uint64_t reserved;
    uint64_t mptr;
    uint64_t prp1, prp2;
    uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
} __packed;

struct cqe {
    uint32_t result;
    uint32_t reserved;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status;                    /* bit 0 is the phase tag */
} __packed;

/* A queue pair. The submission tail is protected by nvme.io_lock, the
 * completion head and phase by nvme.lock. */
struct queue {
    unsigned qid;
    struct sqe *sq;
    uintptr_t sq_phys;
    struct cqe *cq;
    uintptr_t cq_phys;
    unsigned sq_tail;
    unsigned cq_head;
    uint16_t phase;
};

struct nvme;

/* A namespace and its block device. */
struct nvme_ns {
    struct blockdev bdev;
    struct nvme *ctrl;
    uint32_t nsid;
};

/* One controller.
 *
 * io_lock serializes the commands: the submission tails, next_cid, the
 * bounce pages and the PRP list. lock protects the completion heads and
 * phases and the completion record (wait_cid, wait_qid, done, status,
 * result). The interrupt handler takes lock. The other fields are written
 * during the probe and read afterwards without a lock. */
struct nvme {
    struct pci_dev *pci;
    unsigned index;
    volatile uint8_t *regs;
    unsigned stride;                    /* bytes between doorbells */
    unsigned version;
    struct queue admin, io;
    uint8_t *bounce[BOUNCE_PAGES];
    uintptr_t bounce_phys[BOUNCE_PAGES];
    uint64_t *prp_list;
    uintptr_t prp_phys;
    unsigned max_pages;                 /* pages of one transfer */
    bool volatile_cache;
    bool failed;                        /* a command did not complete; written under io_lock */
    int irq;
    const char *irq_kind;
    char model[41], serial[21], firmware[9];
    uint32_t nn;
    struct nvme_ns *ns[MAX_NAMESPACES];
    unsigned nns;

    struct mutex io_lock;
    struct spinlock lock;
    struct waitq waitq;
    uint16_t next_cid;
    uint16_t wait_cid;
    unsigned wait_qid;
    bool done;
    uint16_t status;
    uint32_t result;
};

static struct nvme *controllers[MAX_CONTROLLERS];
static unsigned ncontrollers;           /* written by nvme_init before any device is registered */

static inline uint32_t rd32(struct nvme *x, unsigned off)
{
    return *(volatile uint32_t *)(x->regs + off);
}

static inline void wr32(struct nvme *x, unsigned off, uint32_t v)
{
    *(volatile uint32_t *)(x->regs + off) = v;
}

/* 64 bit registers are accessed as two 32 bit halves, the low half first,
 * which every controller accepts. */
static inline uint64_t rd64(struct nvme *x, unsigned off)
{
    uint64_t lo = rd32(x, off);
    return lo | (uint64_t)rd32(x, off + 4) << 32;
}

static inline void wr64(struct nvme *x, unsigned off, uint64_t v)
{
    wr32(x, off, (uint32_t)v);
    wr32(x, off + 4, (uint32_t)(v >> 32));
}

/* Consume the completions of q. A completion whose identifier is the one
 * the caller waits for completes the wait. The caller has acquired
 * x->lock. Returns true when the wait completed. */
static bool process_queue(struct nvme *x, struct queue *q)
{
    bool consumed = false, wake = false;
    for (;;) {
        struct cqe *c = &q->cq[q->cq_head];
        uint16_t status = __atomic_load_n(&c->status, __ATOMIC_ACQUIRE);
        if ((status & 1) != q->phase)
            break;
        rmb();
        if (q->qid == x->wait_qid && c->cid == x->wait_cid && !x->done) {
            x->status = status >> 1;
            x->result = c->result;
            x->done = true;
            wake = true;
        }
        if (++q->cq_head == QUEUE_ENTRIES) {
            q->cq_head = 0;
            q->phase ^= 1;
        }
        consumed = true;
    }
    if (consumed)
        wr32(x, REG_DOORBELLS + (2 * q->qid + 1) * x->stride, q->cq_head);
    return wake;
}

static void nvme_irq(struct trapframe *tf, void *arg)
{
    struct nvme *x = arg;
    spin_lock(&x->lock);
    bool wake = process_queue(x, &x->admin);
    if (x->io.cq)
        wake |= process_queue(x, &x->io);
    if (wake)
        waitq_wake_all(&x->waitq);
    spin_unlock(&x->lock);
}

/* Submit cmd on q and wait for its completion. The completion queue is
 * also read by the waiter, which completes the wait without an interrupt:
 * every millisecond when the controller has none, every second otherwise.
 * The caller has locked x->io_lock. */
static int submit(struct nvme *x, struct queue *q, struct sqe *cmd, uint32_t *result, uint64_t timeout_ms)
{
    if (x->failed)
        return -EIO;
    spin_lock(&x->lock);
    cmd->cid = x->next_cid++;
    x->wait_cid = cmd->cid;
    x->wait_qid = q->qid;
    x->done = false;
    q->sq[q->sq_tail] = *cmd;
    q->sq_tail = (q->sq_tail + 1) % QUEUE_ENTRIES;
    wmb();
    wr32(x, REG_DOORBELLS + 2 * q->qid * x->stride, q->sq_tail);
    uint64_t end = timer_ms() + timeout_ms;
    int r = 0;
    for (;;) {
        process_queue(x, q);
        if (x->done)
            break;
        uint64_t now = timer_ms();
        if (now >= end) {
            r = -ETIMEDOUT;
            break;
        }
        waitq_wait_timeout(&x->waitq, &x->lock, MIN(end, now + (x->irq >= 0 ? 1000 : 1)));
    }
    uint16_t status = x->status;
    if (result)
        *result = x->result;
    spin_unlock(&x->lock);
    if (r < 0) {
        /* The command remains in the queue, and its identifier could be
         * reused. The controller is not used again. */
        klog_error("nvme%u: command %02x on queue %u: no completion within %lu ms", x->index, cmd->opcode,
                   q->qid, timeout_ms);
        x->failed = true;
        return r;
    }
    if (status & 0x7fff) {
        klog_warn("nvme%u: command %02x on queue %u: status type %u code 0x%02x", x->index, cmd->opcode, q->qid,
                  (status >> 8) & 7, status & 0xff);
        return -EIO;
    }
    return 0;
}

/* Describe the first bytes bytes of the bounce pages in cmd. */
static void set_prps(struct nvme *x, struct sqe *cmd, size_t bytes)
{
    unsigned pages = (unsigned)((bytes + PAGE_SIZE - 1) / PAGE_SIZE);
    cmd->prp1 = x->bounce_phys[0];
    if (pages == 2) {
        cmd->prp2 = x->bounce_phys[1];
    } else if (pages > 2) {
        for (unsigned i = 1; i < pages; i++)
            x->prp_list[i - 1] = x->bounce_phys[i];
        cmd->prp2 = x->prp_phys;
    }
}

/* Copy between buf and the bounce pages. */
static void bounce_copy(struct nvme *x, void *buf, size_t bytes, bool to_bounce)
{
    for (unsigned i = 0; bytes; i++) {
        size_t n = MIN(bytes, (size_t)PAGE_SIZE);
        if (to_bounce)
            memcpy(x->bounce[i], buf, n);
        else
            memcpy(buf, x->bounce[i], n);
        buf = (uint8_t *)buf + n;
        bytes -= n;
    }
}

/* An admin command with a page of data from the controller in bounce
 * page 0. */
static int admin_identify(struct nvme *x, uint32_t nsid, uint32_t cns)
{
    struct sqe cmd = { .opcode = ADM_IDENTIFY, .nsid = nsid, .cdw10 = cns };
    memset(x->bounce[0], 0, PAGE_SIZE);
    set_prps(x, &cmd, PAGE_SIZE);
    return submit(x, &x->admin, &cmd, NULL, ADMIN_TIMEOUT_MS);
}

static int nvme_rw(struct blockdev *dev, uint64_t sector, uint32_t count, void *buf, bool write)
{
    struct nvme_ns *ns = dev->priv;
    struct nvme *x = ns->ctrl;
    if (sector + count > dev->nsectors || sector + count < sector)
        return -EINVAL;
    uint32_t per = (uint32_t)(x->max_pages * PAGE_SIZE / dev->sector_size);
    int r = 0;
    mutex_lock(&x->io_lock);
    while (count && r == 0) {
        uint32_t n = MIN(count, per);
        size_t bytes = (size_t)n * dev->sector_size;
        if (write)
            bounce_copy(x, buf, bytes, true);
        struct sqe cmd = {
            .opcode = write ? NVM_WRITE : NVM_READ,
            .nsid = ns->nsid,
            .cdw10 = (uint32_t)sector,
            .cdw11 = (uint32_t)(sector >> 32),
            .cdw12 = n - 1,
        };
        set_prps(x, &cmd, bytes);
        r = submit(x, &x->io, &cmd, NULL, IO_TIMEOUT_MS);
        if (r == 0 && !write)
            bounce_copy(x, buf, bytes, false);
        buf = (uint8_t *)buf + bytes;
        sector += n;
        count -= n;
    }
    mutex_unlock(&x->io_lock);
    return r;
}

static int nvme_flush(struct blockdev *dev)
{
    struct nvme_ns *ns = dev->priv;
    struct nvme *x = ns->ctrl;
    if (!x->volatile_cache)
        return 0;
    struct sqe cmd = { .opcode = NVM_FLUSH, .nsid = ns->nsid };
    mutex_lock(&x->io_lock);
    int r = submit(x, &x->io, &cmd, NULL, IO_TIMEOUT_MS);
    mutex_unlock(&x->io_lock);
    return r;
}

static int queue_alloc(struct queue *q, unsigned qid)
{
    q->qid = qid;
    q->sq = pmm_alloc_dma_page(&q->sq_phys);
    q->cq = pmm_alloc_dma_page(&q->cq_phys);
    q->sq_tail = q->cq_head = 0;
    q->phase = 1;
    return q->sq && q->cq ? 0 : -ENOMEM;
}

/* Wait until CSTS.RDY equals ready, for at most the timeout of CAP.TO. */
static int wait_ready(struct nvme *x, bool ready, unsigned timeout_ms)
{
    for (unsigned t = 0; t <= timeout_ms; t++) {
        uint32_t csts = rd32(x, REG_CSTS);
        if (csts == 0xffffffffu)
            return -ENODEV;
        if (!!(csts & CSTS_RDY) == ready)
            return 0;
        sleep_ms(1);
    }
    return -ETIMEDOUT;
}

/* The interrupt of the controller: MSI-X entry 0, else MSI, else none,
 * and the waiters poll. Both completion queues use vector 0. The kernel
 * option nvme=poll selects polling, which the QEMU nvme device cannot
 * otherwise show, since it always offers MSI-X. */
static void setup_interrupt(struct nvme *x)
{
    x->irq = -1;
    x->irq_kind = "no";
    char opt[8];
    bool poll = cmdline_lookup("nvme", opt, sizeof opt) && strcmp(opt, "poll") == 0;
    int irq = poll ? -1 : irq_alloc();
    if (irq >= 0) {
        irq_register((unsigned)irq, nvme_irq, x);
        if (pci_msix_enable(x->pci) == 0 && pci_msix_set_vector(x->pci, 0, (unsigned)irq) == 0) {
            x->irq = irq;
            x->irq_kind = "msi-x";
        } else if (pci_msi_enable(x->pci, (unsigned)irq) == 0) {
            x->irq = irq;
            x->irq_kind = "msi";
        }
    }
    /* Without MSI-X or MSI the pin interrupt is masked, since nothing
     * handles it. */
    if (x->irq < 0)
        wr32(x, REG_INTMS, 0xffffffffu);
}

/* Copy an identify string without its trailing blanks. */
static void copy_string(char *dst, const uint8_t *src, size_t len)
{
    memcpy(dst, src, len);
    dst[len] = '\0';
    while (len && (dst[len - 1] == ' ' || dst[len - 1] == '\0'))
        dst[--len] = '\0';
}

static void add_namespace(struct nvme *x, uint32_t nsid)
{
    if (x->nns == MAX_NAMESPACES || admin_identify(x, nsid, CNS_NAMESPACE) < 0)
        return;
    const uint8_t *id = x->bounce[0];
    uint64_t nsze;
    memcpy(&nsze, id, 8);
    unsigned flbas = id[26] & 0xf;
    uint32_t lbaf;
    memcpy(&lbaf, id + 128 + 4 * flbas, 4);
    unsigned ms = lbaf & 0xffff, lbads = (lbaf >> 16) & 0xff;
    if (!nsze)
        return;
    if (ms || lbads < 9 || lbads > PAGE_SHIFT) {
        klog_warn("nvme%u: namespace %u: format with %u byte blocks and %u metadata bytes not supported",
                  x->index, nsid, lbads < 32 ? 1u << lbads : 0, ms);
        return;
    }
    struct nvme_ns *ns = kzalloc(sizeof *ns);
    if (!ns)
        return;
    ns->ctrl = x;
    ns->nsid = nsid;
    ksnprintf(ns->bdev.name, sizeof ns->bdev.name, "nvme%un%u", x->index, nsid);
    ns->bdev.sector_size = 1u << lbads;
    ns->bdev.nsectors = nsze;
    ns->bdev.rw = nvme_rw;
    ns->bdev.flush = nvme_flush;
    ns->bdev.priv = ns;
    if (blockdev_register(&ns->bdev) < 0) {
        kfree(ns);
        return;
    }
    x->ns[x->nns++] = ns;
    klog_info("%s: %lu sectors of %u bytes (%lu MiB)", ns->bdev.name, nsze, ns->bdev.sector_size,
              (nsze << lbads) >> 20);
}

/* The active namespaces: the list of identify CNS 2 since version 1.1,
 * else every identifier up to the number of namespaces. */
static void add_namespaces(struct nvme *x)
{
    uint32_t list[MAX_NAMESPACES];
    unsigned n = 0;
    if (x->version >= 0x10100 && admin_identify(x, 0, CNS_ACTIVE_NS) == 0) {
        const uint32_t *ids = (const uint32_t *)x->bounce[0];
        while (n < MAX_NAMESPACES && ids[n])
            n++;
        memcpy(list, ids, n * sizeof *list);
    } else {
        for (uint32_t id = 1; id <= x->nn && n < MAX_NAMESPACES; id++)
            list[n++] = id;
    }
    for (unsigned i = 0; i < n; i++)
        add_namespace(x, list[i]);
}

static int create_io_queues(struct nvme *x)
{
    struct sqe cmd = { .opcode = ADM_SET_FEATURES, .cdw10 = FEAT_NUM_QUEUES, .cdw11 = 0 };
    uint32_t result;
    int r = submit(x, &x->admin, &cmd, &result, ADMIN_TIMEOUT_MS);
    if (r < 0)
        return r;
    /* The queue is published under lock once it is initialized, since the
     * interrupt handler reads x->io. */
    struct queue q;
    if ((r = queue_alloc(&q, 1)) < 0)
        return r;
    spin_lock(&x->lock);
    x->io = q;
    spin_unlock(&x->lock);
    cmd = (struct sqe){
        .opcode = ADM_CREATE_CQ,
        .prp1 = x->io.cq_phys,
        .cdw10 = (QUEUE_ENTRIES - 1u) << 16 | 1u,
        .cdw11 = 1u | (x->irq >= 0 ? 2u : 0u),    /* physically contiguous, interrupts on vector 0 */
    };
    if ((r = submit(x, &x->admin, &cmd, NULL, ADMIN_TIMEOUT_MS)) < 0)
        return r;
    cmd = (struct sqe){
        .opcode = ADM_CREATE_SQ,
        .prp1 = x->io.sq_phys,
        .cdw10 = (QUEUE_ENTRIES - 1u) << 16 | 1u,
        .cdw11 = 1u << 16 | 1u,                     /* completion queue 1, physically contiguous */
    };
    return submit(x, &x->admin, &cmd, NULL, ADMIN_TIMEOUT_MS);
}

static struct nvme *nvme_start(struct pci_dev *p, unsigned index)
{
    uint64_t size = pci_bar_size(p, 0);
    if (size < 0x2000) {
        klog_error("%02x:%02x.%u: no register BAR", p->bus, p->slot, p->func);
        return NULL;
    }
    struct nvme *x = kzalloc(sizeof *x);
    if (!x)
        return NULL;
    x->pci = p;
    x->index = index;
    mutex_init(&x->io_lock, "nvme_io");
    spinlock_init(&x->lock, "nvme");
    waitq_init(&x->waitq, "nvme");
    pci_enable_bus_master(p);
    x->regs = vmm_map_mmio(p->bar[0], ALIGN_UP(size, PAGE_SIZE), VM_KERNEL_RW | VM_NOCACHE);
    if (!x->regs)
        goto fail;
    uint64_t cap = rd64(x, REG_CAP);
    unsigned mqes = (unsigned)(cap & 0xffff) + 1;
    unsigned timeout_ms = (unsigned)((cap >> 24) & 0xff) * 500 + 500;
    x->stride = 4u << ((cap >> 32) & 0xf);
    x->version = rd32(x, REG_VS);
    if (!(cap & (1ULL << 37)) || ((cap >> 48) & 0xf) != 0 || mqes < QUEUE_ENTRIES) {
        klog_error("%02x:%02x.%u: controller without the NVM command set, 4 KiB pages or %u queue entries",
                   p->bus, p->slot, p->func, QUEUE_ENTRIES);
        goto fail;
    }

    /* Disable the controller, give it the admin queues and enable it. */
    if (rd32(x, REG_CC) & CC_EN)
        wr32(x, REG_CC, 0);
    if (wait_ready(x, false, timeout_ms) < 0) {
        klog_error("%02x:%02x.%u: the controller does not stop", p->bus, p->slot, p->func);
        goto fail;
    }
    if (queue_alloc(&x->admin, 0) < 0)
        goto fail;
    for (unsigned i = 0; i < BOUNCE_PAGES; i++)
        if (!(x->bounce[i] = pmm_alloc_dma_page(&x->bounce_phys[i])))
            goto fail;
    if (!(x->prp_list = pmm_alloc_dma_page(&x->prp_phys)))
        goto fail;
    x->max_pages = BOUNCE_PAGES;
    wr32(x, REG_AQA, (QUEUE_ENTRIES - 1u) << 16 | (QUEUE_ENTRIES - 1u));
    wr64(x, REG_ASQ, x->admin.sq_phys);
    wr64(x, REG_ACQ, x->admin.cq_phys);
    setup_interrupt(x);
    wr32(x, REG_CC, CC_EN | CC_IOSQES | CC_IOCQES);
    if (wait_ready(x, true, timeout_ms) < 0 || (rd32(x, REG_CSTS) & CSTS_CFS)) {
        klog_error("%02x:%02x.%u: the controller does not start (status 0x%x)", p->bus, p->slot, p->func,
                   rd32(x, REG_CSTS));
        goto fail;
    }

    mutex_lock(&x->io_lock);
    if (admin_identify(x, 0, CNS_CONTROLLER) < 0) {
        mutex_unlock(&x->io_lock);
        goto fail;
    }
    const uint8_t *id = x->bounce[0];
    copy_string(x->serial, id + 4, 20);
    copy_string(x->model, id + 24, 40);
    copy_string(x->firmware, id + 64, 8);
    unsigned mdts = id[77];
    if (mdts && mdts < 16 && (1u << mdts) < x->max_pages)
        x->max_pages = 1u << mdts;
    memcpy(&x->nn, id + 516, 4);
    x->volatile_cache = id[525] & 1;
    klog_info("%02x:%02x.%u: nvme %u.%u, %s interrupt, %s (serial %s, firmware %s), %u namespaces", p->bus,
              p->slot, p->func, x->version >> 16, (x->version >> 8) & 0xff, x->irq_kind, x->model, x->serial,
              x->firmware, x->nn);
    if (create_io_queues(x) < 0) {
        mutex_unlock(&x->io_lock);
        klog_error("nvme%u: cannot create the I/O queues", index);
        goto fail;
    }
    add_namespaces(x);
    mutex_unlock(&x->io_lock);
    return x;
fail:
    /* The pages given to a controller that failed are not returned: the
     * controller may still own some of them. */
    return NULL;
}

void nvme_init(void)
{
    for (size_t i = 0; i < pci_count() && ncontrollers < MAX_CONTROLLERS; i++) {
        struct pci_dev *p = pci_device(i);
        if (p->class != 0x01 || p->subclass != 0x08 || p->prog_if != 0x02)
            continue;
        struct nvme *x = nvme_start(p, ncontrollers);
        if (x) {
            controllers[ncontrollers++] = x;
            p->driver = "nvme";
        }
    }
}

/* ---- /dev/devices (docs/design/sysinfo.md) ---- */

void nvme_describe(struct devinfo *d)
{
    if (!ncontrollers)
        return;
    devinfo_node(d, "nvme", "NVMe");
    devinfo_prop(d, "controllers", "%u", ncontrollers);
    for (unsigned i = 0; i < ncontrollers; i++) {
        struct nvme *x = controllers[i];
        const struct pci_dev *p = x->pci;
        char path[32];
        ksnprintf(path, sizeof path, "nvme/nvme%u", i);
        devinfo_node(d, path, "%s", x->model[0] ? x->model : "NVMe controller");
        devinfo_prop(d, "pci_address", "%02x:%02x.%u", p->bus, p->slot, p->func);
        devinfo_prop(d, "version", "%u.%u", x->version >> 16, (x->version >> 8) & 0xff);
        devinfo_prop(d, "model", "%s", x->model);
        devinfo_prop(d, "serial", "%s", x->serial);
        devinfo_prop(d, "firmware", "%s", x->firmware);
        devinfo_prop(d, "interrupt", "%s", x->irq_kind);
        devinfo_prop(d, "largest_transfer", "%u KiB", x->max_pages * (unsigned)(PAGE_SIZE / 1024));
        devinfo_prop(d, "volatile_write_cache", "%s", x->volatile_cache ? "yes" : "no");
        devinfo_prop(d, "namespaces", "%u", x->nns);
        for (unsigned n = 0; n < x->nns; n++)
            devinfo_prop(d, x->ns[n]->bdev.name, "%lu sectors of %u bytes", x->ns[n]->bdev.nsectors,
                         x->ns[n]->bdev.sector_size);
    }
}
