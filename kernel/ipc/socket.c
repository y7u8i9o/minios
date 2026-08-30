/* Unix domain stream sockets: abstract names, a listen backlog, two 64
 * KiB rings per connection, descriptor passing with SCM_RIGHTS records
 * attached to byte positions of the stream, poll and non blocking
 * modes. Locks: sock_table_lock (listener names) -> sock->lock
 * (backlog) and, separately, conn->lock (rings and records); neither
 * is held across a user copy, data moves through a bounce buffer as in
 * pipe.c. */
#define KLOG_SUBSYS "socket"
#include <ipc/socket.h>
#include <ipc/mqueue.h>
#include <ipc/signal.h>
#include <sched/wait.h>
#include <sched/thread.h>
#include <sched/proc.h>
#include <mm/pmm.h>
#include <mm/memlayout.h>
#include <mm/slab.h>
#include <lib/string.h>
#include <kassert.h>
#include <klog.h>
#include <errno.h>

#define SOCK_BUF_ORDER 4                    /* 16 pages, 64 KiB per direction */
#define SOCK_BUF ((size_t)PAGE_SIZE << SOCK_BUF_ORDER)
#define SOCK_MAX_RECS 32
#define SOCK_MAX_LISTENERS 16
#define BOUNCE 256

struct fdrec {
    uint64_t pos;                           /* stream position of the message's first byte */
    struct file *files[SCM_MAX_FD];
    int n;
};

/* One direction of a connection: bytes written by one end and read by
 * the other, with the descriptor records in stream order. */
struct sock_dir {
    struct page *pages;
    uint8_t *buf;
    size_t head, tail, count;
    uint64_t wpos, rpos;
    struct fdrec recs[SOCK_MAX_RECS];
    int rec_head, nrecs;
    bool writer_closed, reader_closed;
    struct waitq rd_waitq, wr_waitq;
};

struct conn {
    struct spinlock lock;
    struct sock_dir dir[2];                 /* dir[i] is written by side i */
    int refs;
};

struct sock {
    struct spinlock lock;
    int state;                              /* 0 unbound, 1 listening, 2 connected */
    char name[SOCK_NAME_MAX];
    struct conn *conn;
    int side;
    struct conn *backlog[SOMAXCONN];
    int nbacklog;
    struct waitq accept_waitq;
};

static struct sock *listeners[SOCK_MAX_LISTENERS];
static DEFINE_SPINLOCK(sock_table_lock);

static const struct file_ops sock_fops;

bool file_is_socket(const struct file *f)
{
    return f->ops == &sock_fops;
}

/* ---- connections ---- */

static void dir_free(struct sock_dir *d)
{
    for (int i = 0; i < d->nrecs; i++) {
        struct fdrec *r = &d->recs[(d->rec_head + i) % SOCK_MAX_RECS];
        for (int j = 0; j < r->n; j++)
            file_put(r->files[j]);
    }
    if (d->pages)
        pmm_free(d->pages, SOCK_BUF_ORDER);
}

static struct conn *conn_create(void)
{
    struct conn *c = kzalloc(sizeof *c);
    if (!c)
        return NULL;
    spinlock_init(&c->lock, "sockconn");
    for (int i = 0; i < 2; i++) {
        struct sock_dir *d = &c->dir[i];
        d->pages = pmm_alloc(SOCK_BUF_ORDER);
        if (!d->pages) {
            dir_free(&c->dir[0]);
            kfree(c);
            return NULL;
        }
        d->buf = P2V(page_to_phys(d->pages));
        waitq_init(&d->rd_waitq, "sock_rd");
        waitq_init(&d->wr_waitq, "sock_wr");
    }
    c->refs = 2;
    return c;
}

static void conn_put(struct conn *c)
{
    spin_lock(&c->lock);
    bool last = --c->refs == 0;
    spin_unlock(&c->lock);
    if (last) {
        dir_free(&c->dir[0]);
        dir_free(&c->dir[1]);
        kfree(c);
    }
}

/* One end goes away: its writes end, its reads end. */
static void conn_close_side(struct conn *c, int side)
{
    spin_lock(&c->lock);
    c->dir[side].writer_closed = true;
    c->dir[1 - side].reader_closed = true;
    waitq_wake_all(&c->dir[side].rd_waitq);
    waitq_wake_all(&c->dir[1 - side].wr_waitq);
    spin_unlock(&c->lock);
    poll_notify();
}

