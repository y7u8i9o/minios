#include <syscall/syscalls.h>
#include <arch/trap.h>
#include <sched/thread.h>
#include <sched/proc.h>
#include <fs/vfs.h>
#include <fs/fdtable.h>
#include <ipc/mqueue.h>
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

/* socket(domain, type, flags) */
long sys_socket(struct trapframe *tf)
{
    if (SYSARG0(tf) != AF_UNIX)
        return -EAFNOSUPPORT;
    if ((SYSARG1(tf) & 0xff) != SOCK_STREAM)
        return -EPROTONOSUPPORT;
    int flags = socket_flags((long)SYSARG1(tf) | (long)SYSARG2(tf));
    struct file *f;
    int r = socket_create(flags, &f);
    return r < 0 ? r : install_flags(f, flags);
}

/* socketpair(domain, type, fds[2]) */
long sys_socketpair(struct trapframe *tf)
{
    if (SYSARG0(tf) != AF_UNIX)
        return -EAFNOSUPPORT;
    uintptr_t ufds = SYSARG2(tf);
    if (!user_range_ok(ufds, 2 * sizeof(int), true))
        return -EFAULT;
    int flags = socket_flags((long)SYSARG1(tf));
    struct file *a, *b;
    int r = socket_pair(flags, &a, &b);
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

static long sock_name_from_user(uintptr_t addr, size_t len, char *name)
{
    if (len < sizeof(struct sockaddr_un) || !user_range_ok(addr, sizeof(struct sockaddr_un), false))
        return -EINVAL;
    struct sockaddr_un sa;
    memcpy(&sa, (void *)addr, sizeof sa);
    if (sa.sun_family != AF_UNIX)
        return -EAFNOSUPPORT;
    sa.sun_path[SOCK_NAME_MAX - 1] = '\0';
    strlcpy(name, sa.sun_path, SOCK_NAME_MAX);
    return 0;
}

static struct file *socket_file(int fd, long *err)
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
    char name[SOCK_NAME_MAX];
    long r = sock_name_from_user(SYSARG1(tf), SYSARG2(tf), name);
    if (r < 0)
        return r;
    struct file *f = socket_file((int)SYSARG0(tf), &r);
    if (!f)
        return r;
    r = socket_bind(f, name);
    file_put(f);
    return r;
}

long sys_listen(struct trapframe *tf)
{
    long r;
    struct file *f = socket_file((int)SYSARG0(tf), &r);
    if (!f)
        return r;
    r = socket_listen(f, (int)SYSARG1(tf));
    file_put(f);
    return r;
}

/* accept(fd, addr, addrlen, flags) */
long sys_accept(struct trapframe *tf)
{
    long r;
    struct file *f = socket_file((int)SYSARG0(tf), &r);
    if (!f)
        return r;
    int flags = socket_flags((long)SYSARG3(tf));
    struct file *nf;
    r = socket_accept(f, flags, &nf);
    file_put(f);
    return r < 0 ? r : install_flags(nf, flags);
}

long sys_connect(struct trapframe *tf)
{
    char name[SOCK_NAME_MAX];
    long r = sock_name_from_user(SYSARG1(tf), SYSARG2(tf), name);
    if (r < 0)
        return r;
    struct file *f = socket_file((int)SYSARG0(tf), &r);
    if (!f)
        return r;
    r = socket_connect(f, name);
    file_put(f);
    return r;
}

long sys_shutdown(struct trapframe *tf)
{
    long r;
    struct file *f = socket_file((int)SYSARG0(tf), &r);
    if (!f)
        return r;
    r = socket_shutdown(f, (int)SYSARG1(tf));
    file_put(f);
    return r;
}

/* Gather the iovec of a user msghdr into one kernel buffer (at most 64
 * KiB per call) and collect the SCM_RIGHTS descriptors. */
#define MSG_MAX 65536

static long msghdr_from_user(uintptr_t addr, struct msghdr *m, struct iovec *iov, int max_iov)
{
    if (!user_range_ok(addr, sizeof *m, false))
        return -EFAULT;
    memcpy(m, (void *)addr, sizeof *m);
    if (m->msg_iovlen > (size_t)max_iov)
        return -EINVAL;
    if (m->msg_iovlen && !user_range_ok((uintptr_t)m->msg_iov, m->msg_iovlen * sizeof *iov, false))
        return -EFAULT;
    memcpy(iov, m->msg_iov, m->msg_iovlen * sizeof *iov);
    return 0;
}

