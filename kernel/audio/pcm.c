#define KLOG_SUBSYS "pcm"
#include <drivers/devinfo.h>
#include <lib/printf.h>
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

/* The registered devices for /dev/devices. pcm_register runs in the
 * start-up thread before init starts, and the table is read only
 * afterwards. */
#define PCM_MAX_DEVICES 8
static struct pcm_device *registered[PCM_MAX_DEVICES];
static unsigned nregistered;

void pcm_free_name(char *name, size_t size)
{
    ksnprintf(name, size, "pcm%u", nregistered);
}

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
    if (r == 0) {
        klog_info("/dev/%s registered", dev->name);
        if (nregistered < PCM_MAX_DEVICES)
            registered[nregistered++] = dev;
    }
    return r;
}

void pcm_describe(struct devinfo *d)
{
    devinfo_node(d, "audio", "Audio");
    devinfo_prop(d, "devices", "%u", nregistered);
    for (unsigned i = 0; i < nregistered; i++) {
        struct pcm_device *dev = registered[i];
        char path[32];
        ksnprintf(path, sizeof path, "audio/%s", dev->name);
        devinfo_node(d, path, "%s", dev->name);
        devinfo_prop(d, "device_node", "/dev/%s", dev->name);
        spin_lock(&dev->owner_lock);
        bool open = dev->owner != NULL;
        spin_unlock(&dev->owner_lock);
        devinfo_prop(d, "opened", "%s", open ? "yes" : "no");
        if (dev->ops->describe)
            dev->ops->describe(dev, d);
    }
}
