#define KLOG_SUBSYS "pipe"
#include <ipc/pipe.h>
#include <fs/vfs.h>
#include <sched/wait.h>
#include <sched/thread.h>
#include <sched/proc.h>
#include <ipc/signal.h>
#include <ipc/poll.h>
#include <sync/ring.h>
#include <sync/atomic.h>
#include <mm/slab.h>
#include <lib/string.h>
#include <kassert.h>
#include <klog.h>
#include <errno.h>

#define PIPE_SIZE 4096

/* File I/O serializes the sole producer and consumer.  The byte path is an
 * SPSC ring; lock covers endpoint counts and lost-wakeup avoidance only.
 * refs is atomic: one reference per end, dropped as the last step of
 * pipe_release, so the pipe outlives both releases. */
struct pipe {
    struct spinlock lock;
    char buf[PIPE_SIZE];
    struct spsc_ring ring;
    int readers, writers;
    refcount_t refs;
    struct waitq rd_waitq;          /* readers waiting for data */
    struct waitq wr_waitq;          /* writers waiting for room */
    struct poll_source poll;
};


/* User buffers may fault on swapped pages, which blocks, so they are never
 * touched with p->lock acquired: data moves through a bounce buffer. */
#define BOUNCE 256

static long pipe_read(struct file *f, char *buf, size_t n, uint64_t *pos)
{
    struct pipe *p = f->priv;
    char tmp[BOUNCE];
    spin_lock(&p->lock);
    while (ring_count(&p->ring) == 0) {
        if (p->writers == 0) {
            spin_unlock(&p->lock);
            return 0;
        }
        if (signal_should_interrupt()) {
            spin_unlock(&p->lock);
            return -EINTR;
        }
        if (f->flags & O_NONBLOCK) {
            spin_unlock(&p->lock);
            return -EAGAIN;
        }
        waitq_wait(&p->rd_waitq, &p->lock);
    }
    spin_unlock(&p->lock);
    size_t got = 0;
    while (got < n) {
        size_t chunk = ring_read(&p->ring, tmp, MIN(n - got, (size_t)BOUNCE));
        if (!chunk)
            break;
        memcpy(buf + got, tmp, chunk);
        got += chunk;
    }
    spin_lock(&p->lock);
    waitq_wake_all(&p->wr_waitq);
    spin_unlock(&p->lock);
    poll_source_notify(&p->poll);
    return (long)got;
}

static long pipe_write(struct file *f, const char *buf, size_t n, uint64_t *pos)
{
    struct pipe *p = f->priv;
    char tmp[BOUNCE];
    size_t done = 0;
    while (done < n) {
        size_t chunk = MIN(n - done, (size_t)BOUNCE);
        memcpy(tmp, buf + done, chunk);
        size_t off = 0;
        spin_lock(&p->lock);
        while (off < chunk) {
            if (p->readers == 0) {
                spin_unlock(&p->lock);
                signal_send(thread_current()->proc, SIGPIPE);
                return done ? (long)done : -EPIPE;
            }
            if (ring_space(&p->ring) == 0) {
                if (signal_should_interrupt()) {
                    spin_unlock(&p->lock);
                    return done ? (long)done : -EINTR;
                }
                if (f->flags & O_NONBLOCK) {
                    spin_unlock(&p->lock);
                    return done ? (long)done : -EAGAIN;
                }
                waitq_wait(&p->wr_waitq, &p->lock);
                continue;
            }
            spin_unlock(&p->lock);
            off += ring_write(&p->ring, tmp + off, chunk - off);
            spin_lock(&p->lock);
            waitq_wake_all(&p->rd_waitq);
        }
        spin_unlock(&p->lock);
        poll_source_notify(&p->poll);
        done += chunk;
    }
    return (long)done;
}

static void pipe_release(struct file *f)
{
    struct pipe *p = f->priv;
    spin_lock(&p->lock);
    if ((f->flags & O_ACCMODE) == O_RDONLY)
        p->readers--;
    else
        p->writers--;
    waitq_wake_all(&p->rd_waitq);
    waitq_wake_all(&p->wr_waitq);
    spin_unlock(&p->lock);
    poll_source_notify(&p->poll);
    /* The other end may be released on another CPU at the same moment
     * (a pipeline whose processes exit together). Whoever drops the last
     * reference frees the pipe, after both ends have finished with it. */
    if (refcount_dec_and_test(&p->refs))
        kfree(p);
}

/* Readiness: data or no writers for the read end, room or no readers
 * for the write end. */
static int pipe_rd_poll(struct file *f)
{
    struct pipe *p = f->priv;
    spin_lock(&p->lock);
    int r = ring_count(&p->ring) > 0 || p->writers == 0 ? POLLIN : 0;
    spin_unlock(&p->lock);
    return r;
}

static int pipe_wr_poll(struct file *f)
{
    struct pipe *p = f->priv;
    spin_lock(&p->lock);
    int r = ring_space(&p->ring) > 0 || p->readers == 0 ? POLLOUT : 0;
    spin_unlock(&p->lock);
    return r;
}

static struct poll_source *pipe_poll_source(struct file *f)
{
    return &((struct pipe *)f->priv)->poll;
}

static long pipe_lseek(struct file *f, long off, int whence)
{
    return -ESPIPE;
}

static const struct file_ops pipe_rd_fops = {
    .read = pipe_read,
    .poll = pipe_rd_poll,
    .poll_source = pipe_poll_source,
    .release = pipe_release,
    .lseek = pipe_lseek,
};
static const struct file_ops pipe_wr_fops = {
    .write = pipe_write,
    .poll = pipe_wr_poll,
    .poll_source = pipe_poll_source,
    .release = pipe_release,
    .lseek = pipe_lseek,
};

int pipe_create(struct file **rd, struct file **wr)
{
    struct pipe *p = kzalloc(sizeof *p);
    if (!p)
        return -ENOMEM;
    spinlock_init(&p->lock, "pipe");
    ring_init(&p->ring, p->buf, sizeof p->buf);
    waitq_init(&p->rd_waitq, "pipe_rd");
    waitq_init(&p->wr_waitq, "pipe_wr");
    poll_source_init(&p->poll, "pipe_poll");
    p->readers = 1;
    p->writers = 1;
    refcount_set(&p->refs, 2);
    struct file *r = file_alloc(NULL, &pipe_rd_fops, O_RDONLY);
    struct file *w = file_alloc(NULL, &pipe_wr_fops, O_WRONLY);
    if (!r || !w) {
        kfree(r);
        kfree(w);
        kfree(p);
        return -ENOMEM;
    }
    r->priv = p;
    w->priv = p;
    *rd = r;
    *wr = w;
    return 0;
}