long sys_sendmsg(struct trapframe *tf)
{
    long r;
    struct file *f = socket_file((int)SYSARG0(tf), &r);
    if (!f)
        return r;
    struct msghdr m;
    struct iovec iov[16];
    r = msghdr_from_user(SYSARG1(tf), &m, iov, 16);
    if (r < 0)
        goto out;
    size_t total = 0;
    for (size_t i = 0; i < m.msg_iovlen; i++)
        total += iov[i].iov_len;
    if (total > MSG_MAX) {
        r = -EMSGSIZE;
        goto out;
    }
    char *buf = kmalloc(total ? total : 1);
    if (!buf) {
        r = -ENOMEM;
        goto out;
    }
    size_t off = 0;
    for (size_t i = 0; i < m.msg_iovlen; i++) {
        if (!user_range_ok((uintptr_t)iov[i].iov_base, iov[i].iov_len, false)) {
            kfree(buf);
            r = -EFAULT;
            goto out;
        }
        memcpy(buf + off, iov[i].iov_base, iov[i].iov_len);
        off += iov[i].iov_len;
    }
    struct file *files[SCM_MAX_FD];
    int nfiles = 0;
    if (m.msg_controllen) {
        if (!user_range_ok((uintptr_t)m.msg_control, m.msg_controllen, false)) {
            kfree(buf);
            r = -EFAULT;
            goto out;
        }
        uint8_t ctl[CMSG_SPACE(sizeof(int) * SCM_MAX_FD)];
        if (m.msg_controllen > sizeof ctl) {
            kfree(buf);
            r = -EINVAL;
            goto out;
        }
        memcpy(ctl, m.msg_control, m.msg_controllen);
        struct cmsghdr *c = (struct cmsghdr *)ctl;
        if (m.msg_controllen >= sizeof *c && c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS &&
            c->cmsg_len >= CMSG_LEN(0) && c->cmsg_len <= m.msg_controllen) {
            int n = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            if (n > SCM_MAX_FD) {
                kfree(buf);
                r = -EINVAL;
                goto out;
            }
            int *fds = (int *)CMSG_DATA(c);
            for (int i = 0; i < n; i++) {
                files[i] = fdtable_get(&thread_current()->proc->fds, fds[i]);
                if (!files[i]) {
                    for (int j = 0; j < i; j++)
                        file_put(files[j]);
                    kfree(buf);
                    r = -EBADF;
                    goto out;
                }
                nfiles++;
            }
        }
    }
    r = socket_send(f, buf, total, files, nfiles);
    if (r < 0)
        for (int i = 0; i < nfiles; i++)
            file_put(files[i]);
    kfree(buf);
out:
    file_put(f);
    return r;
}

long sys_recvmsg(struct trapframe *tf)
{
    long r;
    struct file *f = socket_file((int)SYSARG0(tf), &r);
    if (!f)
        return r;
    uintptr_t uaddr = SYSARG1(tf);
    struct msghdr m;
    struct iovec iov[16];
    r = msghdr_from_user(uaddr, &m, iov, 16);
    if (r < 0)
        goto out;
    size_t total = 0;
    for (size_t i = 0; i < m.msg_iovlen; i++) {
        if (!user_range_ok((uintptr_t)iov[i].iov_base, iov[i].iov_len, true)) {
            r = -EFAULT;
            goto out;
        }
        total += iov[i].iov_len;
    }
    if (total > MSG_MAX)
        total = MSG_MAX;
    char *buf = kmalloc(total ? total : 1);
    if (!buf) {
        r = -ENOMEM;
        goto out;
    }
    struct file *files[SCM_MAX_FD];
    int nfiles = SCM_MAX_FD;
    r = socket_recv(f, buf, total, files, &nfiles);
    if (r < 0) {
        kfree(buf);
        goto out;
    }
    size_t left = (size_t)r, off = 0;
    for (size_t i = 0; i < m.msg_iovlen && left; i++) {
        size_t k = MIN(left, iov[i].iov_len);
        memcpy(iov[i].iov_base, buf + off, k);
        off += k;
        left -= k;
    }
    kfree(buf);
    /* Deliver descriptors into the control buffer, or drop them. */
    int flags = 0;
    size_t need = CMSG_SPACE(sizeof(int) * (size_t)nfiles);
    if (nfiles && m.msg_control && m.msg_controllen >= need && user_range_ok((uintptr_t)m.msg_control, need, true)) {
        uint8_t ctl[CMSG_SPACE(sizeof(int) * SCM_MAX_FD)];
        memset(ctl, 0, sizeof ctl);
        struct cmsghdr *c = (struct cmsghdr *)ctl;
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int) * (size_t)nfiles);
        int *fds = (int *)CMSG_DATA(c);
        int installed = 0;
        for (int i = 0; i < nfiles; i++) {
            long fd = install(files[i]);
            if (fd < 0)
                break;
            fds[installed++] = (int)fd;
        }
        for (int i = installed; i < nfiles && installed < nfiles; i++)
            ;   /* files that could not be installed were released by install */
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
