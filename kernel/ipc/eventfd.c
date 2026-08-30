/* eventfd: a 64 bit counter readable when non zero. lock protects the
 * counter and is the condition lock of waitq. */
#include <ipc/eventfd.h>
#include <ipc/mqueue.h>
#include <ipc/signal.h>
#include <sched/wait.h>
#include <mm/slab.h>
#include <lib/string.h>
#include <errno.h>

struct eventfd {
    struct spinlock lock;
    uint64_t count;
    struct waitq waitq;
};

static long eventfd_read(struct file *f, char *buf, size_t n, uint64_t *pos)
{
    struct eventfd *e = f->priv;
    if (n < 8)
        return -EINVAL;
    spin_lock(&e->lock);
    while (e->count == 0) {
        if (f->flags & O_NONBLOCK) {
            spin_unlock(&e->lock);
            return -EAGAIN;
        }
        if (signal_should_interrupt()) {
            spin_unlock(&e->lock);
            return -EINTR;
        }
        waitq_wait(&e->waitq, &e->lock);
    }
    uint64_t v = e->count;
    e->count = 0;
    waitq_wake_all(&e->waitq);
    spin_unlock(&e->lock);
    memcpy(buf, &v, 8);
    poll_notify();
    return 8;
}

static long eventfd_write(struct file *f, const char *buf, size_t n, uint64_t *pos)
{
    struct eventfd *e = f->priv;
    if (n < 8)
        return -EINVAL;
    uint64_t v;
    memcpy(&v, buf, 8);
    if (v == UINT64_MAX)
        return -EINVAL;
    spin_lock(&e->lock);
    while (e->count + v < e->count || e->count + v == UINT64_MAX) {
        if (f->flags & O_NONBLOCK) {
            spin_unlock(&e->lock);
            return -EAGAIN;
        }
        if (signal_should_interrupt()) {
            spin_unlock(&e->lock);
            return -EINTR;
        }
        waitq_wait(&e->waitq, &e->lock);
    }
    e->count += v;
    waitq_wake_all(&e->waitq);
    spin_unlock(&e->lock);
    poll_notify();
    return 8;
}

static int eventfd_poll(struct file *f)
{
    struct eventfd *e = f->priv;
    spin_lock(&e->lock);
    int r = (e->count ? POLLIN : 0) | (e->count < UINT64_MAX - 1 ? POLLOUT : 0);
    spin_unlock(&e->lock);
    return r;
}

static void eventfd_release(struct file *f)
{
    kfree(f->priv);
}

static const struct file_ops eventfd_fops = {
    .read = eventfd_read,
    .write = eventfd_write,
    .poll = eventfd_poll,
    .release = eventfd_release,
};

int eventfd_create(uint64_t initval, int flags, struct file **out)
{
    struct eventfd *e = kzalloc(sizeof *e);
    if (!e)
        return -ENOMEM;
    spinlock_init(&e->lock, "eventfd");
    waitq_init(&e->waitq, "eventfd");
    e->count = initval;
    struct file *f = file_alloc(NULL, &eventfd_fops, O_RDWR | (flags & O_NONBLOCK));
    if (!f) {
        kfree(e);
        return -ENOMEM;
    }
    f->priv = e;
    *out = f;
    return 0;
}