/* ---- sockets ---- */

static struct sock *sock_alloc(void)
{
    struct sock *s = kzalloc(sizeof *s);
    if (!s)
        return NULL;
    spinlock_init(&s->lock, "sock");
    waitq_init(&s->accept_waitq, "sock_accept");
    return s;
}

static int sock_file(struct sock *s, int flags, struct file **out)
{
    struct file *f = file_alloc(NULL, &sock_fops, O_RDWR | (flags & O_NONBLOCK));
    if (!f)
        return -ENOMEM;
    f->priv = s;
    *out = f;
    return 0;
}

int socket_create(int flags, struct file **out)
{
    struct sock *s = sock_alloc();
    if (!s)
        return -ENOMEM;
    int r = sock_file(s, flags, out);
    if (r < 0)
        kfree(s);
    return r;
}

int socket_pair(int flags, struct file **a, struct file **b)
{
    struct conn *c = conn_create();
    struct sock *sa = sock_alloc(), *sb = sock_alloc();
    if (!c || !sa || !sb) {
        if (c) { c->refs = 1; conn_put(c); }
        kfree(sa);
        kfree(sb);
        return -ENOMEM;
    }
    sa->state = sb->state = 2;
    sa->conn = sb->conn = c;
    sa->side = 0;
    sb->side = 1;
    int r = sock_file(sa, flags, a);
    if (r < 0) {
        c->refs = 1;
        conn_put(c);
        kfree(sa);
        kfree(sb);
        return r;
    }
    r = sock_file(sb, flags, b);
    if (r < 0) {
        file_put(*a);
        kfree(sb);
        return r;
    }
    return 0;
}

int socket_bind(struct file *f, const char *name)
{
    struct sock *s = f->priv;
    if (!name[0] || strlen(name) >= SOCK_NAME_MAX)
        return -EINVAL;
    spin_lock(&sock_table_lock);
    if (s->state != 0 || s->name[0]) {
        spin_unlock(&sock_table_lock);
        return -EINVAL;
    }
    for (int i = 0; i < SOCK_MAX_LISTENERS; i++)
        if (listeners[i] && strcmp(listeners[i]->name, name) == 0) {
            spin_unlock(&sock_table_lock);
            return -EADDRINUSE;
        }
    strlcpy(s->name, name, sizeof s->name);
    spin_unlock(&sock_table_lock);
    return 0;
}

int socket_listen(struct file *f, int backlog)
{
    struct sock *s = f->priv;
    spin_lock(&sock_table_lock);
    if (s->state != 0 || !s->name[0]) {
        spin_unlock(&sock_table_lock);
        return -EINVAL;
    }
    int slot = -1;
    for (int i = 0; i < SOCK_MAX_LISTENERS; i++)
        if (!listeners[i] && slot < 0)
            slot = i;
    if (slot < 0) {
        spin_unlock(&sock_table_lock);
        return -ENOSPC;
    }
    listeners[slot] = s;
    s->state = 1;
    spin_unlock(&sock_table_lock);
    (void)backlog;
    return 0;
}

int socket_connect(struct file *f, const char *name)
{
    struct sock *s = f->priv;
    if (s->state != 0)
        return -EISCONN;
    struct conn *c = conn_create();
    if (!c)
        return -ENOMEM;
    spin_lock(&sock_table_lock);
    struct sock *l = NULL;
    for (int i = 0; i < SOCK_MAX_LISTENERS; i++)
        if (listeners[i] && strcmp(listeners[i]->name, name) == 0)
            l = listeners[i];
    if (!l) {
        spin_unlock(&sock_table_lock);
        c->refs = 1;
        conn_put(c);
        return -ECONNREFUSED;
    }
    spin_lock(&l->lock);
    if (l->nbacklog == SOMAXCONN) {
        spin_unlock(&l->lock);
        spin_unlock(&sock_table_lock);
        c->refs = 1;
        conn_put(c);
        return -ECONNREFUSED;
    }
    l->backlog[l->nbacklog++] = c;          /* the listener's reference */
    waitq_wake_all(&l->accept_waitq);
    spin_unlock(&l->lock);
    spin_unlock(&sock_table_lock);
    s->conn = c;
    s->side = 0;
    s->state = 2;
    poll_notify();
    return 0;
}

