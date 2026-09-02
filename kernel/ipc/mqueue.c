#define KLOG_SUBSYS "mqueue"
#include <ipc/mqueue.h>
#include <ipc/signal.h>
#include <fs/vfs.h>
#include <sched/wait.h>
#include <sched/thread.h>
#include <drivers/timer.h>
#include <mm/slab.h>
#include <lib/string.h>
#include <kassert.h>
#include <klog.h>
#include <errno.h>

#define MQ_MAX 32

/* A message queue. lock protects the ring and counts; it is the condition
 * lock of rd_waitq and wr_waitq. refs and name are protected by
 * mq_table_lock. */
struct mqueue {
    char name[MQ_NAME_MAX];
    int refs;
    bool unlinked;
    struct spinlock lock;
    struct waitq rd_waitq, wr_waitq;
    struct {
        uint16_t len;
        uint8_t data[MQ_MSG_MAX];
    } ring[MQ_DEPTH];
    unsigned head, tail, count;
};

static struct mqueue *queues[MQ_MAX];
static DEFINE_SPINLOCK(mq_table_lock);

/* poll: a single wait queue woken by every producer.  poll_generation
 * closes the check-to-sleep race: a waiter snapshots it before scanning
 * descriptors and sleeps only when no producer notified in the meantime. */
static DEFINE_SPINLOCK(poll_lock);
static DEFINE_WAITQ(poll_waitq);
static uint64_t poll_generation;

void poll_notify(void)
{
    spin_lock(&poll_lock);
    poll_generation++;
    waitq_wake_all(&poll_waitq);
    spin_unlock(&poll_lock);
}

static long mq_read(struct file *f, char *buf, size_t n, uint64_t *pos)
{
    struct mqueue *q = f->priv;
    spin_lock(&q->lock);
    while (q->count == 0) {
        if (signal_should_interrupt()) {
            spin_unlock(&q->lock);
            return -EINTR;
        }
        if (f->flags & O_NONBLOCK) {
            spin_unlock(&q->lock);
            return -EAGAIN;
        }
        waitq_wait(&q->rd_waitq, &q->lock);
    }
    uint8_t tmp[MQ_MSG_MAX];
    size_t len = q->ring[q->head].len;
    memcpy(tmp, q->ring[q->head].data, len);
    q->head = (q->head + 1) % MQ_DEPTH;
    q->count--;
    waitq_wake_all(&q->wr_waitq);
    spin_unlock(&q->lock);
    poll_notify();
    if (len > n)
        len = n;
    memcpy(buf, tmp, len);
    return (long)len;
}

static long mq_write(struct file *f, const char *buf, size_t n, uint64_t *pos)
{
    struct mqueue *q = f->priv;
    if (n > MQ_MSG_MAX)
        return -EMSGSIZE;
    uint8_t tmp[MQ_MSG_MAX];
    memcpy(tmp, buf, n);
    spin_lock(&q->lock);
    while (q->count == MQ_DEPTH) {
        if (signal_should_interrupt()) {
            spin_unlock(&q->lock);
            return -EINTR;
        }
        if (f->flags & O_NONBLOCK) {
            spin_unlock(&q->lock);
            return -EAGAIN;
        }
        waitq_wait(&q->wr_waitq, &q->lock);
    }
    q->ring[q->tail].len = (uint16_t)n;
    memcpy(q->ring[q->tail].data, tmp, n);
    q->tail = (q->tail + 1) % MQ_DEPTH;
    q->count++;
    waitq_wake_all(&q->rd_waitq);
    spin_unlock(&q->lock);
    poll_notify();
    return (long)n;
}

static int mq_poll(struct file *f)
{
    struct mqueue *q = f->priv;
    spin_lock(&q->lock);
    int r = (q->count ? POLLIN : 0) | (q->count < MQ_DEPTH ? POLLOUT : 0);
    spin_unlock(&q->lock);
    return r;
}

static void mq_release(struct file *f)
{
    struct mqueue *q = f->priv;
    spin_lock(&mq_table_lock);
    bool free_it = --q->refs == 0 && q->unlinked;
    if (free_it) {
        for (int i = 0; i < MQ_MAX; i++)
            if (queues[i] == q)
                queues[i] = NULL;
    }
    spin_unlock(&mq_table_lock);
    if (free_it)
        kfree(q);
}

