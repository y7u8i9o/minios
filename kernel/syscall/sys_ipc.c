#include <syscall/syscalls.h>
#include <sched/thread.h>
#include <sched/proc.h>
#include <fs/vfs.h>
#include <fs/fdtable.h>
#include <ipc/mqueue.h>
#include <ipc/poll.h>
#include <mm/slab.h>
#include <lib/string.h>
#include <errno.h>

static long install(struct file *f)
{
    long r = fdtable_install(&thread_current()->proc->fds, f, 0);
    if (r < 0)
        file_put(f);
    return r;
}

long sys_mq_open(struct trapframe *tf)
{
    char name[MQ_NAME_MAX];
    long r = copy_string_from_user(name, SYSARG0(tf), sizeof name);
    if (r < 0)
        return r;
    struct file *f;
    r = mq_open(name, (int)SYSARG1(tf), &f);
    return r < 0 ? r : install(f);
}

long sys_mq_unlink(struct trapframe *tf)
{
    char name[MQ_NAME_MAX];
    long r = copy_string_from_user(name, SYSARG0(tf), sizeof name);
    return r < 0 ? r : mq_unlink(name);
}

long sys_shm_open(struct trapframe *tf)
{
    char name[SHM_NAME_MAX];
    long r = copy_string_from_user(name, SYSARG0(tf), sizeof name);
    if (r < 0)
        return r;
    struct file *f;
    r = shm_open(name, (int)SYSARG1(tf), SYSARG2(tf), &f);
    return r < 0 ? r : install(f);
}

long sys_shm_unlink(struct trapframe *tf)
{
    char name[SHM_NAME_MAX];
    long r = copy_string_from_user(name, SYSARG0(tf), sizeof name);
    return r < 0 ? r : shm_unlink(name);
}

/* poll(fds, n, timeout_ms) */
long sys_poll(struct trapframe *tf)
{
    uintptr_t ufds = SYSARG0(tf);
    size_t n = SYSARG1(tf);
    long timeout = (long)SYSARG2(tf);
    if (n > OPEN_MAX)
        return -EINVAL;
    if (!user_range_ok(ufds, n * sizeof(struct pollfd), true))
        return -EFAULT;
    struct pollfd pfds[OPEN_MAX];
    struct file *files[OPEN_MAX];
    memcpy(pfds, (void *)ufds, n * sizeof pfds[0]);
    struct fdtable *t = &thread_current()->proc->fds;
    for (size_t i = 0; i < n; i++)
        files[i] = pfds[i].fd >= 0 ? fdtable_get(t, pfds[i].fd) : NULL;
    long r = poll_files(files, pfds, n, timeout);
    for (size_t i = 0; i < n; i++)
        if (files[i])
            file_put(files[i]);
    memcpy((void *)ufds, pfds, n * sizeof pfds[0]);
    return r;
}

/* ---- M23: sockets, descriptor passing, memfd, eventfd, timerfd ---- */

#include <ipc/socket.h>
#include <ipc/socket_validate.h>
#include <ipc/eventfd.h>

static int socket_flags(long flags)
{
    return (int)(flags & (O_NONBLOCK | O_CLOEXEC));
}

static long install_flags(struct file *f, int flags)
{
    long fd = install(f);
    if (fd >= 0 && (flags & O_CLOEXEC))
        fdtable_set_cloexec(&thread_current()->proc->fds, (int)fd, true);
    return fd;
}

/* socket(domain, type, protocol). The type carries SOCK_NONBLOCK and
 * SOCK_CLOEXEC. AF_UNIX callers of M23 pass those flags in the protocol
 * argument instead, which stays accepted for that family alone. */
