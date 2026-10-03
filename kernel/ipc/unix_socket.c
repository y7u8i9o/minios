/* Unix domain stream sockets (M23, the first backend of the common socket
 * layer since N01): abstract names, a listen backlog, two 64 KiB rings
 * per connection, descriptor passing with SCM_RIGHTS records attached to
 * byte positions of the stream, MSG_PEEK, MSG_WAITALL, MSG_DONTWAIT and
 * MSG_NOSIGNAL, poll and non blocking modes.
 *
 * Locks: sock_table_lock (listener names) -> unix_sock->lock (backlog);
 * separately conn->lock (rings, records, names and the two poll source
 * pointers), which is taken before the sockets' poll_source.lock when a
 * readiness change is announced, so a socket that goes away can clear
 * its pointer under conn->lock and never be notified afterwards. No lock
 * is held across a user copy: data moves through a bounce buffer as in
 * pipe.c. */
#define KLOG_SUBSYS "unix"
#include <ipc/socket.h>
#include <ipc/poll.h>
#include <ipc/signal.h>
#include <sync/ring.h>
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
    struct spsc_ring ring;
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
    struct poll_source *poll[2];            /* the sockets' sources, NULL once a side released */
    char name[2][SOCK_NAME_MAX];            /* the bound name of each side, empty if unnamed */
    struct ucred cred[2];                   /* the process of each side, for SO_PEERCRED (U4) */
};

enum { UNIX_UNBOUND, UNIX_LISTENING, UNIX_CONNECTED };

struct unix_sock {
    struct socket *sock;
    struct spinlock lock;
    int state;
    char name[SOCK_NAME_MAX];
    struct conn *conn;
    int side;
    struct conn *backlog[SOMAXCONN];
    int nbacklog;
    struct waitq accept_waitq;
    struct ucred cred;                      /* the process that called listen, under lock */
};

/* The pid and effective ids of the calling process. */
static void current_ucred(struct ucred *out)
{
    struct proc *p = thread_current()->proc;
    struct cred c;
    cred_get(p, &c);
    out->pid = p->pid;
    out->uid = c.euid;
    out->gid = c.egid;
}

static struct unix_sock *listeners[SOCK_MAX_LISTENERS];
static DEFINE_SPINLOCK(sock_table_lock);