int socket_accept(struct file *f, int flags, struct file **out)
{
    struct sock *l = f->priv;
    if (l->state != 1)
        return -EINVAL;
    spin_lock(&l->lock);
    while (l->nbacklog == 0) {
        if (f->flags & O_NONBLOCK) {
            spin_unlock(&l->lock);
            return -EAGAIN;
        }
        if (signal_should_interrupt()) {
            spin_unlock(&l->lock);
            return -EINTR;
        }
        waitq_wait(&l->accept_waitq, &l->lock);
    }
    struct conn *c = l->backlog[0];
    memmove(l->backlog, l->backlog + 1, (size_t)(l->nbacklog - 1) * sizeof l->backlog[0]);
    l->nbacklog--;
    spin_unlock(&l->lock);
    struct sock *s = sock_alloc();
    if (!s) {
        conn_close_side(c, 1);
        conn_put(c);
        return -ENOMEM;
    }
    s->state = 2;
    s->conn = c;
    s->side = 1;
    int r = sock_file(s, flags, out);
    if (r < 0) {
        kfree(s);
        conn_close_side(c, 1);
        conn_put(c);
    }
    return r;
}

int socket_shutdown(struct file *f, int how)
{
    struct sock *s = f->priv;
    if (s->state != 2)
        return -ENOTCONN;
    struct conn *c = s->conn;
    spin_lock(&c->lock);
    if (how == SHUT_WR || how == SHUT_RDWR) {
        c->dir[s->side].writer_closed = true;
        waitq_wake_all(&c->dir[s->side].rd_waitq);
    }
    if (how == SHUT_RD || how == SHUT_RDWR) {
        c->dir[1 - s->side].reader_closed = true;
        waitq_wake_all(&c->dir[1 - s->side].wr_waitq);
    }
    spin_unlock(&c->lock);
    poll_notify();
    return 0;
}

/* ---- data ---- */

long socket_send(struct file *f, const char *buf, size_t n, struct file **files, int nfiles)
{
    struct sock *s = f->priv;
    if (s->state != 2)
        return -ENOTCONN;
    if (nfiles > SCM_MAX_FD || (nfiles > 0 && n == 0))
        return -EINVAL;
    struct conn *c = s->conn;
    struct sock_dir *d = &c->dir[s->side];
    char tmp[BOUNCE];
    size_t done = 0;
    bool first = true;
    while (done < n || first) {
        size_t chunk = MIN(n - done, sizeof tmp);
        memcpy(tmp, buf + done, chunk);
        spin_lock(&c->lock);
        if (first && nfiles > 0) {
            if (d->nrecs == SOCK_MAX_RECS) {
                spin_unlock(&c->lock);
                return -EAGAIN;
            }
            struct fdrec *r = &d->recs[(d->rec_head + d->nrecs) % SOCK_MAX_RECS];
            r->pos = d->wpos;
            r->n = nfiles;
            for (int i = 0; i < nfiles; i++)
                r->files[i] = files[i];
            d->nrecs++;
        }
        first = false;
        size_t off = 0;
        while (off < chunk) {
            if (d->reader_closed || d->writer_closed) {
                spin_unlock(&c->lock);
                if (done + off)
                    return (long)(done + off);
                signal_send(thread_current()->proc, SIGPIPE);
                return -EPIPE;
            }
            if (d->count == SOCK_BUF) {
                if (f->flags & O_NONBLOCK) {
                    spin_unlock(&c->lock);
                    return done + off ? (long)(done + off) : -EAGAIN;
                }
                if (signal_should_interrupt()) {
                    spin_unlock(&c->lock);
                    return done + off ? (long)(done + off) : -EINTR;
                }
                waitq_wait(&d->wr_waitq, &c->lock);
                continue;
            }
            size_t room = SOCK_BUF - d->count, tail_room = SOCK_BUF - d->tail;
            size_t k = MIN(chunk - off, MIN(room, tail_room));
            memcpy(d->buf + d->tail, tmp + off, k);
            d->tail = (d->tail + k) % SOCK_BUF;
            d->count += k;
            d->wpos += k;
            off += k;
            waitq_wake_all(&d->rd_waitq);
        }
        spin_unlock(&c->lock);
        poll_notify();
        done += chunk;
    }
    return (long)n;
}

