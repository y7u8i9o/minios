/* virtio-balloon (V4 of docs/plan/release-0.6.0.md, docs/design/balloon.md).
 *
 * The host writes the target size of the balloon in pages to num_pages of
 * the configuration space and raises the configuration interrupt. The
 * thread "balloon" moves the size towards the target in requests of up to
 * 256 pages. Inflation allocates free pages, sends their frame numbers on
 * the inflate queue and removes the pages from the total of the
 * allocator. Deflation sends the frame numbers on the deflate queue and
 * frees the pages. After each request the driver writes the size to
 * actual.
 *
 * The statistics queue contains one buffer. The host returns the buffer
 * when it wants new values. The thread fills the buffer again and adds it
 * to the queue.
 *
 * With VIRTIO_BALLOON_F_DEFLATE_ON_OOM the driver is the pressure source of
 * the allocator (pmm_set_pressure_source). release_pressure frees pages
 * of the balloon at once. QEMU does not offer
 * VIRTIO_BALLOON_F_MUST_TELL_HOST, so the driver may use the pages before
 * the host learns of the deflation. The thread reports the released pages
 * on the deflate queue afterwards. A release under pressure also stops the
 * inflation until the host sets a new target. Otherwise the balloon would
 * take again the pages that the system needs. */
#define KLOG_SUBSYS "balloon"
#include <drivers/virtio/virtio_balloon.h>
#include <drivers/virtio/virtio.h>
#include <drivers/pci.h>
#include <drivers/timer.h>
#include <fs/vfs.h>
#include <fs/devfs.h>
#include <lib/printf.h>
#include <lib/string.h>
#include <mm/filemap.h>
#include <mm/memlayout.h>
#include <mm/pmm.h>
#include <mm/slab.h>
#include <mm/swap.h>
#include <mm/vma.h>
#include <sched/thread.h>
#include <sched/wait.h>
#include <klog.h>
#include <errno.h>

#define VIRTIO_BALLOON_DEVICE_TRANSITIONAL 0x1002
#define VIRTIO_BALLOON_DEVICE_MODERN       0x1045

#define VIRTIO_BALLOON_F_STATS_VQ       (1ULL << 1)
#define VIRTIO_BALLOON_F_DEFLATE_ON_OOM (1ULL << 2)

#define VIRTIO_BALLOON_S_SWAP_IN  0
#define VIRTIO_BALLOON_S_SWAP_OUT 1
#define VIRTIO_BALLOON_S_MAJFLT   2
#define VIRTIO_BALLOON_S_MINFLT   3
#define VIRTIO_BALLOON_S_MEMFREE  4
#define VIRTIO_BALLOON_S_MEMTOT   5
#define VIRTIO_BALLOON_S_AVAIL    6
#define VIRTIO_BALLOON_S_CACHES   7
#define NSTATS 8

/* Frame numbers per request. One request is a buffer of 1 KiB. */
#define PFNS_PER_REQUEST 256
/* Inflation leaves this many pages free (4 MiB). */
#define INFLATE_RESERVE 1024
/* A release under pressure frees at least this many pages (1 MiB). */
#define PRESSURE_MIN 256
/* Frame numbers of released pages that wait for their report. */
#define REPORT_MAX 1024

/* The configuration space. The device writes num_pages, the driver
 * writes actual. */
struct virtio_balloon_config {
    uint32_t num_pages;
    uint32_t actual;
} __packed;

struct virtio_balloon_stat {
    uint16_t tag;
    uint64_t val;
} __packed;

/* One request on the inflate or deflate queue. done is set by the
 * completion under the lock of the queue. */
struct balloon_request {
    uint32_t pfns[PFNS_PER_REQUEST];
    bool done;
};

