#define KLOG_SUBSYS "pcm"
#include <audio/pcm.h>
#include <fs/devfs.h>
#include <lib/string.h>
#include <klog.h>
#include <errno.h>

static int pcm_open(struct inode *ino, struct file *f)
{
    struct pcm_device *dev = ino->priv;
    spin_lock(&dev->owner_lock);
    if (dev->owner) {
        spin_unlock(&dev->owner_lock);
        return -EBUSY;
    }
    dev->owner = f;
    f->priv = dev;
    spin_unlock(&dev->owner_lock);
    if (dev->ops->open) {
        int r = dev->ops->open(dev, f);
        if (r < 0) {
            spin_lock(&dev->owner_lock);
            dev->owner = NULL;
            spin_unlock(&dev->owner_lock);
            f->priv = NULL;
            return r;
        }
    }
    return 0;
}

static long pcm_read(struct file *f, char *buf, size_t n, uint64_t *pos)
{
    struct pcm_device *dev = f->priv;
    if (!dev || !dev->ops->read)
        return -EINVAL;
    return dev->ops->read(dev, f, buf, n);
}

static long pcm_write(struct file *f, const char *buf, size_t n, uint64_t *pos)
{
    struct pcm_device *dev = f->priv;
    if (!dev || !dev->ops->write)
        return -EINVAL;
    return dev->ops->write(dev, f, buf, n);
}

static long pcm_ioctl(struct file *f, unsigned long req, uintptr_t arg)
{
    struct pcm_device *dev = f->priv;
    if (!dev || !dev->ops->ioctl)
        return -ENOTTY;
    return dev->ops->ioctl(dev, f, req, arg);
}

static int pcm_poll(struct file *f)
{
    struct pcm_device *dev = f->priv;
    if (!dev || !dev->ops->poll)
        return POLLERR;
    return dev->ops->poll(dev, f);
}

static struct poll_source *pcm_poll_source(struct file *f)
{
    struct pcm_device *dev = f->priv;
    return dev ? &dev->poll : NULL;
}

static void pcm_release(struct file *f)
{
    struct pcm_device *dev = f->priv;
    if (!dev)
        return;
    if (dev->ops->close)
        dev->ops->close(dev, f);
    spin_lock(&dev->owner_lock);
    if (dev->owner == f)
        dev->owner = NULL;
    spin_unlock(&dev->owner_lock);
}

static const struct file_ops pcm_fops = {
    .open = pcm_open,
    .read = pcm_read,
    .write = pcm_write,
    .ioctl = pcm_ioctl,
    .poll = pcm_poll,
    .poll_source = pcm_poll_source,
    .release = pcm_release,
};

int pcm_register(struct pcm_device *dev, const char *name,
                 const struct pcm_ops *ops, void *priv)
{
    if (!dev || !name || !ops || !ops->write)
        return -EINVAL;
    memset(dev, 0, sizeof *dev);
    strlcpy(dev->name, name, sizeof dev->name);
    dev->ops = ops;
    dev->priv = priv;
    spinlock_init(&dev->owner_lock, "pcm_owner");
    poll_source_init(&dev->poll, "pcm_poll");
    int r = devfs_register(dev->name, S_IFCHR | 0600, &pcm_fops, dev, 0);
    if (r == 0)
        klog_info("/dev/%s registered", dev->name);
    return r;
}