long sys_socket(struct trapframe *tf)
{
    int family = (int)SYSARG0(tf);
    long type = (long)SYSARG1(tf);
    long protocol = (long)SYSARG2(tf);
    int flags = socket_flags(type);
    type &= ~(long)(SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (type < 0 || type > 0xff)
        return -EINVAL;
    if (family == AF_UNIX) {
        if (protocol & ~(long)(SOCK_NONBLOCK | SOCK_CLOEXEC))
            return -EPROTONOSUPPORT;
        flags |= socket_flags(protocol);
        protocol = 0;
    }
    if (protocol < 0 || protocol > 0xff)
        return -EPROTONOSUPPORT;
    struct file *f;
    int r = socket_create(family, (int)type, (int)protocol, flags, &f);
    return r < 0 ? r : install_flags(f, flags);
}

/* socketpair(domain, type, fds[2]) */
long sys_socketpair(struct trapframe *tf)
{
    int family = (int)SYSARG0(tf);
    long type = (long)SYSARG1(tf);
    uintptr_t ufds = SYSARG2(tf);
    if (!user_range_ok(ufds, 2 * sizeof(int), true))
        return -EFAULT;
    int flags = socket_flags(type);
    type &= ~(long)(SOCK_NONBLOCK | SOCK_CLOEXEC);
    struct file *a, *b;
    int r = socket_pair(family, (int)type, 0, flags, &a, &b);
    if (r < 0)
        return r;
    long fa = install_flags(a, flags);
    if (fa < 0)
        { file_put(b); return fa; }
    long fb = install_flags(b, flags);
    if (fb < 0) {
        fdtable_close(&thread_current()->proc->fds, (int)fa);
        return fb;
    }
    int fds[2] = { (int)fa, (int)fb };
    memcpy((void *)ufds, fds, sizeof fds);
    return 0;
}

static struct file *socket_file_get(int fd, long *err)
{
    struct file *f = fdtable_get(&thread_current()->proc->fds, fd);
    if (!f) {
        *err = -EBADF;
        return NULL;
    }
    if (!file_is_socket(f)) {
        file_put(f);
        *err = -ENOTSOCK;
        return NULL;
    }
    return f;
}

long sys_bind(struct trapframe *tf)
{
    struct sockaddr_storage addr;
    socklen_t len;
    long r = socket_addr_from_user(SYSARG1(tf), SYSARG2(tf), &addr, &len);
    if (r < 0)
        return r;
    struct file *f = socket_file_get((int)SYSARG0(tf), &r);
    if (!f)
        return r;
    r = socket_bind(f->priv, &addr, len);
    file_put(f);
    return r;
}

long sys_listen(struct trapframe *tf)
{
    long r;
    struct file *f = socket_file_get((int)SYSARG0(tf), &r);
    if (!f)
        return r;
    r = socket_listen(f->priv, (int)SYSARG1(tf));
    file_put(f);
    return r;
}

/* accept(fd, addr, addrlen, flags): addr, when given, receives the peer
 * address truncated to *addrlen, and *addrlen its full length. */
long sys_accept(struct trapframe *tf)
{
    long r;
    uintptr_t uaddr = SYSARG1(tf), ulenp = SYSARG2(tf);
    if (uaddr && !ulenp)
        return -EFAULT;
    struct file *f = socket_file_get((int)SYSARG0(tf), &r);
    if (!f)
        return r;
    int flags = socket_flags((long)SYSARG3(tf));
    struct file *nf;
    struct sockaddr_storage peer;
    socklen_t peerlen = sizeof peer;
    r = socket_accept(f->priv, flags, &nf, uaddr ? &peer : NULL, &peerlen);
    file_put(f);
    if (r < 0)
        return r;
    if (uaddr) {
        r = socket_addr_to_user(uaddr, ulenp, &peer, peerlen);
        if (r < 0) {
            file_put(nf);
            return r;
        }
    }
    return install_flags(nf, flags);
}

long sys_connect(struct trapframe *tf)
{
    struct sockaddr_storage addr;
    socklen_t len;
    long r = socket_addr_from_user(SYSARG1(tf), SYSARG2(tf), &addr, &len);
    if (r < 0)
        return r;
    struct file *f = socket_file_get((int)SYSARG0(tf), &r);
    if (!f)
        return r;
    r = socket_connect(f->priv, &addr, len);
    file_put(f);
    return r;
}

long sys_shutdown(struct trapframe *tf)
{
    long r;
    struct file *f = socket_file_get((int)SYSARG0(tf), &r);
    if (!f)
        return r;
    r = socket_shutdown(f->priv, (int)SYSARG1(tf));
    file_put(f);
    return r;
}

/* getsockname(fd, addr, addrlen) and getpeername */
static long getname(struct trapframe *tf, bool peer)
{
    long r;
    uintptr_t uaddr = SYSARG1(tf), ulenp = SYSARG2(tf);
    if (!uaddr || !ulenp)
        return -EFAULT;
    struct file *f = socket_file_get((int)SYSARG0(tf), &r);
    if (!f)
        return r;
    struct sockaddr_storage addr;
    socklen_t len = sizeof addr;
    r = socket_getname(f->priv, &addr, &len, peer);
    file_put(f);
    if (r < 0)
        return r;
    return socket_addr_to_user(uaddr, ulenp, &addr, len);
}

long sys_getsockname(struct trapframe *tf)
{
    return getname(tf, false);
}

long sys_getpeername(struct trapframe *tf)
{
    return getname(tf, true);
}

/* setsockopt(fd, level, name, val, len) */
long sys_setsockopt(struct trapframe *tf)
{
    uintptr_t uval = SYSARG3(tf);
    size_t len = SYSARG4(tf);
    if (len > SOCKET_OPT_MAX)
        return -EINVAL;
    if (len && !user_range_ok(uval, len, false))
        return -EFAULT;
    uint8_t val[SOCKET_OPT_MAX];
    if (len)
        memcpy(val, (const void *)uval, len);
    long r;
    struct file *f = socket_file_get((int)SYSARG0(tf), &r);
    if (!f)
        return r;
    r = socket_setsockopt(f->priv, (int)SYSARG1(tf), (int)SYSARG2(tf), val, (socklen_t)len);
    file_put(f);
    return r;
}

/* getsockopt(fd, level, name, val, lenp) */
long sys_getsockopt(struct trapframe *tf)
{
    uintptr_t uval = SYSARG3(tf), ulenp = SYSARG4(tf);
    if (!user_range_ok(ulenp, sizeof(socklen_t), true))
        return -EFAULT;
    socklen_t len;
    memcpy(&len, (const void *)ulenp, sizeof len);
    if (len > SOCKET_OPT_MAX)
        len = SOCKET_OPT_MAX;
    if (len && !user_range_ok(uval, len, true))
        return -EFAULT;
    uint8_t val[SOCKET_OPT_MAX];
    long r;
    struct file *f = socket_file_get((int)SYSARG0(tf), &r);
    if (!f)
        return r;
    r = socket_getsockopt(f->priv, (int)SYSARG1(tf), (int)SYSARG2(tf), val, &len);
    file_put(f);
    if (r < 0)
        return r;
    if (len)
        memcpy((void *)uval, val, len);
    memcpy((void *)ulenp, &len, sizeof len);
    return 0;
}

/* Gather the iovec of a user msghdr into one kernel buffer (at most 64
 * KiB per call: a stream moves the rest in later calls, a datagram must
 * fit) and collect the SCM_RIGHTS descriptors. Every range is checked
 * before anything is copied; a single element above 1 GiB or a sum that
 * would exceed it is rejected before the ranges are looked at, so the
 * total cannot overflow. */
#define MSG_MAX 65536
#define MSG_MAX_IOV 32
#define MSG_IOV_LIMIT ((size_t)1 << 30)

static long msghdr_from_user(uintptr_t addr, struct msghdr *m, struct iovec *iov, bool write,
                             size_t *total)
{
    if (!user_range_ok(addr, sizeof *m, true))
        return -EFAULT;
    memcpy(m, (void *)addr, sizeof *m);
    if (m->msg_iovlen > MSG_MAX_IOV)
        return -EINVAL;
    if (m->msg_iovlen && !user_range_ok((uintptr_t)m->msg_iov, m->msg_iovlen * sizeof *iov, false))
        return -EFAULT;
    memcpy(iov, m->msg_iov, m->msg_iovlen * sizeof *iov);
    size_t lengths[MSG_MAX_IOV];
    for (size_t i = 0; i < m->msg_iovlen; i++)
        lengths[i] = iov[i].iov_len;
    size_t sum;
    int result = socket_iovec_size(lengths, m->msg_iovlen, &sum);
    if (result < 0)
        return result;
    for (size_t i = 0; i < m->msg_iovlen; i++)
        if (iov[i].iov_len && !user_range_ok((uintptr_t)iov[i].iov_base, iov[i].iov_len, write))
            return -EFAULT;
    *total = sum;
    return 0;
}

/* sendmsg(fd, msg, flags) */
long sys_sendmsg(struct trapframe *tf)
{
    long r;
    struct file *f = socket_file_get((int)SYSARG0(tf), &r);
    if (!f)
        return r;
    struct socket *s = f->priv;
    struct msghdr m;
    struct iovec iov[MSG_MAX_IOV];
    size_t total;
    r = msghdr_from_user(SYSARG1(tf), &m, iov, false, &total);
    if (r < 0)
        goto out;
    if (total > MSG_MAX) {
        if (s->type != SOCK_STREAM) {
            r = -EMSGSIZE;
            goto out;
        }
        total = MSG_MAX;
    }
    struct socket_msg sm = { .flags = (int)SYSARG2(tf) };
    struct sockaddr_storage addr;
    if (m.msg_name && m.msg_namelen) {
        r = socket_addr_from_user((uintptr_t)m.msg_name, m.msg_namelen, &addr, &sm.addrlen);
        if (r < 0)
            goto out;
        sm.addr = &addr;
    }
    char *buf = kmalloc(total ? total : 1);
    if (!buf) {
        r = -ENOMEM;
        goto out;
    }
    size_t off = 0;
    for (size_t i = 0; i < m.msg_iovlen && off < total; i++) {
        size_t k = MIN(iov[i].iov_len, total - off);
        memcpy(buf + off, iov[i].iov_base, k);
        off += k;
    }
    struct file *files[SCM_MAX_FD];
    int nfiles = 0;
    if (m.msg_controllen) {
        if (s->family != AF_UNIX) {
            r = -EOPNOTSUPP;
            goto free;
        }
        uint8_t ctl[CMSG_SPACE(sizeof(int) * SCM_MAX_FD)];
        if (m.msg_controllen > sizeof ctl || m.msg_controllen < sizeof(struct cmsghdr)) {
            r = -EINVAL;
            goto free;
        }
        if (!user_range_ok((uintptr_t)m.msg_control, m.msg_controllen, false)) {
            r = -EFAULT;
            goto free;
        }
        memcpy(ctl, m.msg_control, m.msg_controllen);
        struct cmsghdr *c = (struct cmsghdr *)ctl;
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS ||
            c->cmsg_len < CMSG_LEN(0) || c->cmsg_len > m.msg_controllen ||
            (c->cmsg_len - CMSG_LEN(0)) % sizeof(int) != 0) {
            r = -EINVAL;
            goto free;
        }
        int n = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
        if (n > SCM_MAX_FD) {
            r = -EINVAL;
            goto free;
        }
        int *fds = (int *)CMSG_DATA(c);
        for (int i = 0; i < n; i++) {
            files[i] = fdtable_get(&thread_current()->proc->fds, fds[i]);
            if (!files[i]) {
                r = -EBADF;
                goto release;
            }
            nfiles++;
        }
    }
    sm.data = buf;
    sm.len = total;
    sm.files = files;
    sm.nfiles = nfiles;
    r = socket_sendmsg(s, &sm);
    if (r >= 0)
        nfiles = 0;                 /* references consumed by the backend */
release:
    for (int i = 0; i < nfiles; i++)
        file_put(files[i]);
free:
    kfree(buf);
out:
    file_put(f);
    return r;
}

