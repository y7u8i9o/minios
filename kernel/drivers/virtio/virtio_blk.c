#define KLOG_SUBSYS "virtio-blk"
#include <drivers/virtio/virtio_blk.h>
#include <drivers/virtio/virtio.h>
#include <drivers/pci.h>
#include <block/blockdev.h>
#include <mm/memlayout.h>
#include <mm/slab.h>
#include <lib/string.h>
#include <lib/printf.h>
#include <kassert.h>
#include <klog.h>
#include <errno.h>

#define VIRTIO_BLK_DEVICE_TRANSITIONAL 0x1001
#define VIRTIO_BLK_DEVICE_MODERN       0x1042

#define VIRTIO_BLK_T_IN    0
#define VIRTIO_BLK_T_OUT   1
#define VIRTIO_BLK_T_FLUSH 4
#define VIRTIO_BLK_S_OK    0

#define VIRTIO_BLK_F_FLUSH (1ULL << 9)

struct virtio_blk_req {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} __packed;

/* One in flight request. done and status are set by the completion
 * callback under vq->lock. */
struct blk_request {
    struct virtio_blk_req hdr;
    uint8_t status;
    bool done;
};

struct virtio_blk {
    struct virtio_dev vdev;
    struct virtqueue *vq;
    struct blockdev bdev;
    bool has_flush;
};

static int ndisks;

static void blk_complete(struct virtqueue *vq, uint16_t head, uint32_t len)
{
    struct blk_request *req = vq->cookie[head];
    if (req)
        req->done = true;
}

/* Submit one request and wait for its completion. buf may be NULL for a
 * flush. */
static int blk_do_request(struct virtio_blk *d, uint32_t type, uint64_t sector,
                          void *buf, size_t len)
{
    struct blk_request *req = kzalloc(sizeof *req);
    if (!req)
        return -ENOMEM;
    req->hdr.type = type;
    req->hdr.sector = sector;
    req->status = 0xff;
    struct virtqueue *vq = d->vq;
    unsigned n = buf ? 3 : 2;
    uint16_t ids[3];

    spin_lock(&vq->lock);
    while (virtq_alloc_chain(vq, n, ids) < 0)
        waitq_wait_bounded(&vq->waitq, &vq->lock);
    vq->desc[ids[0]].addr = virt_to_phys(&req->hdr);
    vq->desc[ids[0]].len = sizeof req->hdr;
    unsigned last = 1;
    if (buf) {
        vq->desc[ids[1]].addr = virt_to_phys(buf);
        vq->desc[ids[1]].len = (uint32_t)len;
        if (type == VIRTIO_BLK_T_IN)
            vq->desc[ids[1]].flags |= VIRTQ_DESC_F_WRITE;
        last = 2;
    }
    vq->desc[ids[last]].addr = virt_to_phys(&req->status);
    vq->desc[ids[last]].len = 1;
    vq->desc[ids[last]].flags |= VIRTQ_DESC_F_WRITE;
    virtq_submit(vq, ids[0], req);
    while (!req->done)
        waitq_wait_bounded(&vq->waitq, &vq->lock);
    spin_unlock(&vq->lock);

    int r = req->status == VIRTIO_BLK_S_OK ? 0 : -EIO;
    kfree(req);
    return r;
}

/* Data buffers must be physically contiguous. kmalloc blocks are (small
 * ones come from one slab page, large ones from one buddy block), so the
 * transfer is bounced through one in chunks of up to 128 KiB, halving the
 * chunk when a large block is not available. */
#define MAX_CHUNK (128 * 1024)

static int blk_rw(struct blockdev *bdev, uint64_t sector, uint32_t count, void *buf, bool write)
{
    struct virtio_blk *d = bdev->priv;
    if (sector + count > bdev->nsectors || count == 0)
        return -EINVAL;
    uint8_t *p = buf;
    size_t chunk = MAX_CHUNK;
    while (count) {
        uint32_t n = MIN(count, chunk / bdev->sector_size);
        size_t len = (size_t)n * bdev->sector_size;
        void *bounce = kmalloc(len);
        if (!bounce) {
            if (chunk > 4096) {
                chunk /= 2;
                continue;
            }
            return -ENOMEM;
        }
        if (write)
            memcpy(bounce, p, len);
        int r = blk_do_request(d, write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN, sector, bounce, len);
        if (r == 0 && !write)
            memcpy(p, bounce, len);
        kfree(bounce);
        if (r < 0)
            return r;
        sector += n;
        count -= n;
        p += len;
    }
    return 0;
}

static int blk_flush(struct blockdev *bdev)
{
    struct virtio_blk *d = bdev->priv;
    if (!d->has_flush)
        return 0;
    return blk_do_request(d, VIRTIO_BLK_T_FLUSH, 0, NULL, 0);
}

static void probe(struct pci_dev *pci)
{
    struct virtio_blk *d = kzalloc(sizeof *d);
    if (!d)
        return;
    if (virtio_pci_setup(pci, &d->vdev) < 0)
        goto fail;
    if (virtio_negotiate(&d->vdev, VIRTIO_BLK_F_FLUSH) < 0)
        goto fail;
    d->has_flush = d->vdev.features & VIRTIO_BLK_F_FLUSH;
    d->vq = virtio_queue_setup(&d->vdev, 0, blk_complete);
    if (!d->vq) {
        klog_error("cannot set up request queue");
        goto fail;
    }
    if (virtio_start(&d->vdev) < 0)
        goto fail;
    volatile uint32_t *cfg = (volatile uint32_t *)d->vdev.device_cfg;
    uint64_t capacity = cfg[0] | ((uint64_t)cfg[1] << 32);
    ksnprintf(d->bdev.name, sizeof d->bdev.name, "vd%c", 'a' + ndisks);
    d->bdev.sector_size = 512;
    d->bdev.nsectors = capacity;
    d->bdev.rw = blk_rw;
    d->bdev.flush = blk_flush;
    d->bdev.priv = d;
    if (blockdev_register(&d->bdev) < 0)
        goto fail;
    ndisks++;
    klog_info("%s: %lu sectors (%lu MiB), queue %u, vector %u, flush %d", d->bdev.name,
              capacity, capacity / 2048, d->vq->size, d->vdev.vector, d->has_flush);
    return;
fail:
    klog_error("%02x:%02x.%u: initialization failed", pci->bus, pci->slot, pci->func);
    kfree(d);
}

void virtio_blk_init(void)
{
    for (size_t i = 0; i < pci_count(); i++) {
        struct pci_dev *p = pci_device(i);
        if (p->vendor == VIRTIO_VENDOR &&
            (p->device == VIRTIO_BLK_DEVICE_TRANSITIONAL || p->device == VIRTIO_BLK_DEVICE_MODERN))
            probe(p);
    }
    if (!ndisks)
        klog_warn("no virtio-blk device");
}