struct balloon {
    struct virtio_dev vdev;
    struct virtqueue *inflateq, *deflateq, *statsq;
    struct thread *thread;
    bool deflate_on_oom;
    /* lock protects pages, size, target, report, nreport, unreported,
     * released, inflation_stopped, stats_wanted, stats_updates and
     * target_changed. It is the condition lock of work. The thread never
     * allocates memory and never takes the lock of a queue while it has
     * acquired lock. lock -> pmm_lock. */
    struct spinlock lock;
    struct waitq work;
    struct list_head pages;         /* the pages of the balloon, through page.lru */
    uint64_t size, target;
    uint32_t report[REPORT_MAX];    /* released under pressure, not yet reported */
    unsigned nreport;
    uint64_t unreported;            /* released while report was full */
    uint64_t released;              /* released under pressure since boot */
    bool inflation_stopped;         /* a release under pressure stopped the inflation */
    bool target_changed;
    bool stats_wanted;
    uint64_t stats_updates;
    /* Used by the thread only. */
    struct balloon_request request;
    struct virtio_balloon_stat stats[NSTATS];
};

static struct balloon *balloon;

static volatile struct virtio_balloon_config *config(struct balloon *b)
{
    return (volatile struct virtio_balloon_config *)b->vdev.device_cfg;
}

/* ---- completions and the configuration interrupt ---- */

static void request_complete(struct virtqueue *vq, uint16_t head, uint32_t len)
{
    struct balloon_request *r = vq->cookie[head];
    if (r)
        r->done = true;
}

/* The host took the statistics buffer and wants new values. */
static void stats_complete(struct virtqueue *vq, uint16_t head, uint32_t len)
{
    struct balloon *b = balloon;
    if (!b)
        return;
    spin_lock(&b->lock);
    b->stats_wanted = true;
    waitq_wake_all(&b->work);
    spin_unlock(&b->lock);
}

static void config_changed(struct virtio_dev *dev)
{
    struct balloon *b = container_of(dev, struct balloon, vdev);
    uint32_t target = config(b)->num_pages;
    spin_lock(&b->lock);
    if (target != b->target) {
        b->target = target;
        b->inflation_stopped = false;
    }
    b->target_changed = true;
    waitq_wake_all(&b->work);
    spin_unlock(&b->lock);
}

/* ---- the pressure source ---- */

static size_t release_pressure(size_t want)
{
    struct balloon *b = balloon;
    if (!b || !b->deflate_on_oom || thread_current() == b->thread)
        return 0;
    if (want < PRESSURE_MIN)
        want = PRESSURE_MIN;
    size_t n = 0;
    spin_lock(&b->lock);
    while (n < want && !list_empty(&b->pages)) {
        struct page *pg = list_first_entry(&b->pages, struct page, lru);
        list_del(&pg->lru);
        if (b->nreport < REPORT_MAX)
            b->report[b->nreport++] = (uint32_t)page_to_pfn(pg);
        else
            b->unreported++;
        pmm_free(pg, 0);
        n++;
    }
    if (n) {
        b->size -= n;
        b->released += n;
        b->inflation_stopped = true;
        pmm_adjust_total((int64_t)n);
        waitq_wake_all(&b->work);
    }
    spin_unlock(&b->lock);
    return n;
}

/* ---- requests ---- */

/* Sends count frame numbers of b->request on vq and waits for the host. */
static int send_pfns(struct balloon *b, struct virtqueue *vq, unsigned count)
{
    struct balloon_request *r = &b->request;
    uint16_t id;
    spin_lock(&vq->lock);
    while (virtq_alloc_chain(vq, 1, &id) < 0)
        waitq_wait(&vq->waitq, &vq->lock);
    r->done = false;
    vq->desc[id].addr = virt_to_phys(r->pfns);
    vq->desc[id].len = count * (uint32_t)sizeof r->pfns[0];
    vq->desc[id].flags = 0;
    virtq_submit(vq, id, r);
    while (!r->done && !vq->broken)
        waitq_wait(&vq->waitq, &vq->lock);
    bool broken = vq->broken;
    spin_unlock(&vq->lock);
    return broken ? -EIO : 0;
}

