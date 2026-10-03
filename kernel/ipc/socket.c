/* The common socket layer: family dispatch, files, addresses, flags and
 * options shared by every backend (N01). Locks: socket.lock (the error
 * word) is a leaf; families_lock (the family table) is a leaf. Neither
 * is locked across a backend call. */
#define KLOG_SUBSYS "socket"
#include <ipc/socket.h>
#include <ipc/socket_validate.h>
_Static_assert(AF_UNIX == 1 && AF_INET == 2 && sizeof(struct sockaddr_in) == 16 &&
               sizeof(struct sockaddr_storage) == 128, "socket validator ABI");
#include <net/inet.h>
#include <net/byteorder.h>
#include <sched/cred.h>
#include <syscall/syscalls.h>
#include <mm/slab.h>
#include <lib/string.h>
#include <kassert.h>
#include <klog.h>
#include <errno.h>

#define SOCKET_MAX_FAMILIES 4

static const struct socket_family *families[SOCKET_MAX_FAMILIES];
static DEFINE_SPINLOCK(families_lock);

static const struct file_ops sock_fops;

int socket_register_family(const struct socket_family *fam)
{
    spin_lock(&families_lock);
    for (int i = 0; i < SOCKET_MAX_FAMILIES; i++) {
        if (families[i] && families[i]->family == fam->family) {
            spin_unlock(&families_lock);
            return -EEXIST;
        }
    }
    for (int i = 0; i < SOCKET_MAX_FAMILIES; i++) {
        if (!families[i]) {
            families[i] = fam;
            spin_unlock(&families_lock);
            return 0;
        }
    }
    spin_unlock(&families_lock);
    return -ENOSPC;
}

static const struct socket_family *family_find(int family)
{
    const struct socket_family *fam = NULL;
    spin_lock(&families_lock);
    for (int i = 0; i < SOCKET_MAX_FAMILIES; i++)
        if (families[i] && families[i]->family == family)
            fam = families[i];
    spin_unlock(&families_lock);
    return fam;
}

static const struct socket_family unix_family = {
    .family = AF_UNIX,
    .create = unix_socket_create,
};

void socket_init(void)
{
    socket_register_family(&unix_family);
    inet_socket_init();
}

/* ---- objects and files ---- */

struct socket *socket_alloc(int family, int type, int protocol, const struct socket_ops *ops)
{
    struct socket *s = kzalloc(sizeof *s);
    if (!s)
        return NULL;
    s->family = family;
    s->type = type;
    s->protocol = protocol;
    s->ops = ops;
    spinlock_init(&s->lock, "socket");
    poll_source_init(&s->poll, "socket_poll");
    return s;
}

void socket_free(struct socket *s)
{
    kfree(s);
}

bool file_is_socket(const struct file *f)
{
    return f->ops == &sock_fops;
}

struct socket *socket_from_file(struct file *f)
{
    return file_is_socket(f) ? f->priv : NULL;
}

int socket_file(struct socket *s, int fflags, struct file **out)
{
    struct file *f = file_alloc(NULL, &sock_fops, O_RDWR | (fflags & O_NONBLOCK));
    if (!f) {
        if (s->ops && s->ops->release)
            s->ops->release(s);
        socket_free(s);
        return -ENOMEM;
    }
    f->priv = s;
    s->file = f;
    *out = f;
    return 0;
}

int socket_create(int family, int type, int protocol, int fflags, struct file **out)
{
    const struct socket_family *fam = family_find(family);
    if (!fam)
        return -EAFNOSUPPORT;
    struct socket *s = socket_alloc(family, type, protocol, NULL);
    if (!s)
        return -ENOMEM;
    int r = fam->create(s);
    if (r < 0) {
        socket_free(s);
        return r;
    }
    kassert(s->ops != NULL);
    return socket_file(s, fflags, out);
}

int socket_pair(int family, int type, int protocol, int fflags, struct file **a, struct file **b)
{
    if (family != AF_UNIX)
        return -EOPNOTSUPP;
    const struct socket_family *fam = family_find(family);
    if (!fam)
        return -EAFNOSUPPORT;
    struct socket *sa = socket_alloc(family, type, protocol, NULL);
    struct socket *sb = socket_alloc(family, type, protocol, NULL);
    if (!sa || !sb) {
        socket_free(sa);
        socket_free(sb);
        return -ENOMEM;
    }
    int r = fam->create(sa);
    if (r < 0) {
        socket_free(sa);
        socket_free(sb);
        return r;
    }
    r = fam->create(sb);
    if (r < 0) {
        sa->ops->release(sa);
        socket_free(sa);
        socket_free(sb);
        return r;
    }
    r = unix_socket_pair(sa, sb);
    if (r < 0) {
        sa->ops->release(sa);
        sb->ops->release(sb);
        socket_free(sa);
        socket_free(sb);
        return r;
    }
    r = socket_file(sa, fflags, a);
    if (r < 0) {
        sb->ops->release(sb);
        socket_free(sb);
        return r;
    }
    r = socket_file(sb, fflags, b);
    if (r < 0) {
        file_put(*a);
        return r;
    }
    return 0;
}