static const struct socket_ops unix_ops;

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
    /* Both ends start as the creator, which is right for socketpair. connect
     * replaces the listener's side. */
    current_ucred(&c->cred[0]);
    c->cred[1] = c->cred[0];
    for (int i = 0; i < 2; i++) {
        struct sock_dir *d = &c->dir[i];
        d->pages = pmm_alloc(SOCK_BUF_ORDER);
        if (!d->pages) {
            dir_free(&c->dir[0]);
            kfree(c);
            return NULL;
        }
        d->buf = P2V(page_to_phys(d->pages));
        ring_init(&d->ring, d->buf, SOCK_BUF);
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

/* Announce a readiness change to both ends. Called with c->lock held so
 * a side that released cannot be notified after clearing its pointer. */
static void conn_notify_locked(struct conn *c)
{
    poll_source_notify(c->poll[0]);
    poll_source_notify(c->poll[1]);
}

/* One end goes away: its writes end, its reads end. */
static void conn_close_side(struct conn *c, int side)
{
    spin_lock(&c->lock);
    c->poll[side] = NULL;
    c->dir[side].writer_closed = true;
    c->dir[1 - side].reader_closed = true;
    waitq_wake_all(&c->dir[side].rd_waitq);
    waitq_wake_all(&c->dir[1 - side].wr_waitq);
    conn_notify_locked(c);
    spin_unlock(&c->lock);
}

/* ---- sockets ---- */

int unix_socket_create(struct socket *s)
{
    if (s->type != SOCK_STREAM)
        return -ESOCKTNOSUPPORT;
    if (s->protocol != 0)
        return -EPROTONOSUPPORT;
    struct unix_sock *u = kzalloc(sizeof *u);
    if (!u)
        return -ENOMEM;
    u->sock = s;
    spinlock_init(&u->lock, "unix_sock");
    waitq_init(&u->accept_waitq, "sock_accept");
    s->priv = u;
    s->ops = &unix_ops;
    return 0;
}

static void attach(struct unix_sock *u, struct conn *c, int side)
{
    u->state = UNIX_CONNECTED;
    u->conn = c;
    u->side = side;
    spin_lock(&c->lock);
    c->poll[side] = &u->sock->poll;
    strlcpy(c->name[side], u->name, SOCK_NAME_MAX);
    spin_unlock(&c->lock);
}

int unix_socket_pair(struct socket *a, struct socket *b)
{
    struct conn *c = conn_create();
    if (!c)
        return -ENOMEM;
    attach(a->priv, c, 0);
    attach(b->priv, c, 1);
    return 0;
}

static int unix_bind(struct socket *s, const struct sockaddr_storage *addr, socklen_t len)
{
    struct unix_sock *u = s->priv;
    const struct sockaddr_un *sa = (const struct sockaddr_un *)addr;
    char name[SOCK_NAME_MAX];
    size_t pathlen = len - offsetof(struct sockaddr_un, sun_path);
    if (len <= offsetof(struct sockaddr_un, sun_path))
        return -EINVAL;
    if (pathlen > SOCK_NAME_MAX)
        pathlen = SOCK_NAME_MAX;
    memcpy(name, sa->sun_path, pathlen);
    name[pathlen - 1] = '\0';
    name[SOCK_NAME_MAX - 1] = '\0';
    if (!name[0] || strnlen(name, SOCK_NAME_MAX) >= SOCK_NAME_MAX)
        return -EINVAL;
    spin_lock(&sock_table_lock);
    if (u->state != UNIX_UNBOUND || u->name[0]) {
        spin_unlock(&sock_table_lock);
        return -EINVAL;
    }
    for (int i = 0; i < SOCK_MAX_LISTENERS; i++)
        if (listeners[i] && strcmp(listeners[i]->name, name) == 0) {
            spin_unlock(&sock_table_lock);
            return -EADDRINUSE;
        }
    strlcpy(u->name, name, sizeof u->name);
    spin_unlock(&sock_table_lock);
    return 0;
}

static int unix_listen(struct socket *s, int backlog)
{
    struct unix_sock *u = s->priv;
    if (backlog < 0)
        return -EINVAL;
    /* Taken before the table lock, since it takes proc.lock. */
    struct ucred me;
    current_ucred(&me);
    spin_lock(&sock_table_lock);
    if (u->state == UNIX_LISTENING) {
        spin_unlock(&sock_table_lock);
        return 0;
    }
    if (u->state != UNIX_UNBOUND || !u->name[0]) {
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
    spin_lock(&u->lock);
    u->cred = me;
    spin_unlock(&u->lock);
    listeners[slot] = u;
    u->state = UNIX_LISTENING;
    spin_unlock(&sock_table_lock);
    return 0;
}

static int unix_connect(struct socket *s, const struct sockaddr_storage *addr, socklen_t len)
{
    struct unix_sock *u = s->priv;
    const struct sockaddr_un *sa = (const struct sockaddr_un *)addr;
    char name[SOCK_NAME_MAX];
    if (len <= offsetof(struct sockaddr_un, sun_path))
        return -EINVAL;
    size_t pathlen = MIN(len - offsetof(struct sockaddr_un, sun_path), (size_t)SOCK_NAME_MAX);
    memcpy(name, sa->sun_path, pathlen);
    name[pathlen - 1] = '\0';
    name[SOCK_NAME_MAX - 1] = '\0';
    if (u->state == UNIX_CONNECTED)
        return -EISCONN;
    if (u->state == UNIX_LISTENING)
        return -EOPNOTSUPP;
    struct conn *c = conn_create();
    if (!c)
        return -ENOMEM;
    spin_lock(&sock_table_lock);
    struct unix_sock *l = NULL;
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
    /* The listener's name is the peer name the client reports, and its
     * process the peer credentials. c is not shared yet. */
    strlcpy(c->name[1], l->name, SOCK_NAME_MAX);
    c->cred[1] = l->cred;
    l->backlog[l->nbacklog++] = c;          /* the listener's reference */
    waitq_wake_all(&l->accept_waitq);
    spin_unlock(&l->lock);
    spin_unlock(&sock_table_lock);
    attach(u, c, 0);
    poll_source_notify(&l->sock->poll);
    return 0;
}

static int unix_accept(struct socket *s, struct socket **out)
{
    struct unix_sock *l = s->priv;
    if (l->state != UNIX_LISTENING)
        return -EINVAL;
    spin_lock(&l->lock);
    while (l->nbacklog == 0) {
        if (socket_nonblocking(s)) {
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
    struct socket *ns = socket_alloc(s->family, s->type, s->protocol, NULL);
    if (!ns || unix_socket_create(ns) < 0) {
        socket_free(ns);
        conn_close_side(c, 1);
        conn_put(c);
        return -ENOMEM;
    }
    struct unix_sock *nu = ns->priv;
    strlcpy(nu->name, l->name, sizeof nu->name);
    attach(nu, c, 1);
    *out = ns;
    return 0;
}

static int unix_shutdown(struct socket *s, int how)
{
    struct unix_sock *u = s->priv;
    if (u->state != UNIX_CONNECTED)
        return -ENOTCONN;
    struct conn *c = u->conn;
    spin_lock(&c->lock);
    if (how == SHUT_WR || how == SHUT_RDWR) {
        c->dir[u->side].writer_closed = true;
        waitq_wake_all(&c->dir[u->side].rd_waitq);
    }
    if (how == SHUT_RD || how == SHUT_RDWR) {
        c->dir[1 - u->side].reader_closed = true;
        waitq_wake_all(&c->dir[1 - u->side].wr_waitq);
    }
    conn_notify_locked(c);
    spin_unlock(&c->lock);
    return 0;
}

/* ---- data ---- */

/* An unnamed socket reports the family alone, as an unbound Unix socket
 * does elsewhere; a named one reports the whole sockaddr_un. */
static int fill_name(struct sockaddr_storage *addr, socklen_t *len, const char *name)
{
    struct sockaddr_un *sa = (struct sockaddr_un *)addr;
    sa->sun_family = AF_UNIX;
    socklen_t full = sizeof(uint16_t);
    if (name[0]) {
        strlcpy(sa->sun_path, name, sizeof sa->sun_path);
        full = sizeof *sa;
    }
    *len = full;
    return 0;
}

static long unix_sendmsg(struct socket *s, struct socket_msg *m)
{
    struct unix_sock *u = s->priv;
    if (m->flags & (MSG_OOB | MSG_EOR))
        return -EOPNOTSUPP;
    if (m->addrlen)
        return u->state == UNIX_CONNECTED ? -EISCONN : -EOPNOTSUPP;
    if (u->state != UNIX_CONNECTED)
        return -ENOTCONN;
    if (m->nfiles > SCM_MAX_FD || (m->nfiles > 0 && m->len == 0))
        return -EINVAL;
    const char *buf = m->data;
    size_t n = m->len;
    struct conn *c = u->conn;
    struct sock_dir *d = &c->dir[u->side];
    char tmp[BOUNCE];
    size_t done = 0;
    bool first = true;
    while (done < n || first) {
        size_t chunk = MIN(n - done, sizeof tmp);
        memcpy(tmp, buf + done, chunk);
        spin_lock(&c->lock);
        if (first && m->nfiles > 0) {
            if (d->reader_closed || d->writer_closed) {
                spin_unlock(&c->lock);
                if (!(m->flags & MSG_NOSIGNAL))
                    signal_send(thread_current()->proc, SIGPIPE);
                return -EPIPE;
            }
            if (d->nrecs == SOCK_MAX_RECS) {
                spin_unlock(&c->lock);
                return -EAGAIN;
            }
            struct fdrec *r = &d->recs[(d->rec_head + d->nrecs) % SOCK_MAX_RECS];
            r->pos = d->wpos;
            r->n = m->nfiles;
            for (int i = 0; i < m->nfiles; i++)
                r->files[i] = m->files[i];
            d->nrecs++;
        }
        first = false;
        size_t off = 0;
        while (off < chunk) {
            if (d->reader_closed || d->writer_closed) {
                spin_unlock(&c->lock);
                if (done + off)
                    return (long)(done + off);
                if (!(m->flags & MSG_NOSIGNAL))
                    signal_send(thread_current()->proc, SIGPIPE);
                return -EPIPE;
            }
            if (ring_space(&d->ring) == 0) {
                if (m->flags & MSG_DONTWAIT) {
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
            size_t k = ring_write(&d->ring, tmp + off, chunk - off);
            d->wpos += k;
            off += k;
            waitq_wake_all(&d->rd_waitq);
        }
        conn_notify_locked(c);
        spin_unlock(&c->lock);
        done += chunk;
    }
    return (long)n;
}

static long unix_recvmsg(struct socket *s, struct socket_msg *m)
{
    struct unix_sock *u = s->priv;
    if (m->flags & (MSG_OOB | MSG_TRUNC))
        return -EOPNOTSUPP;
    int room = m->nfiles;
    m->nfiles = 0;
    m->addrlen = 0;
    if (u->state != UNIX_CONNECTED)
        return -ENOTCONN;
    char *buf = m->data;
    size_t n = m->len;
    if (n == 0)
        return 0;
    bool peek = m->flags & MSG_PEEK;
    bool waitall = m->flags & MSG_WAITALL;
    struct conn *c = u->conn;
    struct sock_dir *d = &c->dir[1 - u->side];
    char tmp[BOUNCE];
    size_t got = 0;
    int delivered = 0;
    spin_lock(&c->lock);
    /* The source is the peer, named or not, for callers that ask. */
    if (m->addr)
        fill_name(m->addr, &m->addrlen, c->name[1 - u->side]);
    for (;;) {
        while (ring_count(&d->ring) == 0) {
            if (d->writer_closed || (got && !waitall))
                goto done;
            if (m->flags & MSG_DONTWAIT) {
                if (got)
                    goto done;
                spin_unlock(&c->lock);
                return -EAGAIN;
            }
            if (signal_should_interrupt()) {
                if (got)
                    goto done;
                spin_unlock(&c->lock);
                return -EINTR;
            }
            waitq_wait(&d->rd_waitq, &c->lock);
        }
        /* Descriptors of the message starting here go with the bytes that
         * consume it, then a limit so the read does not run into the next
         * message carrying descriptors. A peek consumes nothing and
         * ignores the records. */
        size_t limit = ring_count(&d->ring);
        if (!peek) {
            if (d->nrecs && d->recs[d->rec_head].pos == d->rpos) {
                if (got)
                    goto done;
                struct fdrec *r = &d->recs[d->rec_head];
                for (int i = 0; i < r->n; i++) {
                    if (m->files && delivered < room)
                        m->files[delivered++] = r->files[i];
                    else {
                        file_put(r->files[i]);
                        m->rflags |= MSG_CTRUNC;
                    }
                }
                d->rec_head = (d->rec_head + 1) % SOCK_MAX_RECS;
                d->nrecs--;
            }
            if (d->nrecs && d->recs[d->rec_head].pos - d->rpos < limit)
                limit = (size_t)(d->recs[d->rec_head].pos - d->rpos);
        }
        size_t want = MIN(n - got, limit);
        bool boundary = !peek && d->nrecs && want == limit && limit < n - got;
        size_t taken = 0;
        while (taken < want) {
            size_t chunk = MIN(want - taken, sizeof tmp);
            if (peek) {
                chunk = ring_peek(&d->ring, got + taken, tmp, chunk);
            } else {
                chunk = ring_read(&d->ring, tmp, chunk);
                d->rpos += chunk;
                waitq_wake_all(&d->wr_waitq);
            }
            spin_unlock(&c->lock);
            memcpy(buf + got + taken, tmp, chunk);
            taken += chunk;
            spin_lock(&c->lock);
        }
        got += taken;
        if (!peek)
            conn_notify_locked(c);
        if (got >= n || !waitall || peek || boundary)
            goto done;
    }
done:
    spin_unlock(&c->lock);
    m->nfiles = delivered;
    return (long)got;
}

/* ---- state ---- */

static int unix_poll(struct socket *s)
{
    struct unix_sock *u = s->priv;
    int r = 0;
    if (u->state == UNIX_LISTENING) {
        spin_lock(&u->lock);
        r = u->nbacklog ? POLLIN : 0;
        spin_unlock(&u->lock);
        return r;
    }
    if (u->state != UNIX_CONNECTED)
        return POLLHUP;
    struct conn *c = u->conn;
    spin_lock(&c->lock);
    struct sock_dir *in = &c->dir[1 - u->side], *out = &c->dir[u->side];
    if (ring_count(&in->ring) || in->writer_closed)
        r |= POLLIN;
    if (ring_space(&out->ring) || out->reader_closed)
        r |= POLLOUT;
    if (in->writer_closed && out->reader_closed)
        r |= POLLHUP;
    spin_unlock(&c->lock);
    return r;
}

static int unix_getname(struct socket *s, struct sockaddr_storage *addr, socklen_t *len, bool peer)
{
    struct unix_sock *u = s->priv;
    if (!peer)
        return fill_name(addr, len, u->name);
    if (u->state != UNIX_CONNECTED)
        return -ENOTCONN;
    struct conn *c = u->conn;
    char name[SOCK_NAME_MAX];
    spin_lock(&c->lock);
    strlcpy(name, c->name[1 - u->side], sizeof name);
    spin_unlock(&c->lock);
    return fill_name(addr, len, name);
}

static int unix_setsockopt(struct socket *s, int level, int name, const void *val, socklen_t len)
{
    return -ENOPROTOOPT;
}

/* SO_PEERCRED reports the process at the other end (U4). */
static int unix_getsockopt(struct socket *s, int level, int name, void *val, socklen_t *len)
{
    struct unix_sock *u = s->priv;
    if (level != SOL_SOCKET || name != SO_PEERCRED)
        return -ENOPROTOOPT;
    if (*len < sizeof(struct ucred))
        return -EINVAL;
    struct conn *c = u->state == UNIX_CONNECTED ? u->conn : NULL;
    if (!c)
        return -ENOTCONN;
    spin_lock(&c->lock);
    struct ucred peer = c->cred[1 - u->side];
    spin_unlock(&c->lock);
    memcpy(val, &peer, sizeof peer);
    *len = sizeof peer;
    return 0;
}

static void unix_release(struct socket *s)
{
    struct unix_sock *u = s->priv;
    if (u->state == UNIX_LISTENING) {
        spin_lock(&sock_table_lock);
        for (int i = 0; i < SOCK_MAX_LISTENERS; i++)
            if (listeners[i] == u)
                listeners[i] = NULL;
        spin_unlock(&sock_table_lock);
        for (int i = 0; i < u->nbacklog; i++) {
            conn_close_side(u->backlog[i], 1);
            conn_put(u->backlog[i]);
        }
    } else if (u->state == UNIX_CONNECTED) {
        conn_close_side(u->conn, u->side);
        conn_put(u->conn);
    }
    kfree(u);
    s->priv = NULL;
}

static const struct socket_ops unix_ops = {
    .bind = unix_bind,
    .listen = unix_listen,
    .accept = unix_accept,
    .connect = unix_connect,
    .shutdown = unix_shutdown,
    .sendmsg = unix_sendmsg,
    .recvmsg = unix_recvmsg,
    .poll = unix_poll,
    .getname = unix_getname,
    .setsockopt = unix_setsockopt,
    .getsockopt = unix_getsockopt,
    .release = unix_release,
};
