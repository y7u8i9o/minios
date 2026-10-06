#define KLOG_SUBSYS "virtio-gpu"
#include <drivers/virtio/virtio_gpu.h>
#include <drivers/virtio/virtio.h>
#include <drivers/fbdev.h>
#include <drivers/pci.h>
#include <boot.h>
#include <drivers/timer.h>
#include <arch/barrier.h>
#include <console.h>
#include <mm/memlayout.h>
#include <mm/pmm.h>
#include <mm/slab.h>
#include <sched/thread.h>
#include <sched/wait.h>
#include <sync/mutex.h>
#include <lib/string.h>
#include <sync/atomic.h>
#include <klog.h>
#include <errno.h>
#include <drivers/devinfo.h>

#define VIRTIO_GPU_DEVICE_MODERN 0x1050

#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO        0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D      0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF          0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT             0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH          0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D     0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106
#define VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING 0x0107
#define VIRTIO_GPU_RESP_OK_NODATA              0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO        0x1101
#define VIRTIO_GPU_RESP_ERR_UNSPEC             0x1200

#define VIRTIO_GPU_EVENT_DISPLAY 1

#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM 2     /* bytes B G R X: 0x00RRGGBB as a little endian word */
#define VIRTIO_GPU_MAX_SCANOUTS 16

/* One 16 MiB block (PMM_MAX_ORDER) backs every mode, so a mode change
 * leaves user mappings valid. */
#define GPU_BUFFER_ORDER PMM_MAX_ORDER
#define GPU_CTRL_MAX 512
#define GPU_FLUSH_MS 20

struct virtio_gpu_ctrl_hdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint8_t ring_idx;
    uint8_t padding[3];
} __packed;

struct virtio_gpu_rect {
    uint32_t x, y, width, height;
} __packed;

/* The device configuration space. */
struct virtio_gpu_config {
    uint32_t events_read;
    uint32_t events_clear;
    uint32_t num_scanouts;
    uint32_t num_capsets;
} __packed;

struct virtio_gpu_resp_display_info {
    struct virtio_gpu_ctrl_hdr hdr;
    struct {
        struct virtio_gpu_rect r;
        uint32_t enabled;
        uint32_t flags;
    } pmodes[VIRTIO_GPU_MAX_SCANOUTS];
} __packed;

struct virtio_gpu_resource_create_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} __packed;

struct virtio_gpu_resource_unref {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t padding;
} __packed;

struct virtio_gpu_set_scanout {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t scanout_id;
    uint32_t resource_id;
} __packed;

struct virtio_gpu_resource_flush {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t resource_id;
    uint32_t padding;
} __packed;

struct virtio_gpu_transfer_to_host_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
} __packed;

struct virtio_gpu_resource_attach_backing {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
    struct {
        uint64_t addr;
        uint32_t length;
        uint32_t padding;
    } entries[1];
} __packed;

struct gpu_ctrl {
    bool done;
    uint8_t request[GPU_CTRL_MAX];
    uint8_t response[GPU_CTRL_MAX];
};

struct virtio_gpu {
    struct virtio_dev vdev;
    struct virtqueue *ctrlq;
    struct mutex lock;              /* serializes control sequences (flushes, mode changes) */
    struct gpu_ctrl panic_ctrl;     /* used by flush_poll only */
    struct page *pages;
    void *buf;                      /* the scanout buffer in the direct map */
    uintptr_t buf_phys;
    size_t buf_size;
    /* Scanout state, protected by lock. */
    uint32_t resource;              /* resource on the scanout, 0 before the first commit */
    uint32_t pending;               /* resource created by prepare_mode */
    uint32_t width, height;         /* of resource */
    uint32_t pending_width, pending_height;
    uint32_t pref_width, pref_height;   /* what the host asked for */
    /* 1 when the interrupt handler saw a display event that gpu_flushd
     * has not handled. Atomic, set in the handler and taken by the
     * thread. */
    atomic_u32_t display_event;
};

static struct virtio_gpu *gpu;

/* One request of a flush batch: a transfer or a flush command and its
 * response header. */
struct gpu_batch_req {
    bool done;
    union {
        struct virtio_gpu_transfer_to_host_2d xfer;
        struct virtio_gpu_resource_flush flush;
    } req;
    struct virtio_gpu_ctrl_hdr rsp;
};

/* The cookie of every control request is its completion flag. */
static void ctrl_complete(struct virtqueue *vq, uint16_t head, uint32_t len)
{
    bool *done = vq->cookie[head];
    if (done)
        *done = true;
}