bool socket_nonblocking(const struct socket *s)
{
    return s->file && (s->file->flags & O_NONBLOCK);
}

void socket_set_error(struct socket *s, int err)
{
    spin_lock(&s->lock);
    s->error = err;
    spin_unlock(&s->lock);
}

int socket_take_error(struct socket *s)
{
    spin_lock(&s->lock);
    int err = s->error;
    s->error = 0;
    spin_unlock(&s->lock);
    return err;
}

/* ---- operations ---- */

int socket_bind(struct socket *s, const struct sockaddr_storage *addr, socklen_t len)
{
    if (addr->ss_family != s->family)
        return -EAFNOSUPPORT;
    /* The ports below 1024 are reserved to root (U2). */
    if (s->family == AF_INET && len >= sizeof(struct sockaddr_in)) {
        uint16_t port = ntohs(((const struct sockaddr_in *)addr)->sin_port);
        if (port && port < 1024 && !cred_current_is_root())
            return -EACCES;
    }
    if (!s->ops->bind)
        return -EOPNOTSUPP;
    return s->ops->bind(s, addr, len);
}

int socket_listen(struct socket *s, int backlog)
{
    if (!s->ops->listen)
        return -EOPNOTSUPP;
    return s->ops->listen(s, backlog);
}

int socket_accept(struct socket *s, int fflags, struct file **out,
                  struct sockaddr_storage *peer, socklen_t *peerlen)
{
    if (!s->ops->accept)
        return -EOPNOTSUPP;
    struct socket *ns;
    int r = s->ops->accept(s, &ns);
    if (r < 0)
        return r;
    if (peer) {
        memset(peer, 0, sizeof *peer);
        r = ns->ops->getname ? ns->ops->getname(ns, peer, peerlen, true) : -EOPNOTSUPP;
        if (r < 0) {
            ns->ops->release(ns);
            socket_free(ns);
            return r;
        }
    }
    return socket_file(ns, fflags, out);
}

int socket_connect(struct socket *s, const struct sockaddr_storage *addr, socklen_t len)
{
    if (addr->ss_family != s->family)
        return -EAFNOSUPPORT;
    if (!s->ops->connect)
        return -EOPNOTSUPP;
    return s->ops->connect(s, addr, len);
}

int socket_shutdown(struct socket *s, int how)
{
    if (how != SHUT_RD && how != SHUT_WR && how != SHUT_RDWR)
        return -EINVAL;
    if (!s->ops->shutdown)
        return -EOPNOTSUPP;
    return s->ops->shutdown(s, how);
}

long socket_sendmsg(struct socket *s, struct socket_msg *m)
{
    if (m->flags & ~SOCKET_MSG_FLAGS)
        return -EINVAL;
    if (m->flags & (MSG_PEEK | MSG_TRUNC | MSG_WAITALL))
        return -EINVAL;
    if (!s->ops->sendmsg)
        return -EOPNOTSUPP;
    if (socket_nonblocking(s))
        m->flags |= MSG_DONTWAIT;
    m->rflags = 0;
    return s->ops->sendmsg(s, m);
}

long socket_recvmsg(struct socket *s, struct socket_msg *m)
{
    if (m->flags & ~SOCKET_MSG_FLAGS)
        return -EINVAL;
    if (m->flags & (MSG_EOR | MSG_NOSIGNAL))
        return -EINVAL;
    if (!s->ops->recvmsg)
        return -EOPNOTSUPP;
    if (socket_nonblocking(s))
        m->flags |= MSG_DONTWAIT;
    m->rflags = 0;
    if (m->nfiles < 0)
        m->nfiles = 0;
    return s->ops->recvmsg(s, m);
}

int socket_getname(struct socket *s, struct sockaddr_storage *addr, socklen_t *len, bool peer)
{
    if (!s->ops->getname)
        return -EOPNOTSUPP;
    memset(addr, 0, sizeof *addr);
    return s->ops->getname(s, addr, len, peer);
}

/* SOL_SOCKET options every family answers alike are handled here; the
 * rest, and every other level, go to the backend. */
int socket_setsockopt(struct socket *s, int level, int name, const void *val, socklen_t len)
{
    if (level == SOL_SOCKET) {
        switch (name) {
        case SO_TYPE:
        case SO_ERROR:
        case SO_DOMAIN:
        case SO_PROTOCOL:
            return -ENOPROTOOPT;
        default:
            break;
        }
    }
    if (!s->ops->setsockopt)
        return -ENOPROTOOPT;
    return s->ops->setsockopt(s, level, name, val, len);
}