static const struct file_ops mq_fops = {
    .read = mq_read,
    .write = mq_write,
    .poll = mq_poll,
    .release = mq_release,
    .lseek = NULL,
};

int mq_open(const char *name, int flags, struct file **out)
{
    if (!name[0] || strlen(name) >= MQ_NAME_MAX)
        return -EINVAL;
    spin_lock(&mq_table_lock);
    struct mqueue *q = NULL;
    int slot = -1;
    for (int i = 0; i < MQ_MAX; i++) {
        if (queues[i] && !queues[i]->unlinked && strcmp(queues[i]->name, name) == 0)
            q = queues[i];
        else if (!queues[i] && slot < 0)
            slot = i;
    }
    if (q && (flags & MQ_EXCL)) {
        spin_unlock(&mq_table_lock);
        return -EEXIST;
    }
    if (!q) {
        if (!(flags & MQ_CREATE)) {
            spin_unlock(&mq_table_lock);
            return -ENOENT;
        }
        if (slot < 0) {
            spin_unlock(&mq_table_lock);
            return -ENOSPC;
        }
        spin_unlock(&mq_table_lock);
        q = kzalloc(sizeof *q);
        if (!q)
            return -ENOMEM;
        strlcpy(q->name, name, sizeof q->name);
        spinlock_init(&q->lock, "mqueue");
        waitq_init(&q->rd_waitq, "mq_rd");
        waitq_init(&q->wr_waitq, "mq_wr");
        spin_lock(&mq_table_lock);
        /* Recheck: another thread may have created it meanwhile. */
        struct mqueue *other = NULL;
        for (int i = 0; i < MQ_MAX; i++)
            if (queues[i] && !queues[i]->unlinked && strcmp(queues[i]->name, name) == 0)
                other = queues[i];
        if (other) {
            kfree(q);
            q = other;
        } else if (!queues[slot]) {
            queues[slot] = q;
        } else {
            spin_unlock(&mq_table_lock);
            kfree(q);
            return -ENOSPC;
        }
    }
    q->refs++;
    spin_unlock(&mq_table_lock);
    struct file *f = file_alloc(NULL, &mq_fops, O_RDWR);
    if (!f) {
        mq_release(&(struct file){ .priv = q });
        return -ENOMEM;
    }
    f->priv = q;
    *out = f;
    return 0;
}

int mq_unlink(const char *name)
{
    spin_lock(&mq_table_lock);
    for (int i = 0; i < MQ_MAX; i++) {
        struct mqueue *q = queues[i];
        if (q && !q->unlinked && strcmp(q->name, name) == 0) {
            q->unlinked = true;
            bool free_it = q->refs == 0;
            if (free_it)
                queues[i] = NULL;
            spin_unlock(&mq_table_lock);
            if (free_it)
                kfree(q);
            return 0;
        }
    }
    spin_unlock(&mq_table_lock);
    return -ENOENT;
}

long poll_files(struct file **files, struct pollfd *pfds, size_t n, long timeout_ms)
{
    uint64_t deadline = timeout_ms > 0 ? timer_ms() + (uint64_t)timeout_ms : 0;
    for (;;) {
        spin_lock(&poll_lock);
        uint64_t generation = poll_generation;
        spin_unlock(&poll_lock);

        long ready = 0;
        for (size_t i = 0; i < n; i++) {
            pfds[i].revents = 0;
            if (!files[i]) {
                continue;
            }
            int r = files[i]->ops && files[i]->ops->poll ? files[i]->ops->poll(files[i]) : POLLIN | POLLOUT;
            pfds[i].revents = (int16_t)(r & (pfds[i].events | POLLHUP | POLLERR | POLLNVAL));
            if (pfds[i].revents)
                ready++;
        }
        if (ready || timeout_ms == 0)
            return ready;
        if (timeout_ms > 0 && timer_ms() >= deadline)
            return 0;
        if (signal_should_interrupt())
            return -EINTR;
        /* Sleep until a producer wakes poll or, with a timeout, for a
         * bounded slice so the deadline is honoured.  Recheck the
         * generation while holding poll_lock so a notification between
         * the readiness scan and waiter registration cannot be lost. */
        spin_lock(&poll_lock);
        if (poll_generation == generation) {
            if (timeout_ms < 0)
                waitq_wait(&poll_waitq, &poll_lock);
            else
                waitq_wait_timeout(&poll_waitq, &poll_lock, deadline);
        }
        spin_unlock(&poll_lock);
    }
}
