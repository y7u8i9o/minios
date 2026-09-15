/* The entropy device is used once at boot. Short completions are accumulated,
 * no IRQ callback allocates or generates random values, and reset precedes
 * every release of ring/DMA memory. The device object has static IRQ lifetime. */
#include <drivers/virtio/virtio_rng.h>
#include <drivers/virtio/virtio.h>
#include <drivers/pci.h>
#include <drivers/timer.h>
#include <mm/slab.h>
#include <mm/memlayout.h>
#include <lib/string.h>
#include <errno.h>

static struct virtio_dev device;
static unsigned requested, completed;
static bool done;
static void completion(struct virtqueue *queue, uint16_t head, uint32_t length)
{
    if (done || !length || length > requested || queue->cookie[head] != &device) {
        queue->broken = true;
        return;
    }
    completed = length;
    done = true;
}
int virtio_rng_seed(uint8_t output[64])
{
    struct pci_dev *pci = pci_find(VIRTIO_VENDOR, 0x1044);
    if (!pci)
        pci = pci_find(VIRTIO_VENDOR, 0x1005);
    if (!pci)
        return -ENODEV;
    uint8_t *buffer = NULL;
    int result = -EIO;
    if (virtio_pci_setup(pci, &device) < 0 || virtio_negotiate(&device, 0) < 0)
        goto stop;
    struct virtqueue *queue = virtio_queue_setup(&device, 0, completion);
    buffer = kzalloc(64);
    if (!queue || !buffer || virtio_start(&device) < 0)
        goto stop;
    unsigned received = 0;
    uint64_t deadline = timer_ms() + 1000;
    while (received < 64 && timer_ms() < deadline) {
        spin_lock(&queue->lock);
        uint16_t id;
        if (virtq_alloc_chain(queue, 1, &id) < 0) {
            spin_unlock(&queue->lock);
            goto stop;
        }
        requested = 64 - received;
        completed = 0;
        done = false;
        queue->desc[id].addr = V2P(buffer + received);
        queue->desc[id].len = requested;
        queue->desc[id].flags = VIRTQ_DESC_F_WRITE;
        virtq_submit(queue, id, &device);
        spin_unlock(&queue->lock);
        for (;;) {
            spin_lock(&queue->lock);
            virtq_poll_locked(queue);
            bool finished = done;
            bool broken = queue->broken;
            unsigned length = completed;
            spin_unlock(&queue->lock);
            if (broken)
                goto stop;
            if (finished) {
                received += length;
                break;
            }
            if (timer_ms() >= deadline)
                goto stop;
            sleep_ms(1);
        }
    }
    if (received == 64)
        result = 0;
stop:
    if (virtio_reset(&device) < 0)
        return -EIO; /* DMA and ring storage deliberately remain pinned. */
    if (!result)
        memcpy(output, buffer, 64);
    if (buffer) {
        for (unsigned i = 0; i < 64; i++)
            ((volatile uint8_t *)buffer)[i] = 0;
        kfree(buffer);
    }
    return result;
}