/* Send one control request and wait for the response. With poll the
 * call spins on the used ring instead of sleeping (panic path). */
static int ctrl_xfer(struct virtio_gpu *g, const void *request, size_t request_len,
                     void *response, size_t response_len, bool poll)
{
    if (request_len > GPU_CTRL_MAX || response_len > GPU_CTRL_MAX ||
        response_len < sizeof(struct virtio_gpu_ctrl_hdr))
        return -EINVAL;
    struct gpu_ctrl *x = poll ? &g->panic_ctrl : kzalloc(sizeof *x);
    if (!x)
        return -ENOMEM;
    x->done = false;
    memcpy(x->request, request, request_len);
    memset(x->response, 0, response_len);

    struct virtqueue *vq = g->ctrlq;
    uint16_t ids[2];
    if (poll && vq->lock.locked)
        return -EBUSY;          /* locked by a halted CPU or by us: cannot make progress */
    spin_lock(&vq->lock);
    while (virtq_alloc_chain(vq, 2, ids) < 0) {
        if (poll) {
            spin_unlock(&vq->lock);
            return -ENOSPC;
        }
        waitq_wait(&vq->waitq, &vq->lock);
    }
    vq->desc[ids[0]].addr = virt_to_phys(x->request);
    vq->desc[ids[0]].len = (uint32_t)request_len;
    vq->desc[ids[1]].addr = virt_to_phys(x->response);
    vq->desc[ids[1]].len = (uint32_t)response_len;
    vq->desc[ids[1]].flags |= VIRTQ_DESC_F_WRITE;
    virtq_submit(vq, ids[0], &x->done);
    if (poll) {
        for (unsigned i = 0; i < 20000000 && !x->done; i++) {
            virtq_poll_locked(vq);
            cpu_relax();
        }
    } else {
        while (!x->done)
            waitq_wait(&vq->waitq, &vq->lock);
    }
    spin_unlock(&vq->lock);
    if (!x->done)
        return -ETIMEDOUT;

    memcpy(response, x->response, response_len);
    uint32_t type = ((struct virtio_gpu_ctrl_hdr *)response)->type;
    if (!poll)
        kfree(x);
    if (type >= VIRTIO_GPU_RESP_ERR_UNSPEC) {
        klog_error("command %x failed: %x", ((const struct virtio_gpu_ctrl_hdr *)request)->type, type);
        return -EIO;
    }
    return 0;
}

static int simple_cmd(struct virtio_gpu *g, const void *req, size_t len, bool poll)
{
    struct virtio_gpu_ctrl_hdr rsp;
    return ctrl_xfer(g, req, len, &rsp, sizeof rsp, poll);
}

/* Publish the n requests of a batch, each len bytes long, notify the
 * device once and wait until every request completed. When the queue has
 * no free descriptors, the device first works through what is published.
 * Caller has acquired g->lock. Returns 0 or the first error. A broken
 * queue never completes its requests: the batch then remains allocated,
 * because the device may still write the responses. */
static int ctrl_batch(struct virtio_gpu *g, struct gpu_batch_req *b, int n, size_t len, bool *leak)
{
    struct virtqueue *vq = g->ctrlq;
    spin_lock(&vq->lock);
    for (int i = 0; i < n; i++) {
        uint16_t ids[2];
        while (virtq_alloc_chain(vq, 2, ids) < 0) {
            if (vq->broken) {
                spin_unlock(&vq->lock);
                *leak = true;
                return -EIO;
            }
            virtq_notify(vq);
            waitq_wait(&vq->waitq, &vq->lock);
        }
        b[i].done = false;
        vq->desc[ids[0]].addr = virt_to_phys(&b[i].req);
        vq->desc[ids[0]].len = (uint32_t)len;
        vq->desc[ids[1]].addr = virt_to_phys(&b[i].rsp);
        vq->desc[ids[1]].len = sizeof b[i].rsp;
        vq->desc[ids[1]].flags |= VIRTQ_DESC_F_WRITE;
        virtq_publish(vq, ids[0], &b[i].done);
    }
    virtq_notify(vq);
    for (int i = 0; i < n; i++)
        while (!b[i].done)
            waitq_wait(&vq->waitq, &vq->lock);
    spin_unlock(&vq->lock);
    for (int i = 0; i < n; i++)
        if (b[i].rsp.type >= VIRTIO_GPU_RESP_ERR_UNSPEC) {
            klog_error("command %x failed: %x", b[i].req.xfer.hdr.type, b[i].rsp.type);
            return -EIO;
        }
    return 0;
}