static int put_int_option(void *val, socklen_t *len, int v)
{
    if (*len < sizeof(int))
        return -EINVAL;
    memcpy(val, &v, sizeof v);
    *len = sizeof v;
    return 0;
}

int socket_getsockopt(struct socket *s, int level, int name, void *val, socklen_t *len)
{
    if (level == SOL_SOCKET) {
        switch (name) {
        case SO_TYPE:
            return put_int_option(val, len, s->type);
        case SO_ERROR:
            return put_int_option(val, len, -socket_take_error(s));
        case SO_DOMAIN:
            return put_int_option(val, len, s->family);
        case SO_PROTOCOL:
            return put_int_option(val, len, s->protocol);
        default:
            break;
        }
    }
    if (!s->ops->getsockopt)
        return -ENOPROTOOPT;
    return s->ops->getsockopt(s, level, name, val, len);
}

/* ---- addresses ---- */

socklen_t socket_addr_min_len(int family)
{
    switch (family) {
    case AF_UNIX:
        return sizeof(uint16_t);
    case AF_INET:
        return sizeof(struct sockaddr_in);
    default:
        return 0;
    }
}

int socket_addr_from_user(uintptr_t uaddr, size_t ulen, struct sockaddr_storage *out, socklen_t *outlen)
{
    if (ulen < sizeof(uint16_t) || ulen > sizeof *out)
        return -EINVAL;
    if (!user_range_ok(uaddr, ulen, false))
        return -EFAULT;
    memset(out, 0, sizeof *out);
    memcpy(out, (const void *)uaddr, ulen);
    int result = socket_parse_address((const uint8_t *)out, ulen);
    if (result < 0)
        return result;
    *outlen = (socklen_t)ulen;
    return 0;
}

int socket_addr_to_user(uintptr_t uaddr, uintptr_t ulenp, const struct sockaddr_storage *addr, socklen_t len)
{
    if (!uaddr || !ulenp)
        return 0;
    if (!user_range_ok(ulenp, sizeof(socklen_t), true))
        return -EFAULT;
    socklen_t room;
    memcpy(&room, (const void *)ulenp, sizeof room);
    socklen_t n = MIN(room, len);
    if (n > sizeof *addr)
        n = sizeof *addr;
    if (n && !user_range_ok(uaddr, n, true))
        return -EFAULT;
    if (n)
        memcpy((void *)uaddr, addr, n);
    memcpy((void *)ulenp, &len, sizeof len);
    return 0;
}

/* ---- file operations ---- */

/* read/write reach file operations with a caller buffer, unlike sendmsg/
 * recvmsg whose syscall layer already copies the message. Internet backends
 * run on netd or copy under endpoint locks, so they must see kernel storage.
 * The file reference prevents the socket from being freed during the worker request. */
static long sock_read(struct file *f, char *buf, size_t n, uint64_t *pos)
{
    struct socket *s = f->priv;
    if (s->family != AF_INET) {
        struct socket_msg m = { .data = buf, .len = n };
        return socket_recvmsg(s, &m);
    }

    size_t capacity = MIN(n, 65536);
    char *copy = kmalloc(capacity ? capacity : 1);
    if (!copy)
        return -ENOMEM;
    struct socket_msg m = { .data = copy, .len = capacity };
    long result = socket_recvmsg(s, &m);
    if (result > 0)
        memcpy(buf, copy, (size_t)result);
    kfree(copy);
    return result;
}

static long sock_write(struct file *f, const char *buf, size_t n, uint64_t *pos)
{
    struct socket *s = f->priv;
    if (s->family != AF_INET) {
        struct socket_msg m = { .data = (char *)buf, .len = n };
        return socket_sendmsg(s, &m);
    }

    if (n > 65536)
        return -EMSGSIZE;
    char *copy = kmalloc(n ? n : 1);
    if (!copy)
        return -ENOMEM;
    if (n)
        memcpy(copy, buf, n);
    struct socket_msg m = { .data = copy, .len = n };
    long result = socket_sendmsg(s, &m);
    kfree(copy);
    return result;
}

static int sock_poll(struct file *f)
{
    struct socket *s = f->priv;
    return s->ops->poll ? s->ops->poll(s) : POLLIN | POLLOUT;
}

static struct poll_source *sock_poll_source(struct file *f)
{
    return &((struct socket *)f->priv)->poll;
}

static void sock_release(struct file *f)
{
    struct socket *s = f->priv;
    if (s->ops->release)
        s->ops->release(s);
    socket_free(s);
}

static long sock_lseek(struct file *f, long off, int whence)
{
    return -ESPIPE;
}

static const struct file_ops sock_fops = {
    .read = sock_read,
    .write = sock_write,
    .poll = sock_poll,
    .poll_source = sock_poll_source,
    .release = sock_release,
    .lseek = sock_lseek,
    .flags = FOPS_STREAM,
};