/* recvmsg(fd, msg, flags) */
long sys_recvmsg(struct trapframe *tf)
{
    long r;
    struct file *f = socket_file_get((int)SYSARG0(tf), &r);
    if (!f)
        return r;
    uintptr_t uaddr = SYSARG1(tf);
    struct msghdr m;
    struct iovec iov[MSG_MAX_IOV];
    size_t total;
    r = msghdr_from_user(uaddr, &m, iov, true, &total);
    if (r < 0)
        goto out;
    if (total > MSG_MAX)
        total = MSG_MAX;
    if (m.msg_name && (!user_range_ok((uintptr_t)m.msg_name, m.msg_namelen, true))) {
        r = -EFAULT;
        goto out;
    }
    char *buf = kmalloc(total ? total : 1);
    if (!buf) {
        r = -ENOMEM;
        goto out;
    }
    struct file *files[SCM_MAX_FD];
    struct sockaddr_storage addr;
    struct socket_msg sm = {
        .data = buf, .len = total, .addr = &addr, .files = files, .nfiles = SCM_MAX_FD,
        .flags = (int)SYSARG2(tf),
    };
    r = socket_recvmsg(f->priv, &sm);
    if (r < 0) {
        kfree(buf);
        goto out;
    }
    size_t left = MIN((size_t)r, total), off = 0;
    for (size_t i = 0; i < m.msg_iovlen && left; i++) {
        size_t k = MIN(left, iov[i].iov_len);
        memcpy(iov[i].iov_base, buf + off, k);
        off += k;
        left -= k;
    }
    kfree(buf);
    int flags = sm.rflags;
    /* The source address, truncated to the room offered. */
    if (m.msg_name) {
        socklen_t n = MIN(m.msg_namelen, sm.addrlen);
        if (n)
            memcpy(m.msg_name, &addr, n);
        m.msg_namelen = sm.addrlen;
    } else {
        m.msg_namelen = 0;
    }
    /* Deliver descriptors into the control buffer, or drop them. */
    int nfiles = sm.nfiles;
    size_t need = CMSG_SPACE(sizeof(int) * (size_t)nfiles);
    if (nfiles && m.msg_control && m.msg_controllen >= need && user_range_ok((uintptr_t)m.msg_control, need, true)) {
        uint8_t ctl[CMSG_SPACE(sizeof(int) * SCM_MAX_FD)];
        memset(ctl, 0, sizeof ctl);
        struct cmsghdr *c = (struct cmsghdr *)ctl;
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        int *fds = (int *)CMSG_DATA(c);
        int installed = 0;
        for (int i = 0; i < nfiles; i++) {
            long fd = install(files[i]);       /* releases the file on failure */
            if (fd < 0) {
                for (int j = i + 1; j < nfiles; j++)
                    file_put(files[j]);
                flags |= MSG_CTRUNC;
                break;
            }
            fds[installed++] = (int)fd;
        }
        c->cmsg_len = CMSG_LEN(sizeof(int) * (size_t)installed);
        memcpy(m.msg_control, ctl, need);
        m.msg_controllen = need;
    } else {
        for (int i = 0; i < nfiles; i++)
            file_put(files[i]);
        m.msg_controllen = 0;
        if (nfiles)
            flags |= MSG_CTRUNC;
    }
    m.msg_flags = flags;
    memcpy((void *)uaddr, &m, sizeof m);
out:
    file_put(f);
    return r;
}

