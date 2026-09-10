#include <sys/ipc.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <stdarg.h>
#include <unistd.h>
#include <minios/syscall.h>

int mq_open(const char *name, int flags)
{
    return (int)syscall2(SYS_mq_open, name, flags);
}

int mq_unlink(const char *name)
{
    return (int)syscall1(SYS_mq_unlink, name);
}

ssize_t mq_send(int fd, const void *msg, size_t len)
{
    return write(fd, msg, len);
}

ssize_t mq_recv(int fd, void *msg, size_t len)
{
    return read(fd, msg, len);
}

int shm_open(const char *name, int flags, size_t size)
{
    return (int)syscall3(SYS_shm_open, name, flags, size);
}

int shm_unlink(const char *name)
{
    return (int)syscall1(SYS_shm_unlink, name);
}

int poll(struct pollfd *fds, unsigned n, int timeout_ms)
{
    return (int)syscall3(SYS_poll, fds, n, timeout_ms);
}

/* ---- M23 ---- */

int socket(int domain, int type, int protocol)
{
    return (int)syscall3(SYS_socket, domain, type, protocol);
}

int socketpair(int domain, int type, int protocol, int fds[2])
{
    return (int)syscall3(SYS_socketpair, domain, type, fds);
}

int bind(int fd, const struct sockaddr *addr, socklen_t len)
{
    return (int)syscall3(SYS_bind, fd, addr, len);
}

int listen(int fd, int backlog)
{
    return (int)syscall2(SYS_listen, fd, backlog);
}

int accept4(int fd, struct sockaddr *addr, socklen_t *len, int flags)
{
    return (int)syscall4(SYS_accept, fd, addr, len, flags);
}

int accept(int fd, struct sockaddr *addr, socklen_t *len)
{
    return accept4(fd, addr, len, 0);
}

int connect(int fd, const struct sockaddr *addr, socklen_t len)
{
    return (int)syscall3(SYS_connect, fd, addr, len);
}

int shutdown(int fd, int how)
{
    return (int)syscall2(SYS_shutdown, fd, how);
}

ssize_t sendmsg(int fd, const struct msghdr *msg, int flags)
{
    return syscall3(SYS_sendmsg, fd, msg, flags);
}

ssize_t recvmsg(int fd, struct msghdr *msg, int flags)
{
    return syscall3(SYS_recvmsg, fd, msg, flags);
}

ssize_t sendto(int fd, const void *buf, size_t n, int flags, const struct sockaddr *addr, socklen_t len)
{
    struct iovec iov = { (void *)buf, n };
    struct msghdr m = { (void *)addr, addr ? len : 0, &iov, 1, NULL, 0, 0 };
    return sendmsg(fd, &m, flags);
}

ssize_t recvfrom(int fd, void *buf, size_t n, int flags, struct sockaddr *addr, socklen_t *len)
{
    struct iovec iov = { buf, n };
    struct msghdr m = { addr, addr && len ? *len : 0, &iov, 1, NULL, 0, 0 };
    ssize_t r = recvmsg(fd, &m, flags);
    if (r >= 0 && addr && len)
        *len = m.msg_namelen;
    return r;
}

ssize_t send(int fd, const void *buf, size_t n, int flags)
{
    return sendto(fd, buf, n, flags, NULL, 0);
}

ssize_t recv(int fd, void *buf, size_t n, int flags)
{
    return recvfrom(fd, buf, n, flags, NULL, NULL);
}

int getsockname(int fd, struct sockaddr *addr, socklen_t *len)
{
    return (int)syscall3(SYS_getsockname, fd, addr, len);
}

int getpeername(int fd, struct sockaddr *addr, socklen_t *len)
{
    return (int)syscall3(SYS_getpeername, fd, addr, len);
}

int setsockopt(int fd, int level, int name, const void *val, socklen_t len)
{
    return (int)syscall5(SYS_setsockopt, fd, level, name, val, len);
}

int getsockopt(int fd, int level, int name, void *val, socklen_t *len)
{
    return (int)syscall5(SYS_getsockopt, fd, level, name, val, len);
}

int memfd_create(const char *name, unsigned flags)
{
    return (int)syscall2(SYS_memfd_create, name, flags);
}

int eventfd(unsigned initval, int flags)
{
    return (int)syscall2(SYS_eventfd, initval, flags);
}

int eventfd_read(int fd, uint64_t *value)
{
    return read(fd, value, 8) == 8 ? 0 : -1;
}

int eventfd_write(int fd, uint64_t value)
{
    return write(fd, &value, 8) == 8 ? 0 : -1;
}

int timerfd_create(int flags)
{
    return (int)syscall2(SYS_timerfd_create, 0, flags);
}

int timerfd_settime(int fd, const struct timerfd_spec *spec)
{
    return (int)syscall2(SYS_timerfd_settime, fd, spec);
}

int timerfd_gettime(int fd, struct timerfd_spec *spec)
{
    return (int)syscall2(SYS_timerfd_gettime, fd, spec);
}

int fcntl(int fd, int cmd, ...)
{
    va_list ap;
    va_start(ap, cmd);
    long arg = va_arg(ap, long);
    va_end(ap);
    return (int)syscall3(SYS_fcntl, fd, cmd, arg);
}

int pipe2(int fds[2], int flags)
{
    return (int)syscall2(SYS_pipe2, fds, flags);
}

int ftruncate(int fd, long size)
{
    return (int)syscall2(SYS_ftruncate, fd, size);
}
