#define KLOG_SUBSYS "virtio"
#include <drivers/virtio/virtio.h>
#include <drivers/pci.h>
#include <arch/irq.h>
#include <arch/barrier.h>
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <mm/memlayout.h>
#include <mm/slab.h>
#include <lib/string.h>
#include <kassert.h>
#include <klog.h>
#include <errno.h>
#include <drivers/timer.h>

#define VIRTIO_PCI_CAP_COMMON 1
#define VIRTIO_PCI_CAP_NOTIFY 2
#define VIRTIO_PCI_CAP_ISR    3
#define VIRTIO_PCI_CAP_DEVICE 4


static volatile void *map_cap(struct pci_dev *pci, uint8_t cap)
{
    uint8_t bar = pci_read8(pci, (uint8_t)(cap + 4));
    uint32_t off = pci_read32(pci, (uint8_t)(cap + 8));
    uint32_t len = pci_read32(pci, (uint8_t)(cap + 12));
    if (bar > 5 || pci->bar_is_io[bar] || !pci->bar[bar])
        return NULL;
    uint64_t pa = pci->bar[bar] + off;
    size_t size = ALIGN_UP((pa & (PAGE_SIZE - 1)) + len, PAGE_SIZE);
    volatile uint8_t *va = vmm_map_mmio(pa & PAGE_MASK, size, VM_READ | VM_WRITE | VM_NOCACHE);
    return va ? va + (pa & (PAGE_SIZE - 1)) : NULL;
}

int virtio_pci_setup(struct pci_dev *pci, struct virtio_dev *dev)
{
    memset(dev, 0, sizeof *dev);
    dev->pci = pci;
    spinlock_init(&dev->irq_lock, "virtio_irq");
    uint8_t off = pci_read8(pci, 0x34) & 0xfc;
    for (int guard = 0; off && guard < 48; guard++) {
        if (pci_read8(pci, off) == PCI_CAP_VENDOR) {
            uint8_t type = pci_read8(pci, (uint8_t)(off + 3));
            switch (type) {
            case VIRTIO_PCI_CAP_COMMON:
                dev->common = map_cap(pci, off);
                break;
            case VIRTIO_PCI_CAP_NOTIFY:
                dev->notify_base = map_cap(pci, off);
                dev->notify_multiplier = pci_read32(pci, (uint8_t)(off + 16));
                break;
            case VIRTIO_PCI_CAP_ISR:
                dev->isr = map_cap(pci, off);
                break;
            case VIRTIO_PCI_CAP_DEVICE:
                dev->device_cfg = map_cap(pci, off);
                dev->device_cfg_len = pci_read32(pci, (uint8_t)(off + 12));
                break;
            }
        }
        off = pci_read8(pci, (uint8_t)(off + 1)) & 0xfc;
    }
    if (!dev->common || !dev->notify_base || !dev->isr) {
        klog_error("%02x:%02x.%u lacks modern capabilities", pci->bus, pci->slot, pci->func);
        return -ENODEV;
    }
    pci_enable_bus_master(pci);
    return 0;
}

int virtio_negotiate(struct virtio_dev *dev, uint64_t wanted)
{
    volatile struct virtio_pci_common_cfg *c = dev->common;
    c->device_status = 0;
    while (c->device_status != 0)
        ;
    c->device_status = VIRTIO_STATUS_ACK;
    c->device_status |= VIRTIO_STATUS_DRIVER;

    c->device_feature_select = 0;
    uint64_t features = c->device_feature;
    c->device_feature_select = 1;
    features |= (uint64_t)c->device_feature << 32;
    if (!(features & VIRTIO_F_VERSION_1)) {
        klog_error("device lacks VIRTIO_F_VERSION_1");
        c->device_status |= VIRTIO_STATUS_FAILED;
        return -ENODEV;
    }
    dev->features = features & (wanted | VIRTIO_F_VERSION_1);
    c->driver_feature_select = 0;
    c->driver_feature = (uint32_t)dev->features;
    c->driver_feature_select = 1;
    c->driver_feature = (uint32_t)(dev->features >> 32);
    c->device_status |= VIRTIO_STATUS_FEATURES_OK;
    if (!(c->device_status & VIRTIO_STATUS_FEATURES_OK)) {
        klog_error("device rejected features %lx", dev->features);
        c->device_status |= VIRTIO_STATUS_FAILED;
        return -ENODEV;
    }
    dev->nqueues = c->num_queues;
    return 0;
}

