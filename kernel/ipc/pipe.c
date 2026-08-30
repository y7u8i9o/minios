#define KLOG_SUBSYS "pipe"
#include <ipc/pipe.h>
#include <fs/vfs.h>
#include <sched/wait.h>
#include <sched/thread.h>
#include <sched/proc.h>
#include <ipc/signal.h>
#include <ipc/mqueue.h>
#include <mm/slab.h>
#include <lib/string.h>
#include <kassert.h>
#include <klog.h>
#include <errno.h>

#define PIPE_SIZE 4096

/* A pipe: ring buffer plus the number of open ends. Everything is
 * protected by lock, the condition lock of both wait queues. */
struct pipe {
    struct spinlock lock;
    char buf[PIPE_SIZE];
    size_t head, tail, count;
    int readers, writers;
    struct waitq rd_waitq;          /* readers waiting for data */
    struct waitq wr_waitq;          /* writers waiting for room */
};


/* User buffers may fault on swapped pages, which blocks, so they are never
 * touched with p->lock held: data moves through a bounce buffer. */
#define BOUNCE 256

static long pipe_read(struct file *f, char *buf, size_t n, uint64_t *pos)
{
    struct pipe *p = f->priv;
    char tmp[BOUNCE];
    spin_lock(&p->lock);
    while (p->count == 0) {
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
    size_t got = 0;
    while (got < n && p->count > 0) {
        size_t chunk = 0;
        while (chunk < BOUNCE && got + chunk < n && p->count > 0) {
            tmp[chunk++] = p->buf[p->head];
            p->head = (p->head + 1) % PIPE_SIZE;
            p->count--;
        }
        spin_unlock(&p->lock);
        memcpy(buf + got, tmp, chunk);
        got += chunk;
        spin_lock(&p->lock);
    }
    waitq_wake_all(&p->wr_waitq);
    spin_unlock(&p->lock);
    poll_notify();
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
            if (p->count == PIPE_SIZE) {
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
            while (off < chunk && p->count < PIPE_SIZE) {
                p->buf[p->tail] = tmp[off++];
                p->tail = (p->tail + 1) % PIPE_SIZE;
                p->count++;
            }
            waitq_wake_all(&p->rd_waitq);
        }
        spin_unlock(&p->lock);
        poll_notify();
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
    bool gone = p->readers == 0 && p->writers == 0;
    waitq_wake_all(&p->rd_waitq);
    waitq_wake_all(&p->wr_waitq);
    spin_unlock(&p->lock);
    poll_notify();
    if (gone)
        kfree(p);
}

/* Readiness: data or no writers for the read end, room or no readers
 * for the write end. */
static int pipe_rd_poll(struct file *f)
{
    struct pipe *p = f->priv;
    spin_lock(&p->lock);
    int r = p->count > 0 || p->writers == 0 ? POLLIN : 0;
    spin_unlock(&p->lock);
    return r;
}

static int pipe_wr_poll(struct file *f)
{
    struct pipe *p = f->priv;
    spin_lock(&p->lock);
    int r = p->count < PIPE_SIZE || p->readers == 0 ? POLLOUT : 0;
    spin_unlock(&p->lock);
    return r;
}

static long pipe_lseek(struct file *f, long off, int whence)
{
    return -ESPIPE;
}

static const struct file_ops pipe_rd_fops = {
    .read = pipe_read,
    .poll = pipe_rd_poll,
    .release = pipe_release,
    .lseek = pipe_lseek,
};
static const struct file_ops pipe_wr_fops = {
    .write = pipe_write,
    .poll = pipe_wr_poll,
    .release = pipe_release,
    .lseek = pipe_lseek,
};

int pipe_create(struct file **rd, struct file **wr)
{
    struct pipe *p = kzalloc(sizeof *p);
    if (!p)
        return -ENOMEM;
    spinlock_init(&p->lock, "pipe");
    waitq_init(&p->rd_waitq, "pipe_rd");
    waitq_init(&p->wr_waitq, "pipe_wr");
    p->readers = 1;
    p->writers = 1;
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
