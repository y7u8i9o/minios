#pragma once
#include <kernel.h>
#include <sync/spinlock.h>
#include <sched/wait.h>
#include <ipc/poll.h>

struct pci_dev;

#define VIRTIO_VENDOR       0x1af4
#define VIRTIO_F_VERSION_1  (1ULL << 32)

#define VIRTIO_STATUS_ACK         1
#define VIRTIO_STATUS_DRIVER      2
#define VIRTIO_STATUS_DRIVER_OK   4
#define VIRTIO_STATUS_FEATURES_OK 8
#define VIRTIO_STATUS_FAILED      128

#define VIRTQ_DESC_F_NEXT  1
#define VIRTQ_DESC_F_WRITE 2

#define VIRTQ_MAX_SIZE 128

struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __packed;

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VIRTQ_MAX_SIZE];
    uint16_t used_event;
} __packed;

struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
} __packed;

struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[VIRTQ_MAX_SIZE];
    uint16_t avail_event;
} __packed;

/* Modern virtio PCI common configuration, memory mapped. */
struct virtio_pci_common_cfg {
    uint32_t device_feature_select;
    uint32_t device_feature;
    uint32_t driver_feature_select;
    uint32_t driver_feature;
    uint16_t msix_config;
    uint16_t num_queues;
    uint8_t device_status;
    uint8_t config_generation;
    uint16_t queue_select;
    uint16_t queue_size;
    uint16_t queue_msix_vector;
    uint16_t queue_enable;
    uint16_t queue_notify_off;
    uint64_t queue_desc;
    uint64_t queue_driver;
    uint64_t queue_device;
} __packed;

/* A split virtqueue. lock protects the descriptor free list, the avail
 * ring index and the used ring cursor; it is taken in the interrupt
 * handler and serves as condition lock for completion waits. */
struct virtqueue {
    struct virtio_dev *dev;
    uint16_t index;
    uint16_t size;
    struct virtq_desc *desc;
    struct virtq_avail *avail;
    struct virtq_used *used;
    uintptr_t phys;                 /* physical base of the ring pages */
    unsigned order;
    uint16_t free_head;
    uint16_t num_free;
    uint16_t last_used;
    bool active[VIRTQ_MAX_SIZE]; /* published heads, validated before callback */
    bool broken;
    uint64_t bad_used;
    void *cookie[VIRTQ_MAX_SIZE];   /* per head descriptor completion token */
    volatile uint16_t *notify;
    struct spinlock lock;
    struct waitq waitq;             /* waiters for completions */
    struct poll_source poll;        /* poll waiters for this queue only */
    void (*complete)(struct virtqueue *vq, uint16_t head, uint32_t len);
};

struct virtio_dev {
    struct pci_dev *pci;
    volatile struct virtio_pci_common_cfg *common;
    volatile uint8_t *notify_base;
    uint32_t notify_multiplier;
    volatile uint8_t *isr;
    volatile uint8_t *device_cfg;
    uint32_t device_cfg_len;
    struct spinlock irq_lock; /* IRQ traversal versus reset/detach */
    void (*work_notify)(void);
    uint8_t vector;
    uint64_t features;
    struct virtqueue *queues[4];
    uint16_t nqueues;
};

/* Map the capabilities of a modern virtio PCI function. */
int virtio_pci_setup(struct pci_dev *pci, struct virtio_dev *dev);
/* Reset, acknowledge and negotiate features. wanted is the feature mask
 * the driver accepts; VIRTIO_F_VERSION_1 is always required. */
int virtio_negotiate(struct virtio_dev *dev, uint64_t wanted);
/* Allocate and register queue index. */
struct virtqueue *virtio_queue_setup(struct virtio_dev *dev, uint16_t index,
                                     void (*complete)(struct virtqueue *, uint16_t, uint32_t));
/* Set DRIVER_OK and route the queue interrupts to the device vector. */
int virtio_start(struct virtio_dev *dev);
/* Stop DMA, then detach/free rings. Driver reclaims its cookies afterwards.
 * On timeout buffers MUST remain pinned; dev must outlive its IRQ handler. */
int virtio_reset(struct virtio_dev *dev);

/* Descriptor chains: allocate n descriptors under vq->lock, returns the
 * head or -ENOSPC. Chains are freed on completion. */
int virtq_alloc_chain(struct virtqueue *vq, unsigned n, uint16_t *ids);
void virtq_free_chain(struct virtqueue *vq, uint16_t head);
/* Publish head in the avail ring and notify the device. Caller holds vq->lock. */
void virtq_submit(struct virtqueue *vq, uint16_t head, void *cookie);
/* Process completions without an interrupt (panic path). Caller holds vq->lock. */
void virtq_poll_locked(struct virtqueue *vq);