/* memfd_create(name, flags): the name is informational. */
long sys_memfd_create(struct trapframe *tf)
{
    int flags = (int)SYSARG1(tf);
    struct file *f;
    int r = shm_create_anon(&f);
    return r < 0 ? r : install_flags(f, (flags & MFD_CLOEXEC) ? O_CLOEXEC : 0);
}

long sys_ftruncate(struct trapframe *tf)
{
    struct file *f = fdtable_get(&thread_current()->proc->fds, (int)SYSARG0(tf));
    if (!f)
        return -EBADF;
    long r = f->ops && f->ops->truncate ? f->ops->truncate(f, SYSARG1(tf)) : -EINVAL;
    file_put(f);
    return r;
}

long sys_eventfd(struct trapframe *tf)
{
    int flags = socket_flags((long)SYSARG1(tf));
    struct file *f;
    int r = eventfd_create(SYSARG0(tf), flags, &f);
    return r < 0 ? r : install_flags(f, flags);
}

long sys_timerfd_create(struct trapframe *tf)
{
    int flags = socket_flags((long)SYSARG1(tf));
    struct file *f;
    int r = timerfd_create(flags, &f);
    return r < 0 ? r : install_flags(f, flags);
}

/* timerfd_settime(fd, spec) */
long sys_timerfd_settime(struct trapframe *tf)
{
    uintptr_t addr = SYSARG1(tf);
    if (!user_range_ok(addr, sizeof(struct timerfd_spec), false))
        return -EFAULT;
    struct timerfd_spec spec;
    memcpy(&spec, (void *)addr, sizeof spec);
    struct file *f = fdtable_get(&thread_current()->proc->fds, (int)SYSARG0(tf));
    if (!f)
        return -EBADF;
    long r = timerfd_settime(f, spec.initial_ms, spec.interval_ms);
    file_put(f);
    return r;
}

long sys_timerfd_gettime(struct trapframe *tf)
{
    uintptr_t addr = SYSARG1(tf);
    if (!user_range_ok(addr, sizeof(struct timerfd_spec), true))
        return -EFAULT;
    struct file *f = fdtable_get(&thread_current()->proc->fds, (int)SYSARG0(tf));
    if (!f)
        return -EBADF;
    struct timerfd_spec spec;
    long r = timerfd_gettime(f, &spec.initial_ms, &spec.interval_ms);
    file_put(f);
    if (r == 0)
        memcpy((void *)addr, &spec, sizeof spec);
    return r;
}
