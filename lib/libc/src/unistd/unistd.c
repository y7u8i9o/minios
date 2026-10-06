#include <unistd.h>
#include <utime.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
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
#include <libc_arch.h>

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

/* A file that the kernel does not recognize as a program (ENOEXEC) is a
 * shell script, which execvp and execlp run with /bin/sh as POSIX asks. */
static int exec_script(const char *path, char *const argv[])
{
    int argc = 0;
    while (argv[argc])
        argc++;
    char *args[argc + 2];
    args[0] = "sh";
    args[1] = (char *)path;
    for (int i = 1; i <= argc; i++)
        args[i + 1] = argv[i];
    execve("/bin/sh", args, environ);
    return -1;
}

int execvp(const char *file, char *const argv[])
{
    if (strchr(file, '/')) {
        execve(file, argv, environ);
        return errno == ENOEXEC ? exec_script(file, argv) : -1;
    }
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
            if (errno == ENOEXEC)
                return exec_script(buf, argv);
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
    top = __arch_thread_stack_top(top);
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
    return (int)syscall4(SYS_mount, source, target, fstype, NULL);
}

int mount_options(const char *source, const char *target, const char *fstype, const char *options)
{
    return (int)syscall4(SYS_mount, source, target, fstype, options);
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
    if (action < TCSANOW || action > TCSAFLUSH) {
        errno = EINVAL;
        return -1;
    }
    if (action == TCSAFLUSH && ioctl(fd, TCFLSH, (void *)TCIFLUSH) < 0)
        return -1;
    return ioctl(fd, TCSETS, t);
}

int tcflush(int fd, int queue)
{
    return ioctl(fd, TCFLSH, (void *)(long)queue);
}

/* Output reaches the terminal when write returns. */
int tcdrain(int fd)
{
    struct termios t;
    return tcgetattr(fd, &t);
}

int tcsendbreak(int fd, int duration)
{
    return tcdrain(fd);
}

speed_t cfgetispeed(const struct termios *t) { return t->c_ispeed; }
speed_t cfgetospeed(const struct termios *t) { return t->c_ospeed; }

int cfsetispeed(struct termios *t, speed_t speed)
{
    if (speed > B38400) {
        errno = EINVAL;
        return -1;
    }
    t->c_ispeed = speed;
    return 0;
}

int cfsetospeed(struct termios *t, speed_t speed)
{
    if (speed > B38400) {
        errno = EINVAL;
        return -1;
    }
    t->c_ospeed = speed;
    return 0;
}

int cfsetspeed(struct termios *t, speed_t speed)
{
    return cfsetispeed(t, speed) < 0 ? -1 : cfsetospeed(t, speed);
}