static void write_actual(struct balloon *b)
{
    spin_lock(&b->lock);
    uint64_t size = b->size;
    spin_unlock(&b->lock);
    config(b)->actual = (uint32_t)size;
}

/* Inflates by up to want pages. Returns the number of pages added. */
static unsigned inflate(struct balloon *b, uint64_t want)
{
    struct page *pages[PFNS_PER_REQUEST];
    unsigned n = 0;
    while (n < want && n < PFNS_PER_REQUEST) {
        struct pmm_stats st;
        pmm_get_stats(&st);
        if (st.free_pages <= INFLATE_RESERVE)
            break;
        struct page *pg = pmm_alloc(0);
        if (!pg)
            break;
        pages[n] = pg;
        b->request.pfns[n] = (uint32_t)page_to_pfn(pg);
        n++;
    }
    if (n == 0)
        return 0;
    if (send_pfns(b, b->inflateq, n) < 0) {
        for (unsigned i = 0; i < n; i++)
            pmm_free(pages[i], 0);
        return 0;
    }
    spin_lock(&b->lock);
    for (unsigned i = 0; i < n; i++)
        list_add_tail(&pages[i]->lru, &b->pages);
    b->size += n;
    spin_unlock(&b->lock);
    pmm_adjust_total(-(int64_t)n);
    return n;
}

/* Deflates by up to want pages. Returns the number of pages freed. */
static unsigned deflate(struct balloon *b, uint64_t want)
{
    struct page *pages[PFNS_PER_REQUEST];
    unsigned n = 0;
    spin_lock(&b->lock);
    while (n < want && n < PFNS_PER_REQUEST && !list_empty(&b->pages)) {
        struct page *pg = list_last_entry(&b->pages, struct page, lru);
        list_del(&pg->lru);
        pages[n] = pg;
        b->request.pfns[n] = (uint32_t)page_to_pfn(pg);
        n++;
    }
    spin_unlock(&b->lock);
    if (n == 0)
        return 0;
    /* The pages are no longer in the list, so the pressure source cannot
     * release them twice. A failed request returns them to the list. */
    if (send_pfns(b, b->deflateq, n) < 0) {
        spin_lock(&b->lock);
        for (unsigned i = 0; i < n; i++)
            list_add_tail(&pages[i]->lru, &b->pages);
        spin_unlock(&b->lock);
        return 0;
    }
    for (unsigned i = 0; i < n; i++)
        pmm_free(pages[i], 0);
    spin_lock(&b->lock);
    b->size -= n;
    spin_unlock(&b->lock);
    pmm_adjust_total((int64_t)n);
    return n;
}

/* Reports the pages that the pressure source released. */
static void report_released(struct balloon *b)
{
    spin_lock(&b->lock);
    unsigned n = b->nreport < PFNS_PER_REQUEST ? b->nreport : PFNS_PER_REQUEST;
    memcpy(b->request.pfns, b->report + b->nreport - n, n * sizeof b->report[0]);
    b->nreport -= n;
    spin_unlock(&b->lock);
    if (n && send_pfns(b, b->deflateq, n) < 0)
        klog_warn("the report of %u released pages failed", n);
}

/* ---- statistics ---- */

static void put_stat(struct balloon *b, unsigned i, uint16_t tag, uint64_t value)
{
    b->stats[i].tag = tag;
    b->stats[i].val = value;
}

