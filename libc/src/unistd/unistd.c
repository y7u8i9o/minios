#include <unistd.h>
#include <sys/utsname.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/thread.h>
#include <minios/syscall.h>
#include <sys/mount.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <stdio.h>
#include <stdarg.h>

ssize_t write(int fd, const void *buf, size_t n)
{
    return syscall3(SYS_write, fd, buf, n);
}

ssize_t read(int fd, void *buf, size_t n)
{
    return syscall3(SYS_read, fd, buf, n);
}

pid_t fork(void)
{
    return (pid_t)syscall0(SYS_fork);
}

int execve(const char *path, char *const argv[], char *const envp[])
{
    return (int)syscall3(SYS_execve, path, argv, envp);
}

int execv(const char *path, char *const argv[])
{
    return execve(path, argv, environ);
}

int execvp(const char *file, char *const argv[])
{
    if (strchr(file, '/'))
        return execve(file, argv, environ);
    const char *path = getenv("PATH");
    if (!path)
        path = "/bin";
    char buf[512];
    while (*path) {
        const char *sep = strchr(path, ':');
        size_t n = sep ? (size_t)(sep - path) : strlen(path);
        if (n + 1 + strlen(file) + 1 <= sizeof buf) {
            memcpy(buf, path, n);
            buf[n] = '/';
            strcpy(buf + n + 1, file);
            execve(buf, argv, environ);
            if (errno != ENOENT)
                return -1;
        }
        if (!sep)
            break;
        path = sep + 1;
    }
    errno = ENOENT;
    return -1;
}

void _exit(int status)
{
    for (;;)
        __syscall6(SYS_exit, status, 0, 0, 0, 0, 0);
}

pid_t getpid(void)
{
    return (pid_t)syscall0(SYS_getpid);
}

pid_t getppid(void)
{
    return (pid_t)syscall0(SYS_getppid);
}

void *sbrk(intptr_t increment)
{
    long r = syscall1(SYS_sbrk, increment);
    return r == -1 ? (void *)-1 : (void *)r;
}

int chdir(const char *path)
{
    return (int)syscall1(SYS_chdir, path);
}

char *getcwd(char *buf, size_t size)
{
    return syscall2(SYS_getcwd, buf, size) < 0 ? NULL : buf;
}

int sched_yield(void)
{
    return (int)syscall0(SYS_yield);
}

pid_t wait4(pid_t pid, int *status, int options, void *rusage)
{
    return (pid_t)syscall4(SYS_wait4, pid, status, options, rusage);
}

pid_t waitpid(pid_t pid, int *status, int options)
{
    return wait4(pid, status, options, NULL);
}

pid_t wait(int *status)
{
    return wait4(-1, status, 0, NULL);
}


int gettid(void)
{
    return (int)syscall0(SYS_gettid);
}

int thread_create(thread_t *out, void (*fn)(void *), void *arg, void *stack, size_t stack_size)
{
    if (!out || !fn || !stack || stack_size < 16 ||
        (unsigned long)stack + stack_size < (unsigned long)stack) {
        errno = EINVAL;
        return -1;
    }
    unsigned long top = ((unsigned long)stack + stack_size) & ~15UL;
    if (top - (unsigned long)stack < sizeof(unsigned long)) {
        errno = EINVAL;
        return -1;
    }
    /* The kernel enters the C function directly, without a call instruction.
     * Supply its return-address slot so RSP is 8 modulo 16 at function entry,
     * as required by the x86-64 ABI (including aligned SIMD stack accesses). */
    top -= sizeof(unsigned long);
    *(unsigned long *)top = 0;
    long r = syscall3(SYS_thread_create, fn, arg, top);
    if (r < 0)
        return -1;
    *out = (thread_t)r;
    return 0;
}

void thread_exit(int code)
{
    for (;;)
        __syscall6(SYS_thread_exit, code, 0, 0, 0, 0, 0);
}

int thread_join(thread_t t, int *code)
{
    return (int)syscall2(SYS_thread_join, t, code);
}

int open(const char *path, int flags, ...)
{
    unsigned mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, unsigned);
        va_end(ap);
    }
    return (int)syscall3(SYS_open, path, flags, mode);
}

int creat(const char *path, unsigned mode)
{
    return open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
}

int close(int fd)
{
    return (int)syscall1(SYS_close, fd);
}

off_t lseek(int fd, off_t off, int whence)
{
    return (off_t)syscall3(SYS_lseek, fd, off, whence);
}