long socket_recv(struct file *f, char *buf, size_t n, struct file **files, int *nfiles)
{
    struct sock *s = f->priv;
    int room = nfiles ? *nfiles : 0;
    if (nfiles)
        *nfiles = 0;
    if (s->state != 2)
        return -ENOTCONN;
    struct conn *c = s->conn;
    struct sock_dir *d = &c->dir[1 - s->side];
    char tmp[BOUNCE];
    spin_lock(&c->lock);
    while (d->count == 0) {
        if (d->writer_closed) {
            spin_unlock(&c->lock);
            return 0;
        }
        if (f->flags & O_NONBLOCK) {
            spin_unlock(&c->lock);
            return -EAGAIN;
        }
        if (signal_should_interrupt()) {
            spin_unlock(&c->lock);
            return -EINTR;
        }
        waitq_wait(&d->rd_waitq, &c->lock);
    }
    /* Descriptors of the message starting here, then a limit so the
     * read does not run into the next message carrying descriptors. */
    int delivered = 0;
    if (d->nrecs && d->recs[d->rec_head].pos == d->rpos) {
        struct fdrec *r = &d->recs[d->rec_head];
        for (int i = 0; i < r->n; i++) {
            if (files && delivered < room)
                files[delivered++] = r->files[i];
            else
                file_put(r->files[i]);
        }
        d->rec_head = (d->rec_head + 1) % SOCK_MAX_RECS;
        d->nrecs--;
    }
    size_t limit = d->count;
    if (d->nrecs) {
        uint64_t next = d->recs[d->rec_head].pos;
        if (next - d->rpos < limit)
            limit = (size_t)(next - d->rpos);
    }
    if (n > limit)
        n = limit;
    size_t got = 0;
    while (got < n) {
        size_t chunk = MIN(n - got, sizeof tmp);
        size_t off = 0;
        while (off < chunk) {
            size_t head_run = SOCK_BUF - d->head;
            size_t k = MIN(chunk - off, head_run);
            memcpy(tmp + off, d->buf + d->head, k);
            d->head = (d->head + k) % SOCK_BUF;
            d->count -= k;
            d->rpos += k;
            off += k;
        }
        waitq_wake_all(&d->wr_waitq);
        spin_unlock(&c->lock);
        memcpy(buf + got, tmp, chunk);
        got += chunk;
        spin_lock(&c->lock);
    }
    spin_unlock(&c->lock);
    poll_notify();
    if (nfiles)
        *nfiles = delivered;
    return (long)got;
}

/* ---- file operations ---- */

static long sock_read(struct file *f, char *buf, size_t n, uint64_t *pos)
{
    return socket_recv(f, buf, n, NULL, NULL);
}

static long sock_write(struct file *f, const char *buf, size_t n, uint64_t *pos)
{
    return socket_send(f, buf, n, NULL, 0);
}

static int sock_poll(struct file *f)
{
    struct sock *s = f->priv;
    int r = 0;
    if (s->state == 1) {
        spin_lock(&s->lock);
        r = s->nbacklog ? POLLIN : 0;
        spin_unlock(&s->lock);
        return r;
    }
    if (s->state != 2)
        return POLLHUP;
    struct conn *c = s->conn;
    spin_lock(&c->lock);
    struct sock_dir *in = &c->dir[1 - s->side], *out = &c->dir[s->side];
    if (in->count || in->writer_closed)
        r |= POLLIN;
    if (out->count < SOCK_BUF || out->reader_closed)
        r |= POLLOUT;
    if (in->writer_closed && out->reader_closed)
        r |= POLLHUP;
    spin_unlock(&c->lock);
    return r;
}

static void sock_release(struct file *f)
{
    struct sock *s = f->priv;
    if (s->state == 1) {
        spin_lock(&sock_table_lock);
        for (int i = 0; i < SOCK_MAX_LISTENERS; i++)
            if (listeners[i] == s)
                listeners[i] = NULL;
        spin_unlock(&sock_table_lock);
        for (int i = 0; i < s->nbacklog; i++) {
            conn_close_side(s->backlog[i], 1);
            conn_put(s->backlog[i]);
        }
    } else if (s->state == 2) {
        conn_close_side(s->conn, s->side);
        conn_put(s->conn);
    }
    kfree(s);
}

static long sock_lseek(struct file *f, long off, int whence)
{
    return -ESPIPE;
}

static const struct file_ops sock_fops = {
    .read = sock_read,
    .write = sock_write,
    .poll = sock_poll,
    .release = sock_release,
    .lseek = sock_lseek,
};