static void fill_stats(struct balloon *b)
{
    struct pmm_stats st;
    struct swap_stats ss;
    struct filemap_stats fs;
    uint64_t minor, major;
    pmm_get_stats(&st);
    swap_get_stats(&ss);
    filemap_get_stats(&fs);
    vma_get_fault_counts(&minor, &major);
    put_stat(b, 0, VIRTIO_BALLOON_S_SWAP_IN, ss.swapped_in * PAGE_SIZE);
    put_stat(b, 1, VIRTIO_BALLOON_S_SWAP_OUT, ss.swapped_out * PAGE_SIZE);
    put_stat(b, 2, VIRTIO_BALLOON_S_MAJFLT, major);
    put_stat(b, 3, VIRTIO_BALLOON_S_MINFLT, minor);
    put_stat(b, 4, VIRTIO_BALLOON_S_MEMFREE, st.free_pages * PAGE_SIZE);
    put_stat(b, 5, VIRTIO_BALLOON_S_MEMTOT, st.total_pages * PAGE_SIZE);
    /* Free memory and the file pages that a reclaim could drop. */
    put_stat(b, 6, VIRTIO_BALLOON_S_AVAIL, (st.free_pages + fs.cached_pages) * PAGE_SIZE);
    put_stat(b, 7, VIRTIO_BALLOON_S_CACHES, fs.cached_pages * PAGE_SIZE);
}

/* Adds the statistics buffer to its queue. The host returns it with its
 * next request. */
static void send_stats(struct balloon *b)
{
    struct virtqueue *vq = b->statsq;
    fill_stats(b);
    uint16_t id;
    spin_lock(&vq->lock);
    if (virtq_alloc_chain(vq, 1, &id) == 0) {
        vq->desc[id].addr = virt_to_phys(b->stats);
        vq->desc[id].len = sizeof b->stats;
        vq->desc[id].flags = 0;
        virtq_submit(vq, id, b->stats);
    }
    spin_unlock(&vq->lock);
}

/* ---- the thread ---- */

static void balloon_thread(void *arg)
{
    struct balloon *b = arg;
    bool stalled = false;
    for (;;) {
        spin_lock(&b->lock);
        bool stats = b->stats_wanted;
        b->stats_wanted = false;
        bool changed = b->target_changed;
        b->target_changed = false;
        bool report = b->nreport > 0;
        uint64_t unreported = b->unreported;
        b->unreported = 0;
        uint64_t size = b->size, target = b->target;
        bool stopped = b->inflation_stopped;
        spin_unlock(&b->lock);
        if (stats) {
            send_stats(b);
            spin_lock(&b->lock);
            b->stats_updates++;
            spin_unlock(&b->lock);
        }
        if (unreported)
            klog_warn("%lu pages released under pressure without a report", (unsigned long)unreported);
        if (report) {
            report_released(b);
            write_actual(b);
            continue;
        }
        unsigned moved = 0;
        if (size < target && !stopped) {
            moved = inflate(b, target - size);
            if (!moved && !stalled)
                klog_warn("inflation stopped at %lu of %lu pages, free memory is low", (unsigned long)size,
                          (unsigned long)target);
            stalled = !moved;
        } else if (size > target) {
            moved = deflate(b, size - target);
        }
        if (moved) {
            write_actual(b);
            spin_lock(&b->lock);
            bool reached = b->size == b->target;
            uint64_t now = b->size;
            spin_unlock(&b->lock);
            if (reached)
                klog_info("%lu pages, the target", (unsigned long)now);
            continue;
        }
        if (changed)
            write_actual(b);
        /* Wait for work. A stalled inflation tries again after a second. */
        spin_lock(&b->lock);
        while (!b->stats_wanted && !b->target_changed && b->nreport == 0 && !(stalled && !b->inflation_stopped))
            waitq_wait(&b->work, &b->lock);
        if (stalled && !b->stats_wanted && !b->target_changed && b->nreport == 0)
            waitq_wait_timeout(&b->work, &b->lock, timer_ms() + 1000);
        spin_unlock(&b->lock);
    }
}

/* ---- /dev/balloon ---- */

void virtio_balloon_get_info(struct balloon_info *out)
{
    struct balloon *b = balloon;
    memset(out, 0, sizeof *out);
    if (!b)
        return;
    out->present = true;
    out->deflate_on_oom = b->deflate_on_oom;
    spin_lock(&b->lock);
    out->size = b->size;
    out->target = b->target;
    out->released_on_pressure = b->released;
    out->stats_updates = b->stats_updates;
    spin_unlock(&b->lock);
}

