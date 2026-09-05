#pragma once
#include <kernel.h>
#include <fs/vfs.h>
#include <sync/spinlock.h>
#include <ipc/poll.h>

struct pcm_device;

/* Hardware-facing operations for one raw PCM device.  The PCM core owns
 * exclusive-open policy; the driver owns format and queue state. */
struct pcm_ops {
    int (*open)(struct pcm_device *dev, struct file *f);
    long (*read)(struct pcm_device *dev, struct file *f, char *buf, size_t n);    /* capture, optional */
    long (*write)(struct pcm_device *dev, struct file *f, const char *buf, size_t n);
    long (*ioctl)(struct pcm_device *dev, struct file *f, unsigned long req, uintptr_t arg);
    int (*poll)(struct pcm_device *dev, struct file *f);
    void (*close)(struct pcm_device *dev, struct file *f);
};

struct pcm_device {
    char name[16];
    const struct pcm_ops *ops;
    void *priv;
    struct spinlock owner_lock;     /* protects owner */
    struct file *owner;
    struct poll_source poll;
};

/* Register a flat devfs node, normally pcm0, pcm1, ... */
int pcm_register(struct pcm_device *dev, const char *name,
                 const struct pcm_ops *ops, void *priv);