void cfmakeraw(struct termios *t)
{
    t->c_iflag &= ~(tcflag_t)(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    t->c_oflag &= ~(tcflag_t)OPOST;
    t->c_lflag &= ~(tcflag_t)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    t->c_cflag = (t->c_cflag & ~(tcflag_t)(CSIZE | PARENB)) | CS8;
    t->c_cc[VMIN] = 1;
    t->c_cc[VTIME] = 0;
}

int gethostname(char *name, size_t size)
{
    struct utsname u;
    if (uname(&u) < 0)
        return -1;
    if (strlen(u.nodename) >= size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(name, u.nodename);
    return 0;
}

int fchdir(int fd)
{
    return (int)syscall1(SYS_fchdir, fd);
}

int ttyname_r(int fd, char *buf, size_t size)
{
    struct stat st, dst;
    if (!isatty(fd))
        return errno = ENOTTY;
    if (fstat(fd, &st) < 0)
        return errno;
    DIR *d = opendir("/dev");
    if (!d)
        return errno;
    struct dirent *e;
    int r = ENOTTY;
    char path[64];
    while ((e = readdir(d))) {
        snprintf(path, sizeof path, "/dev/%s", e->d_name);
        if (stat(path, &dst) == 0 && S_ISCHR(dst.st_mode) && dst.st_rdev == st.st_rdev) {
            r = strlen(path) < size ? 0 : ERANGE;
            if (r == 0)
                strcpy(buf, path);
            break;
        }
    }
    closedir(d);
    return errno = r;
}

char *ttyname(int fd)
{
    static char name[64];
    return ttyname_r(fd, name, sizeof name) == 0 ? name : NULL;
}

unsigned alarm(unsigned seconds)
{
    return (unsigned)syscall1(SYS_alarm, seconds);
}

pid_t setsid(void)
{
    return (pid_t)syscall0(SYS_setsid);
}

int chroot(const char *path)
{
    errno = ENOSYS;
    return -1;
}

pid_t getsid(pid_t pid)
{
    return (pid_t)syscall1(SYS_getsid, pid);
}

int getpriority(int which, id_t who)
{
    errno = 0;
    return 0;
}

int setpriority(int which, id_t who, int prio)
{
    if (prio != 0) {
        errno = EACCES;
        return -1;
    }
    return 0;
}

int utime(const char *path, const struct utimbuf *times)
{
    if (!times)
        return utimensat(AT_FDCWD, path, NULL, 0);
    struct timespec ts[2] = { { times->actime, 0 }, { times->modtime, 0 } };
    return utimensat(AT_FDCWD, path, ts, 0);
}

long sysconf(int name)
{
    switch (name) {
    case _SC_ARG_MAX: return 4096;
    case _SC_CHILD_MAX: return 64;
    case _SC_CLK_TCK: return 1000;
    case _SC_NGROUPS_MAX: return NGROUPS_MAX;
    case _SC_OPEN_MAX: {
        struct rlimit rl;
        return getrlimit(RLIMIT_NOFILE, &rl) == 0 ? (long)rl.rlim_cur : 64;
    }
    case _SC_PAGESIZE: return 4096;
    case _SC_LINE_MAX: return 2048;
    case _SC_LOGIN_NAME_MAX: return 33;
    case _SC_HOST_NAME_MAX: return 31;
    case _SC_NPROCESSORS_CONF:
    case _SC_NPROCESSORS_ONLN: return nproc();
    case _SC_GETPW_R_SIZE_MAX:
    case _SC_GETGR_R_SIZE_MAX: return 1024;
    case _SC_TTY_NAME_MAX: return 32;
    case _SC_SYMLOOP_MAX: return 8;
    }
    errno = EINVAL;
    return -1;
}

/* getpass reads a line without echo from /dev/tty, or from standard
 * input when there is no terminal, and returns it without the newline. */
char *getpass(const char *prompt)
{
    static char buf[128];
    int fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
    int in = fd >= 0 ? fd : 0, out = fd >= 0 ? fd : 2;
    struct termios saved, quiet;
    int tty = tcgetattr(in, &saved) == 0;
    if (tty) {
        quiet = saved;
        quiet.c_lflag &= ~(tcflag_t)(ECHO | ECHONL);
        tcsetattr(in, TCSAFLUSH, &quiet);
    }
    write(out, prompt, strlen(prompt));
    size_t n = 0;
    char c;
    ssize_t r = 0;
    while ((r = read(in, &c, 1)) == 1 && c != '\n')
        if (n + 1 < sizeof buf)
            buf[n++] = c;
    buf[n] = '\0';
    if (tty) {
        tcsetattr(in, TCSAFLUSH, &saved);
        write(out, "\n", 1);
    }
    if (fd >= 0)
        close(fd);
    return r < 0 && n == 0 ? NULL : buf;
}

int sleep_ms(unsigned long ms)
{
    return (int)syscall1(SYS_sleep_ms, ms);
}

/* sleep returns the unslept seconds, rounded up, when a signal ends the
 * sleep (POSIX). */
unsigned sleep(unsigned seconds)
{
    struct timespec request = { .tv_sec = seconds, .tv_nsec = 0 }, remain = { 0, 0 };
    if (nanosleep(&request, &remain) == 0)
        return 0;
    return (unsigned)remain.tv_sec + (remain.tv_nsec > 0);
}

int usleep(unsigned long usec)
{
    return sleep_ms((usec + 999) / 1000);
}

long uptime_ms(void)
{
    return syscall0(SYS_uptime_ms);
}

long uptime_us(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
        return uptime_ms() * 1000;
    return (long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

int openpty(int *master, int *slave, char *name, const struct termios *termp, const struct winsize *winp)
{
    int m = open("/dev/ptmx", O_RDWR);
    if (m < 0)
        return -1;
    int n;
    char path[32];
    if (ioctl(m, TIOCGPTN, &n) < 0) {
        close(m);
        return -1;
    }
    snprintf(path, sizeof path, "/dev/pts%d", n);
    int s = open(path, O_RDWR);
    if (s < 0) {
        int e = errno;
        close(m);
        errno = e;
        return -1;
    }
    if (termp)
        tcsetattr(s, TCSANOW, termp);
    if (winp)
        ioctl(m, TIOCSWINSZ, (void *)winp);
    if (name)
        strcpy(name, path);
    *master = m;
    *slave = s;
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
    return (int)syscall3(SYS_fchown, fd, owner, group);
}

int fchownat(int dirfd, const char *path, uid_t owner, gid_t group, int flags)
{
    return (int)syscall5(SYS_fchownat, dirfd, path, owner, group, flags);
}

int chown(const char *path, uid_t owner, gid_t group)
{
    return fchownat(AT_FDCWD, path, owner, group, 0);
}

int fchmod(int fd, mode_t mode)
{
    return (int)syscall2(SYS_fchmod, fd, mode);
}

int fchmodat(int dirfd, const char *path, mode_t mode, int flags)
{
    return (int)syscall4(SYS_fchmodat, dirfd, path, mode, flags);
}

int chmod(const char *path, mode_t mode)
{
    return fchmodat(AT_FDCWD, path, mode, 0);
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
    return faccessat(AT_FDCWD, path, mode, 0);
}

int faccessat(int dirfd, const char *path, int mode, int flags)
{
    return (int)syscall4(SYS_faccessat, dirfd, path, mode, flags);
}

size_t confstr(int name, char *buf, size_t len)
{
    if (name != _CS_PATH) {
        errno = EINVAL;
        return 0;
    }
    const char *value = "/usr/bin";
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
 * otherwise continues in the directory containing the link, and ".." removes
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
    return fchownat(AT_FDCWD, path, owner, group, AT_SYMLINK_NOFOLLOW);
}

int getresuid(uid_t *ruid, uid_t *euid, uid_t *suid)
{
    return (int)syscall3(SYS_getresuid, ruid, euid, suid);
}

int getresgid(gid_t *rgid, gid_t *egid, gid_t *sgid)
{
    return (int)syscall3(SYS_getresgid, rgid, egid, sgid);
}

uid_t getuid(void)
{
    uid_t r;
    getresuid(&r, NULL, NULL);
    return r;
}

uid_t geteuid(void)
{
    uid_t e;
    getresuid(NULL, &e, NULL);
    return e;
}

gid_t getgid(void)
{
    gid_t r;
    getresgid(&r, NULL, NULL);
    return r;
}

gid_t getegid(void)
{
    gid_t e;
    getresgid(NULL, &e, NULL);
    return e;
}

int setresuid(uid_t ruid, uid_t euid, uid_t suid)
{
    return (int)syscall3(SYS_setresuid, ruid, euid, suid);
}

int setresgid(gid_t rgid, gid_t egid, gid_t sgid)
{
    return (int)syscall3(SYS_setresgid, rgid, egid, sgid);
}

int setuid(uid_t uid)
{
    if (geteuid() == 0)
        return setresuid(uid, uid, uid);
    return setresuid((uid_t)-1, uid, (uid_t)-1);
}

int setgid(gid_t gid)
{
    if (geteuid() == 0)
        return setresgid(gid, gid, gid);
    return setresgid((gid_t)-1, gid, (gid_t)-1);
}

int seteuid(uid_t uid)
{
    return setresuid((uid_t)-1, uid, (uid_t)-1);
}

int setegid(gid_t gid)
{
    return setresgid((gid_t)-1, gid, (gid_t)-1);
}

/* POSIX setreuid: the saved id follows the new effective id when the real
 * id is set or the effective id differs from the old real id. */
int setreuid(uid_t ruid, uid_t euid)
{
    uid_t r, e, s;
    getresuid(&r, &e, &s);
    uid_t new_e = euid == (uid_t)-1 ? e : euid;
    uid_t saved = (ruid != (uid_t)-1 || (euid != (uid_t)-1 && euid != r)) ? new_e : (uid_t)-1;
    return setresuid(ruid, euid, saved);
}

int setregid(gid_t rgid, gid_t egid)
{
    gid_t r, e, s;
    getresgid(&r, &e, &s);
    gid_t new_e = egid == (gid_t)-1 ? e : egid;
    gid_t saved = (rgid != (gid_t)-1 || (egid != (gid_t)-1 && egid != r)) ? new_e : (gid_t)-1;
    return setresgid(rgid, egid, saved);
}

int getgroups(int size, gid_t list[])
{
    return (int)syscall2(SYS_getgroups, size, list);
}

int setgroups(size_t size, const gid_t *list)
{
    return (int)syscall2(SYS_setgroups, size, list);
}

mode_t umask(mode_t mask)
{
    return (mode_t)syscall1(SYS_umask, mask);
}

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