/* Drain the used ring. The caller has acquired vq->lock. Returns true if any
 * chain completed. */
static bool virtq_drain_locked(struct virtqueue *vq)
{
    bool completed = false;
    mb();
    for (;;) {
        if (vq->broken)
            return completed;
        /* The device writes the entries before the index. The entries up
         * to the index are read after it (rmb): otherwise a weakly
         * ordered CPU may read an entry of the previous lap (A8). */
        uint16_t idx = vq->used->idx;
        rmb();
        if (idx == vq->last_used)
            return completed;
        if ((uint16_t)(idx - vq->last_used) > vq->size) {
            vq->broken = true;
            vq->bad_used++;
            return completed;
        }
        while (vq->last_used != idx) {
            completed = true;
            struct virtq_used_elem e = vq->used->ring[vq->last_used % vq->size];
            vq->last_used++;
            if (e.id >= vq->size || !vq->active[e.id]) {
                vq->broken = true;
                vq->bad_used++;
                break;
            }
            vq->active[e.id] = false;
            if (vq->complete)
                vq->complete(vq, (uint16_t)e.id, e.len);
            virtq_free_chain(vq, (uint16_t)e.id);
        }
    }
}

void virtq_poll_locked(struct virtqueue *vq)
{
    virtq_drain_locked(vq);
}

static void virtio_irq(struct trapframe *tf, void *arg)
{
    struct virtio_dev *dev = arg;
    spin_lock(&dev->irq_lock);
    for (unsigned i = 0; i < dev->nqueues && i < ARRAY_SIZE(dev->queues); i++) {
        struct virtqueue *vq = dev->queues[i];
        if (!vq)
            continue;
        spin_lock(&vq->lock);
        bool completed = virtq_drain_locked(vq);
        waitq_wake_all(&vq->waitq);
        spin_unlock(&vq->lock);
        if (completed)
            poll_source_notify(&vq->poll);
    }
    if (dev->work_notify)
        dev->work_notify();
    spin_unlock(&dev->irq_lock);
}

struct virtqueue *virtio_queue_setup(struct virtio_dev *dev, uint16_t index,
                                     void (*complete)(struct virtqueue *, uint16_t, uint32_t))
{
    volatile struct virtio_pci_common_cfg *c = dev->common;
    if (index >= dev->nqueues || index >= ARRAY_SIZE(dev->queues))
        return NULL;
    c->queue_select = index;
    uint16_t size = c->queue_size;
    if (size == 0)
        return NULL;
    if (size > VIRTQ_MAX_SIZE)
        size = VIRTQ_MAX_SIZE;

    struct virtqueue *vq = kzalloc(sizeof *vq);
    if (!vq)
        return NULL;
    /* Descriptors and avail ring in the first page, used ring in the
     * second. Two pages contain a queue of up to 128 entries. */
    vq->order = 1;
    struct page *pg = pmm_alloc(vq->order);
    if (!pg) {
        kfree(vq);
        return NULL;
    }
    vq->phys = page_to_phys(pg);
    uint8_t *base = phys_to_virt(vq->phys);
    memset(base, 0, PAGE_SIZE << vq->order);
    vq->dev = dev;
    vq->index = index;
    vq->size = size;
    vq->desc = (struct virtq_desc *)base;
    vq->avail = (struct virtq_avail *)(base + size * sizeof(struct virtq_desc));
    vq->used = (struct virtq_used *)(base + PAGE_SIZE);
    vq->complete = complete;
    spinlock_init(&vq->lock, "virtqueue");
    waitq_init(&vq->waitq, "virtqueue");
    poll_source_init(&vq->poll, "virtqueue_poll");
    for (uint16_t i = 0; i < size; i++)
        vq->desc[i].next = (uint16_t)(i + 1);
    vq->free_head = 0;
    vq->num_free = size;

    c->queue_size = size;
    c->queue_desc = vq->phys;
    c->queue_driver = vq->phys + (uintptr_t)((uint8_t *)vq->avail - base);
    c->queue_device = vq->phys + PAGE_SIZE;
    uint16_t notify_off = c->queue_notify_off;
    vq->notify = (volatile uint16_t *)(dev->notify_base + (uint32_t)notify_off * dev->notify_multiplier);
    c->queue_enable = 1;
    dev->queues[index] = vq;
    return vq;
}