static long balloon_read(struct file *f, char *buf, size_t n, uint64_t *pos)
{
    struct balloon_info info;
    virtio_balloon_get_info(&info);
    char text[256];
    int len = ksnprintf(text, sizeof text,
                        "BalloonSize: %lu kB\nBalloonTarget: %lu kB\nDeflateOnOOM: %s\nReleasedOnPressure: %lu kB\n"
                        "StatsUpdates: %lu\n",
                        (unsigned long)(info.size * 4), (unsigned long)(info.target * 4),
                        info.deflate_on_oom ? "yes" : "no", (unsigned long)(info.released_on_pressure * 4),
                        (unsigned long)info.stats_updates);
    if (*pos >= (uint64_t)len)
        return 0;
    size_t avail = (size_t)len - *pos;
    if (n > avail)
        n = avail;
    memcpy(buf, text + *pos, n);
    *pos += n;
    return (long)n;
}

static const struct file_ops balloon_fops = {
    .read = balloon_read,
};

/* ---- probe ---- */

static void probe(struct pci_dev *pci)
{
    struct balloon *b = kzalloc(sizeof *b);
    if (!b)
        return;
    spinlock_init(&b->lock, "balloon");
    waitq_init(&b->work, "balloon");
    list_init(&b->pages);
    if (virtio_pci_setup(pci, &b->vdev) < 0 || !b->vdev.device_cfg ||
        virtio_negotiate(&b->vdev, VIRTIO_BALLOON_F_STATS_VQ | VIRTIO_BALLOON_F_DEFLATE_ON_OOM) < 0)
        goto fail;
    b->vdev.config_changed = config_changed;
    b->deflate_on_oom = (b->vdev.features & VIRTIO_BALLOON_F_DEFLATE_ON_OOM) != 0;
    b->inflateq = virtio_queue_setup(&b->vdev, 0, request_complete);
    b->deflateq = virtio_queue_setup(&b->vdev, 1, request_complete);
    if (b->vdev.features & VIRTIO_BALLOON_F_STATS_VQ)
        b->statsq = virtio_queue_setup(&b->vdev, 2, stats_complete);
    if (!b->inflateq || !b->deflateq || ((b->vdev.features & VIRTIO_BALLOON_F_STATS_VQ) && !b->statsq)) {
        klog_error("cannot set up the queues");
        goto fail;
    }
    balloon = b;
    if (virtio_start(&b->vdev) < 0)
        goto fail;
    pci->driver = "virtio-balloon";
    b->target = config(b)->num_pages;
    config(b)->actual = 0;
    /* The first statistics buffer tells the host that the driver supports
     * statistics. */
    if (b->statsq)
        send_stats(b);
    b->thread = thread_create("balloon", balloon_thread, b, 0);
    if (!b->thread) {
        klog_error("cannot start the thread");
        return;         /* the interrupt handler refers to b: it remains allocated */
    }
    if (b->deflate_on_oom)
        pmm_set_pressure_source(release_pressure);
    devfs_register("balloon", S_IFCHR | 0444, &balloon_fops, NULL, 0);
    klog_info("target %lu pages, statistics %s, deflate on OOM %s, vector %u", (unsigned long)b->target,
              b->statsq ? "yes" : "no", b->deflate_on_oom ? "yes" : "no", b->vdev.vector);
    return;
fail:
    /* b remains allocated. The interrupt handler of a started device
     * refers to it. */
    klog_error("%02x:%02x.%u: initialization failed", pci->bus, pci->slot, pci->func);
    balloon = NULL;
    virtio_reset(&b->vdev);
}

void virtio_balloon_init(void)
{
    for (size_t i = 0; i < pci_count() && !balloon; i++) {
        struct pci_dev *p = pci_device(i);
        if (p->vendor == VIRTIO_VENDOR &&
            (p->device == VIRTIO_BALLOON_DEVICE_MODERN || p->device == VIRTIO_BALLOON_DEVICE_TRANSITIONAL))
            probe(p);
    }
}
