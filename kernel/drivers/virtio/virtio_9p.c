/* virtio-9p (V5 of docs/plan/release-0.6.0.md, docs/design/9p.md).
 *
 * The device has one request queue. A request is a chain of two
 * descriptors: the message for the host and the buffer for the answer.
 * The head descriptor of the chain identifies the request in the queue,
 * and its index is also the 9P tag of the message. The tags of the
 * pending requests therefore differ. The completion marks the request
 * done and wakes the waiters of the queue. Each waiter checks its own
 * request. */
#define KLOG_SUBSYS "9p"
#include <drivers/virtio/virtio_9p.h>
#include <drivers/virtio/virtio.h>
#include <drivers/pci.h>
#include <fs/vfs.h>
#include <fs/devfs.h>
#include <lib/printf.h>
#include <lib/string.h>
#include <mm/memlayout.h>
#include <mm/slab.h>
#include <klog.h>
#include <errno.h>

#define VIRTIO_9P_DEVICE_TRANSITIONAL 0x1009
#define VIRTIO_9P_DEVICE_MODERN       0x1049
#define VIRTIO_9P_F_MOUNT_TAG         (1ULL << 0)
#define P9_TVERSION 100
#define MAX_CHANNELS 8
#define TAG_MAX 31      /* the longest name of a devfs node */

/* The configuration space: the length of the tag and the tag without a
 * NUL. */
struct virtio_9p_config {
    uint16_t tag_len;
    uint8_t tag[];
} __packed;

/* One pending request. done and len are written by the completion under
 * the lock of the queue. */
struct p9_request {
    bool done;
    uint32_t len;
};

struct p9_channel {
    struct virtio_dev vdev;
    struct virtqueue *vq;
    char tag[TAG_MAX + 1];
    /* Counters, protected by the lock of the queue. */
    uint64_t requests;
    unsigned pending, most_pending;
};

/* The channels, written at boot by virtio_9p_init and read only
 * afterwards. */
static struct p9_channel *channels[MAX_CHANNELS];
static unsigned nchannels;

static void request_complete(struct virtqueue *vq, uint16_t head, uint32_t len)
{
    struct p9_request *r = vq->cookie[head];
    if (r) {
        r->len = len;
        r->done = true;
    }
}

long virtio_9p_request(struct p9_channel *ch, void *tbuf, size_t tlen, void *rbuf, size_t rcap)
{
    struct virtqueue *vq = ch->vq;
    struct p9_request r = { false, 0 };
    uint16_t ids[2];
    spin_lock(&vq->lock);
    while (virtq_alloc_chain(vq, 2, ids) < 0 && !vq->broken)
        waitq_wait(&vq->waitq, &vq->lock);
    if (vq->broken) {
        spin_unlock(&vq->lock);
        return -EIO;
    }
    uint8_t *t = tbuf;
    if (t[4] != P9_TVERSION) {
        t[5] = (uint8_t)ids[0];
        t[6] = (uint8_t)(ids[0] >> 8);
    }
    vq->desc[ids[0]].addr = virt_to_phys(tbuf);
    vq->desc[ids[0]].len = (uint32_t)tlen;
    vq->desc[ids[1]].addr = virt_to_phys(rbuf);
    vq->desc[ids[1]].len = (uint32_t)rcap;
    vq->desc[ids[1]].flags |= VIRTQ_DESC_F_WRITE;
    virtq_submit(vq, ids[0], &r);
    ch->requests++;
    if (++ch->pending > ch->most_pending)
        ch->most_pending = ch->pending;
    while (!r.done && !vq->broken)
        waitq_wait(&vq->waitq, &vq->lock);
    ch->pending--;
    spin_unlock(&vq->lock);
    if (!r.done) {
        klog_error("%s: the request queue failed", ch->tag);
        return -EIO;
    }
    return (long)r.len;
}

struct p9_channel *virtio_9p_find(const char *tag)
{
    for (unsigned i = 0; i < nchannels; i++)
        if (strcmp(channels[i]->tag, tag) == 0)
            return channels[i];
    return NULL;
}

/* /dev/9p/TAG reports the tag, the number of requests and the most
 * requests that were pending at once. */
static long node_read(struct file *f, char *buf, size_t n, uint64_t *pos)
{
    struct p9_channel *ch = f->inode->priv;
    spin_lock(&ch->vq->lock);
    uint64_t requests = ch->requests;
    unsigned most = ch->most_pending;
    spin_unlock(&ch->vq->lock);
    char text[TAG_MAX + 80];
    int len = ksnprintf(text, sizeof text, "tag %s\nrequests %lu\nmost_pending %u\n", ch->tag,
                        (unsigned long)requests, most);
    if (*pos >= (uint64_t)len)
        return 0;
    size_t avail = (size_t)len - *pos;
    if (n > avail)
        n = avail;
    memcpy(buf, text + *pos, n);
    *pos += n;
    return (long)n;
}

static const struct file_ops node_fops = {
    .read = node_read,
};

static void probe(struct pci_dev *pci)
{
    struct p9_channel *ch = kzalloc(sizeof *ch);
    if (!ch)
        return;
    if (virtio_pci_setup(pci, &ch->vdev) < 0 || !ch->vdev.device_cfg ||
        virtio_negotiate(&ch->vdev, VIRTIO_9P_F_MOUNT_TAG) < 0 || !(ch->vdev.features & VIRTIO_9P_F_MOUNT_TAG))
        goto fail;
    volatile struct virtio_9p_config *cfg = (volatile struct virtio_9p_config *)ch->vdev.device_cfg;
    size_t len = cfg->tag_len;
    if (len == 0 || len > TAG_MAX || len + 2 > ch->vdev.device_cfg_len) {
        klog_error("invalid mount tag length %zu", len);
        goto fail;
    }
    for (size_t i = 0; i < len; i++)
        ch->tag[i] = (char)cfg->tag[i];
    if (strchr(ch->tag, '/') || virtio_9p_find(ch->tag)) {
        klog_error("mount tag %s is a duplicate or contains a slash", ch->tag);
        goto fail;
    }
    ch->vq = virtio_queue_setup(&ch->vdev, 0, request_complete);
    if (!ch->vq || virtio_start(&ch->vdev) < 0)
        goto fail;
    pci->driver = "virtio-9p";
    channels[nchannels++] = ch;
    if (nchannels == 1)
        devfs_register("9p", S_IFDIR | 0755, NULL, NULL, 0);
    char node[TAG_MAX + 4];
    ksnprintf(node, sizeof node, "9p/%s", ch->tag);
    devfs_register(node, S_IFCHR | 0444, &node_fops, ch, 0);
    klog_info("mount tag %s, queue of %u entries, vector %u", ch->tag, ch->vq->size, ch->vdev.vector);
    return;
fail:
    /* ch remains allocated. The interrupt handler of a started device
     * refers to it. */
    klog_error("%02x:%02x.%u: initialization failed", pci->bus, pci->slot, pci->func);
    virtio_reset(&ch->vdev);
}

void virtio_9p_init(void)
{
    for (size_t i = 0; i < pci_count() && nchannels < MAX_CHANNELS; i++) {
        struct pci_dev *p = pci_device(i);
        if (p->vendor == VIRTIO_VENDOR &&
            (p->device == VIRTIO_9P_DEVICE_MODERN || p->device == VIRTIO_9P_DEVICE_TRANSITIONAL))
            probe(p);
    }
}