/* ---- fb_gpu_ops ---- */

static int gpu_prepare_mode(void *priv, uint32_t width, uint32_t height)
{
    struct virtio_gpu *g = priv;
    if ((uint64_t)width * height * 4 > g->buf_size)
        return -EINVAL;
    mutex_lock(&g->lock);
    uint32_t id = g->resource == 1 ? 2 : 1;
    struct virtio_gpu_resource_create_2d create = {
        .hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D,
        .resource_id = id,
        .format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM,
        .width = width,
        .height = height,
    };
    int r = simple_cmd(g, &create, sizeof create, false);
    if (r < 0)
        goto out;
    struct virtio_gpu_resource_attach_backing attach = {
        .hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING,
        .resource_id = id,
        .nr_entries = 1,
        .entries[0].addr = g->buf_phys,
        .entries[0].length = width * height * 4,
    };
    r = simple_cmd(g, &attach, sizeof attach, false);
    if (r < 0) {
        struct virtio_gpu_resource_unref unref = { .hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF, .resource_id = id };
        simple_cmd(g, &unref, sizeof unref, false);
        goto out;
    }
    g->pending = id;
    g->pending_width = width;
    g->pending_height = height;
out:
    mutex_unlock(&g->lock);
    return r;
}

static int gpu_commit_mode(void *priv)
{
    struct virtio_gpu *g = priv;
    mutex_lock(&g->lock);
    if (!g->pending) {
        mutex_unlock(&g->lock);
        return -EINVAL;
    }
    struct virtio_gpu_set_scanout scanout = {
        .hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT,
        .r = { 0, 0, g->pending_width, g->pending_height },
        .scanout_id = 0,
        .resource_id = g->pending,
    };
    int r = simple_cmd(g, &scanout, sizeof scanout, false);
    if (r == 0) {
        if (g->resource) {
            struct virtio_gpu_resource_unref unref = {
                .hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF, .resource_id = g->resource,
            };
            simple_cmd(g, &unref, sizeof unref, false);
        }
        g->resource = g->pending;
        g->width = g->pending_width;
        g->height = g->pending_height;
    }
    g->pending = 0;
    mutex_unlock(&g->lock);
    return r;
}

/* The part of r inside the resource, false when nothing remains. */
static bool clip_rect(const struct virtio_gpu *g, struct fb_rect r, struct virtio_gpu_rect *out)
{
    int32_t x0 = MAX(r.x, 0), y0 = MAX(r.y, 0);
    int32_t x1 = MIN(r.x + r.w, (int32_t)g->width), y1 = MIN(r.y + r.h, (int32_t)g->height);
    if (x0 >= x1 || y0 >= y1)
        return false;
    *out = (struct virtio_gpu_rect){ (uint32_t)x0, (uint32_t)y0, (uint32_t)(x1 - x0), (uint32_t)(y1 - y0) };
    return true;
}

/* Transfer a rectangle of the buffer to the host and show it, without
 * interrupts (panic path). */
static int flush_poll_locked(struct virtio_gpu *g, struct fb_rect r)
{
    struct virtio_gpu_rect rect;
    if (!g->resource)
        return -ENODEV;
    if (!clip_rect(g, r, &rect))
        return 0;
    struct virtio_gpu_transfer_to_host_2d xfer = {
        .hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D,
        .r = rect,
        .offset = (uint64_t)rect.y * g->width * 4 + (uint64_t)rect.x * 4,
        .resource_id = g->resource,
    };
    int err = simple_cmd(g, &xfer, sizeof xfer, true);
    if (err < 0)
        return err;
    struct virtio_gpu_resource_flush flush = {
        .hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH,
        .r = rect,
        .resource_id = g->resource,
    };
    return simple_cmd(g, &flush, sizeof flush, true);
}

/* Transfer the rectangles of the buffer to the host and show them: all
 * transfers in one batch, then all flushes in a second one, so that the
 * host shows only transferred pixels and the thread waits twice per call
 * whatever the number of rectangles. Caller has acquired g->lock. */