int dup(int fd)
{
    return (int)syscall1(SYS_dup, fd);
}

int dup2(int fd, int fd2)
{
    return (int)syscall2(SYS_dup2, fd, fd2);
}

int pipe(int fds[2])
{
    return (int)syscall1(SYS_pipe, fds);
}

int unlink(const char *path)
{
    return (int)syscall1(SYS_unlink, path);
}

int rmdir(const char *path)
{
    return (int)syscall1(SYS_rmdir, path);
}

int link(const char *oldpath, const char *newpath)
{
    return (int)syscall2(SYS_link, oldpath, newpath);
}

int rename(const char *oldpath, const char *newpath)
{
    return (int)syscall2(SYS_rename, oldpath, newpath);
}

int remove(const char *path)
{
    struct stat st;
    if (lstat(path, &st) < 0)
        return -1;
    return S_ISDIR(st.st_mode) ? rmdir(path) : unlink(path);
}

int stat(const char *path, struct stat *st)
{
    return (int)syscall2(SYS_stat, path, st);
}

int fstat(int fd, struct stat *st)
{
    return (int)syscall2(SYS_fstat, fd, st);
}

int mkdir(const char *path, mode_t mode)
{
    return (int)syscall2(SYS_mkdir, path, mode);
}

long getdents(int fd, struct dirent *buf, size_t count)
{
    return syscall3(SYS_getdents, fd, buf, count);
}

int mount(const char *source, const char *target, const char *fstype)
{
    return (int)syscall3(SYS_mount, source, target, fstype);
}

int umount(const char *target)
{
    return (int)syscall1(SYS_umount, target);
}

void sync(void)
{
    syscall0(SYS_sync);
}

int setpgid(pid_t pid, pid_t pgid)
{
    return (int)syscall2(SYS_setpgid, pid, pgid);
}

pid_t getpgid(pid_t pid)
{
    return (pid_t)syscall1(SYS_getpgid, pid);
}

pid_t getpgrp(void)
{
    return getpgid(0);
}

int tcsetpgrp(int fd, pid_t pgrp)
{
    return (int)syscall2(SYS_tcsetpgrp, fd, pgrp);
}

pid_t tcgetpgrp(int fd)
{
    return (pid_t)syscall1(SYS_tcgetpgrp, fd);
}

int isatty(int fd)
{
    struct stat st;
    if (fstat(fd, &st) < 0)
        return 0;
    if (!S_ISCHR(st.st_mode)) {
        errno = ENOTTY;
        return 0;
    }
    return 1;
}

/* Directory streams read a batch of entries at a time. */
struct DIR {
    int fd;
    struct dirent buf[8];
    int count, index;
};

DIR *fdopendir(int fd)
{
    DIR *d = calloc(1, sizeof *d);
    if (!d)
        return NULL;
    d->fd = fd;
    return d;
}

DIR *opendir(const char *path)
{
    int fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0)
        return NULL;
    DIR *d = fdopendir(fd);
    if (!d)
        close(fd);
    return d;
}

struct dirent *readdir(DIR *d)
{
    if (d->index >= d->count) {
        long n = getdents(d->fd, d->buf, sizeof d->buf);
        if (n <= 0)
            return NULL;
        d->count = (int)(n / (long)sizeof(struct dirent));
        d->index = 0;
    }
    return &d->buf[d->index++];
}

int closedir(DIR *d)
{
    int r = close(d->fd);
    free(d);
    return r;
}

int dirfd(DIR *d)
{
    return d->fd;
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    long r = __syscall6(SYS_mmap, (long)addr, (long)len, prot, flags, fd, off);
    if (r < 0 && r > -4096) {
        errno = (int)-r;
        return MAP_FAILED;
    }
    return (void *)r;
}

int munmap(void *addr, size_t len)
{
    return (int)syscall2(SYS_munmap, addr, len);
}

int mprotect(void *addr, size_t len, int prot)
{
    return (int)syscall3(SYS_mprotect, addr, len, prot);
}

int msync(void *addr, size_t len, int flags)
{
    return (int)syscall3(SYS_msync, addr, len, flags);
}

int madvise(void *addr, size_t len, int advice)
{
    return (int)syscall3(SYS_madvise, addr, len, advice);
}

int ioctl(int fd, unsigned long req, ...)
{
    va_list ap;
    va_start(ap, req);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    return (int)syscall3(SYS_ioctl, fd, req, arg);
}