int virtio_start(struct virtio_dev *dev)
{
    volatile struct virtio_pci_common_cfg *c = dev->common;
    int irq = irq_alloc();
    if (irq < 0)
        return irq;
    dev->vector = (unsigned)irq;
    irq_register(dev->vector, virtio_irq, dev);
    int r = pci_msix_enable(dev->pci);
    if (r < 0)
        return r;
    r = pci_msix_set_vector(dev->pci, 0, dev->vector);
    if (r < 0)
        return r;
    c->msix_config = 0;
    for (unsigned i = 0; i < ARRAY_SIZE(dev->queues); i++) {
        if (!dev->queues[i])
            continue;
        c->queue_select = (uint16_t)i;
        c->queue_msix_vector = 0;
        if (c->queue_msix_vector != 0) {
            klog_error("queue %u refused msix vector", i);
            return -EIO;
        }
    }
    c->device_status |= VIRTIO_STATUS_DRIVER_OK;
    return 0;
}

int virtq_alloc_chain(struct virtqueue *vq, unsigned n, uint16_t *ids)
{
    kassert(spin_locked_by_current(&vq->lock));
    if (vq->broken || !n || n > vq->num_free)
        return -ENOSPC;
    for (unsigned i = 0; i < n; i++) {
        ids[i] = vq->free_head;
        vq->free_head = vq->desc[vq->free_head].next;
        vq->num_free--;
    }
    for (unsigned i = 0; i < n; i++) {
        vq->desc[ids[i]].flags = i + 1 < n ? VIRTQ_DESC_F_NEXT : 0;
        vq->desc[ids[i]].next = i + 1 < n ? ids[i + 1] : 0;
    }
    return ids[0];
}

void virtq_free_chain(struct virtqueue *vq, uint16_t head)
{
    kassert(spin_locked_by_current(&vq->lock));
    uint16_t i = head;
    for (;;) {
        uint16_t next = vq->desc[i].next;
        bool more = vq->desc[i].flags & VIRTQ_DESC_F_NEXT;
        vq->desc[i].next = vq->free_head;
        vq->free_head = i;
        vq->num_free++;
        if (!more)
            break;
        i = next;
    }
    vq->cookie[head] = NULL;
    vq->active[head] = false;
}

void virtq_submit(struct virtqueue *vq, uint16_t head, void *cookie)
{
    kassert(spin_locked_by_current(&vq->lock));
    kassert(head < vq->size && !vq->active[head] && !vq->broken);
    vq->active[head] = true;
    vq->cookie[head] = cookie;
    vq->avail->ring[vq->avail->idx % vq->size] = head;
    mb();
    vq->avail->idx++;
    mb();
    *vq->notify = vq->index;
}

int virtio_reset(struct virtio_dev *dev)
{
    if (!dev->common)
        return 0;
    dev->common->device_status = 0;
    uint64_t deadline = timer_ms() + 100;
    while (dev->common->device_status != 0) {
        if (timer_ms() >= deadline)
            return -ETIMEDOUT;
        cpu_relax();
    }
    /* Device reset ends DMA. Serialize with any ISR already traversing
     * the rings before detaching them. The static device remains valid. */
    struct virtqueue *old[4];
    spin_lock(&dev->irq_lock);
    dev->work_notify = NULL;
    for (unsigned i = 0; i < ARRAY_SIZE(old); i++) {
        old[i] = dev->queues[i];
        dev->queues[i] = NULL;
    }
    spin_unlock(&dev->irq_lock);
    for (unsigned i = 0; i < ARRAY_SIZE(old); i++) {
        if (!old[i])
            continue;
        pmm_free(phys_to_page(old[i]->phys), old[i]->order);
        kfree(old[i]);
    }
    return 0;
}