static int flush_locked(struct virtio_gpu *g, const struct fb_rect *r, int n)
{
    if (!g->resource)
        return -ENODEV;
    if (n > FB_FLUSH_MAX)
        return -EINVAL;
    struct virtio_gpu_rect rects[FB_FLUSH_MAX];
    int m = 0;
    for (int i = 0; i < n; i++)
        if (clip_rect(g, r[i], &rects[m]))
            m++;
    if (!m)
        return 0;
    struct gpu_batch_req *b = kzalloc((size_t)m * sizeof *b);
    if (!b)
        return -ENOMEM;
    for (int i = 0; i < m; i++)
        b[i].req.xfer = (struct virtio_gpu_transfer_to_host_2d){
            .hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D,
            .r = rects[i],
            .offset = (uint64_t)rects[i].y * g->width * 4 + (uint64_t)rects[i].x * 4,
            .resource_id = g->resource,
        };
    bool leak = false;
    int err = ctrl_batch(g, b, m, sizeof b[0].req.xfer, &leak);
    if (err == 0) {
        for (int i = 0; i < m; i++)
            b[i].req.flush = (struct virtio_gpu_resource_flush){
                .hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH,
                .r = rects[i],
                .resource_id = g->resource,
            };
        err = ctrl_batch(g, b, m, sizeof b[0].req.flush, &leak);
    }
    if (!leak)
        kfree(b);
    return err;
}

static int gpu_flush(void *priv, const struct fb_rect *r, int n)
{
    struct virtio_gpu *g = priv;
    mutex_lock(&g->lock);
    int err = flush_locked(g, r, n);
    mutex_unlock(&g->lock);
    return err;
}

static void gpu_flush_poll(void *priv, struct fb_rect r)
{
    flush_poll_locked(priv, r);
}

static void gpu_describe(void *priv, struct devinfo *d)
{
    struct virtio_gpu *g = priv;
    mutex_lock(&g->lock);
    devinfo_prop(d, "gpu_scanout", "%u x %u, resource %u", g->width, g->height, g->resource);
    devinfo_prop(d, "gpu_preferred_mode", "%u x %u", g->pref_width, g->pref_height);
    devinfo_size(d, "gpu_buffer", g->buf_size);
    mutex_unlock(&g->lock);
    devinfo_prop(d, "gpu_pci_address", "%02x:%02x.%u", g->vdev.pci->bus, g->vdev.pci->slot, g->vdev.pci->func);
}

static const struct fb_gpu_ops gpu_ops = {
    .prepare_mode = gpu_prepare_mode,
    .commit_mode = gpu_commit_mode,
    .flush = gpu_flush,
    .flush_poll = gpu_flush_poll,
    .describe = gpu_describe,
};

/* The host changed the configuration space. The interrupt handler clears
 * a display event and leaves the control requests to gpu_flushd. */
static void gpu_config_changed(struct virtio_dev *dev)
{
    struct virtio_gpu *g = container_of(dev, struct virtio_gpu, vdev);
    volatile struct virtio_gpu_config *cfg = (volatile struct virtio_gpu_config *)dev->device_cfg;
    uint32_t events = cfg->events_read;
    if (!(events & VIRTIO_GPU_EVENT_DISPLAY))
        return;
    cfg->events_clear = VIRTIO_GPU_EVENT_DISPLAY;
    atomic_u32_store_release(&g->display_event, 1);
}

/* Reads the display information after a display event and reports the
 * preferred size of the first scanout. */
static void display_changed(struct virtio_gpu *g)
{
    struct virtio_gpu_ctrl_hdr req = { .type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO };
    struct virtio_gpu_resp_display_info info;
    mutex_lock(&g->lock);
    int r = ctrl_xfer(g, &req, sizeof req, &info, sizeof info, false);
    bool ok = r == 0 && info.hdr.type == VIRTIO_GPU_RESP_OK_DISPLAY_INFO && info.pmodes[0].enabled;
    if (ok) {
        g->pref_width = info.pmodes[0].r.width;
        g->pref_height = info.pmodes[0].r.height;
    }
    uint32_t w = g->pref_width, h = g->pref_height;
    mutex_unlock(&g->lock);
    if (!ok) {
        klog_warn("display event without an enabled scanout");
        return;
    }
    fb_display_changed(w, h);
}

/* The console draws into the buffer under console_lock and cannot call
 * the device; this thread pushes what it drew. The thread also handles
 * the display events of the host. */
static void gpu_flushd(void *arg)
{
    struct virtio_gpu *g = arg;
    for (;;) {
        sleep_ms(GPU_FLUSH_MS);
        if (atomic_u32_exchange_acq_rel(&g->display_event, 0))
            display_changed(g);
        struct fb_rect r;
        if (console_take_dirty(&r))
            gpu_flush(g, &r, 1);
    }
}