int tcgetattr(int fd, struct termios *t)
{
    return ioctl(fd, TCGETS, t);
}

int tcsetattr(int fd, int action, const struct termios *t)
{
    return ioctl(fd, TCSETS, t);
}

int sleep_ms(unsigned long ms)
{
    return (int)syscall1(SYS_sleep_ms, ms);
}

unsigned sleep(unsigned seconds)
{
    sleep_ms((unsigned long)seconds * 1000);
    return 0;
}

int usleep(unsigned long usec)
{
    return sleep_ms((usec + 999) / 1000);
}

long uptime_ms(void)
{
    return syscall0(SYS_uptime_ms);
}

int openpty(int *master, char *slave_path, size_t size)
{
    int m = open("/dev/ptmx", O_RDWR);
    if (m < 0)
        return -1;
    int n;
    if (ioctl(m, TIOCGPTN, &n) < 0) {
        close(m);
        return -1;
    }
    snprintf(slave_path, size, "/dev/pts%d", n);
    *master = m;
    return 0;
}

int nproc(void)
{
    return (int)syscall0(SYS_nproc);
}

int getcpu(void)
{
    return (int)syscall0(SYS_getcpu);
}

int uname(struct utsname *buf)
{
    return (int)syscall1(SYS_uname, buf);
}

int fchown(int fd, uid_t owner, gid_t group)
{
    return 0;
}

int chown(const char *path, uid_t owner, gid_t group)
{
    return 0;
}

int fchmod(int fd, mode_t mode)
{
    return 0;
}

int chmod(const char *path, mode_t mode)
{
    return 0;
}

#include <sys/uio.h>
#include <limits.h>

ssize_t readv(int fd, const struct iovec *iov, int count)
{
    ssize_t total = 0;
    for (int i = 0; i < count; i++) {
        if (iov[i].iov_len == 0)
            continue;
        ssize_t n = read(fd, iov[i].iov_base, iov[i].iov_len);
        if (n < 0)
            return total > 0 ? total : n;
        total += n;
        if ((size_t)n < iov[i].iov_len)
            break;
    }
    return total;
}

ssize_t writev(int fd, const struct iovec *iov, int count)
{
    ssize_t total = 0;
    for (int i = 0; i < count; i++) {
        size_t done = 0;
        while (done < iov[i].iov_len) {
            ssize_t n = write(fd, (const char *)iov[i].iov_base + done, iov[i].iov_len - done);
            if (n < 0)
                return total > 0 ? total : n;
            done += (size_t)n;
            total += n;
        }
    }
    return total;
}

int lstat(const char *path, struct stat *st)
{
    return (int)syscall2(SYS_lstat, path, st);
}

int access(const char *path, int mode)
{
    struct stat st;
    return stat(path, &st);
}

size_t confstr(int name, char *buf, size_t len)
{
    if (name != _CS_PATH) {
        errno = EINVAL;
        return 0;
    }
    const char *value = "/bin";
    if (buf != NULL && len > 0) {
        strncpy(buf, value, len - 1);
        buf[len - 1] = '\0';
    }
    return strlen(value) + 1;
}

int utimensat(int dirfd, const char *path, const struct timespec times[2], int flags)
{
    return (int)syscall4(SYS_utimensat, dirfd, path, times, flags);
}

/* Build the absolute form of path without ".", ".." and symbolic links.
 * The components are resolved one at a time: a symbolic link is replaced
 * by its target, which restarts from the root when it is absolute and
 * otherwise continues in the directory holding the link, and ".." removes
 * the last component of the result so far, which never names a link.
 * Every component but the last must be a directory, and the last must
 * exist. At most SYMLOOP_MAX links are followed (ELOOP). */