bool virtio_gpu_present(void)
{
    return gpu != NULL;
}

static void probe(struct pci_dev *pci)
{
    struct virtio_gpu *g = kzalloc(sizeof *g);
    if (!g)
        return;
    if (virtio_pci_setup(pci, &g->vdev) < 0 || virtio_negotiate(&g->vdev, 0) < 0)
        goto fail;
    g->vdev.config_changed = gpu_config_changed;
    g->ctrlq = virtio_queue_setup(&g->vdev, 0, ctrl_complete);
    if (!g->ctrlq) {
        klog_error("cannot set up the control queue");
        goto fail;
    }
    if (virtio_start(&g->vdev) < 0)
        goto fail;
    pci->driver = "virtio-gpu";
    mutex_init(&g->lock, "virtio_gpu");

    struct virtio_gpu_ctrl_hdr info_req = { .type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO };
    struct virtio_gpu_resp_display_info info;
    if (ctrl_xfer(g, &info_req, sizeof info_req, &info, sizeof info, false) == 0 &&
        info.hdr.type == VIRTIO_GPU_RESP_OK_DISPLAY_INFO && info.pmodes[0].enabled) {
        g->pref_width = info.pmodes[0].r.width;
        g->pref_height = info.pmodes[0].r.height;
    }

    g->pages = pmm_alloc(GPU_BUFFER_ORDER);
    if (!g->pages) {
        klog_error("cannot allocate the %u MiB scanout buffer", (1u << GPU_BUFFER_ORDER) >> 8);
        return;             /* live IRQ refers to g: it remains allocated */
    }
    g->buf_phys = page_to_phys(g->pages);
    g->buf = phys_to_virt(g->buf_phys);
    g->buf_size = (size_t)PAGE_SIZE << GPU_BUFFER_ORDER;
    /* User mappings take and drop a reference per page; the driver's own
     * reference retains every page of the block allocated permanently. */
    for (size_t off = 0; off < g->buf_size; off += PAGE_SIZE)
        page_get(phys_to_page(g->buf_phys + off));

    /* The mode asked for with video=, else the boot mode set by Limine,
     * else what the host prefers (no VGA framebuffer at all). */
    uint32_t w = fb_screen_present ? (uint32_t)fb_screen.width : g->pref_width;
    uint32_t h = fb_screen_present ? (uint32_t)fb_screen.height : g->pref_height;
    if (bootinfo.fb_req_width && (uint64_t)bootinfo.fb_req_width * bootinfo.fb_req_height * 4 <= g->buf_size) {
        w = bootinfo.fb_req_width;
        h = bootinfo.fb_req_height;
    }
    if (w < 320 || h < 200 || (uint64_t)w * h * 4 > g->buf_size) {
        w = 1024;
        h = 768;
    }
    if (gpu_prepare_mode(g, w, h) < 0) {
        klog_error("cannot create the scanout resource");
        return;
    }
    struct limine_framebuffer screen = {
        .address = g->buf,
        .width = w,
        .height = h,
        .pitch = (uint64_t)w * 4,
        .bpp = 32,
        .memory_model = LIMINE_FRAMEBUFFER_RGB,
        .red_mask_size = 8, .red_mask_shift = 16,
        .green_mask_size = 8, .green_mask_shift = 8,
        .blue_mask_size = 8, .blue_mask_shift = 0,
    };
    gpu = g;
    fb_gpu_register(&gpu_ops, g, &screen, g->buf_size);
    if (!thread_create("gpu_flushd", gpu_flushd, g, 0))
        klog_error("cannot start the flush thread");
    klog_info("%ux%u scanout in a %zu MiB buffer, host prefers %ux%u, vector %u", w, h,
              g->buf_size >> 20, g->pref_width, g->pref_height, g->vdev.vector);
    return;
fail:
    klog_error("%02x:%02x.%u: initialization failed", pci->bus, pci->slot, pci->func);
    kfree(g);
}

void virtio_gpu_init(void)
{
    for (size_t i = 0; i < pci_count() && !gpu; i++) {
        struct pci_dev *p = pci_device(i);
        if (p->vendor == VIRTIO_VENDOR && p->device == VIRTIO_GPU_DEVICE_MODERN)
            probe(p);
    }
    if (!gpu)
        klog_info("no device, the boot framebuffer remains in use");
}