char *realpath(const char *path, char *resolved)
{
    char out[PATH_MAX], rest[2 * PATH_MAX], target[PATH_MAX];
    size_t n = 0;
    int links = 0;
    if (path == NULL) {
        errno = EINVAL;
        return NULL;
    }
    if (*path == '\0') {
        errno = ENOENT;
        return NULL;
    }
    if (strlen(path) >= sizeof rest) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    strcpy(rest, path);
    if (path[0] != '/') {
        if (getcwd(out, sizeof out) == NULL)
            return NULL;
        n = strlen(out);
        if (n == 1)
            n = 0;
    }
    size_t i = 0;
    while (rest[i] != '\0') {
        while (rest[i] == '/')
            i++;
        if (rest[i] == '\0')
            break;
        size_t start = i;
        while (rest[i] != '\0' && rest[i] != '/')
            i++;
        size_t len = i - start;
        if (len == 1 && rest[start] == '.')
            continue;
        if (len == 2 && rest[start] == '.' && rest[start + 1] == '.') {
            while (n > 0 && out[n - 1] != '/')
                n--;
            if (n > 0)
                n--;
            continue;
        }
        if (n + 1 + len >= sizeof out) {
            errno = ENAMETOOLONG;
            return NULL;
        }
        size_t prev = n;
        out[n++] = '/';
        memcpy(out + n, rest + start, len);
        n += len;
        out[n] = '\0';
        struct stat st;
        if (lstat(out, &st) < 0)
            return NULL;
        if (S_ISLNK(st.st_mode)) {
            if (++links > SYMLOOP_MAX) {
                errno = ELOOP;
                return NULL;
            }
            ssize_t t = readlink(out, target, sizeof target - 1);
            if (t < 0)
                return NULL;
            target[t] = '\0';
            size_t tail = strlen(rest + i);
            if ((size_t)t + tail >= sizeof rest) {
                errno = ENAMETOOLONG;
                return NULL;
            }
            memmove(rest + t, rest + i, tail + 1);
            memcpy(rest, target, (size_t)t);
            i = 0;
            n = target[0] == '/' ? 0 : prev;
            continue;
        }
        if (!S_ISDIR(st.st_mode)) {
            size_t j = i;
            while (rest[j] == '/')
                j++;
            if (rest[j] != '\0') {
                errno = ENOTDIR;
                return NULL;
            }
        }
    }
    if (n == 0)
        out[n++] = '/';
    out[n] = '\0';
    if (resolved == NULL) {
        resolved = malloc(n + 1);
        if (resolved == NULL)
            return NULL;
    }
    memcpy(resolved, out, n + 1);
    return resolved;
}

int openat(int dirfd, const char *path, int flags, ...)
{
    unsigned mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, unsigned);
        va_end(ap);
    }
    return (int)syscall4(SYS_openat, dirfd, path, flags, mode);
}

int fstatat(int dirfd, const char *path, struct stat *st, int flags)
{
    return (int)syscall4(SYS_fstatat, dirfd, path, st, flags);
}

int lchown(const char *path, uid_t owner, gid_t group)
{
    return 0;
}

uid_t getuid(void) { return 0; }
uid_t geteuid(void) { return 0; }
gid_t getgid(void) { return 0; }
gid_t getegid(void) { return 0; }

int symlink(const char *target, const char *path)
{
    return (int)syscall2(SYS_symlink, target, path);
}

int symlinkat(const char *target, int dirfd, const char *path)
{
    return (int)syscall3(SYS_symlinkat, target, dirfd, path);
}

ssize_t readlink(const char *path, char *buf, size_t size)
{
    return (ssize_t)syscall3(SYS_readlink, path, buf, size);
}

ssize_t readlinkat(int dirfd, const char *path, char *buf, size_t size)
{
    return (ssize_t)syscall4(SYS_readlinkat, dirfd, path, buf, size);
}

int mknod(const char *path, mode_t mode, dev_t dev)
{
    errno = EPERM;
    return -1;
}

int mkfifo(const char *path, mode_t mode)
{
    errno = EPERM;
    return -1;
}

/* Collect the variadic arguments of execl and execlp into a vector. */
static int exec_list(const char *file, const char *arg, va_list ap, int search)
{
    va_list count;
    va_copy(count, ap);
    int n = 1;
    while (va_arg(count, const char *) != NULL)
        n++;
    va_end(count);
    char **argv = malloc((size_t)(n + 1) * sizeof *argv);
    if (argv == NULL)
        return -1;
    argv[0] = (char *)arg;
    for (int i = 1; i <= n; i++)
        argv[i] = va_arg(ap, char *);
    int r = search ? execvp(file, argv) : execv(file, argv);
    free(argv);
    return r;
}

int execl(const char *path, const char *arg, ...)
{
    va_list ap;
    va_start(ap, arg);
    int r = exec_list(path, arg, ap, 0);
    va_end(ap);
    return r;
}

int execlp(const char *file, const char *arg, ...)
{
    va_list ap;
    va_start(ap, arg);
    int r = exec_list(file, arg, ap, 1);
    va_end(ap);
    return r;
}
