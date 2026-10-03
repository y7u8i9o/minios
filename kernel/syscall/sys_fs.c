#include <syscall/syscalls.h>
#include <sched/thread.h>
#include <sched/proc.h>
#include <fs/vfs.h>
#include <fs/fdtable.h>
#include <ipc/pipe.h>
#include <lib/string.h>
#include <lib/printf.h>
#include <errno.h>

static struct fdtable *cur_fds(void)
{
    return &thread_current()->proc->fds;
}

long sys_write(struct trapframe *tf)
{
    int fd = (int)SYSARG0(tf);
    uintptr_t buf = SYSARG1(tf);
    size_t len = SYSARG2(tf);
    if (!user_range_ok(buf, len, false))
        return -EFAULT;
    struct file *f = fdtable_get(cur_fds(), fd);
    if (!f)
        return -EBADF;
    long r = file_write(f, (const char *)buf, len);
    file_put(f);
    return r;
}

long sys_read(struct trapframe *tf)
{
    int fd = (int)SYSARG0(tf);
    uintptr_t buf = SYSARG1(tf);
    size_t len = SYSARG2(tf);
    if (!user_range_ok(buf, len, true))
        return -EFAULT;
    struct file *f = fdtable_get(cur_fds(), fd);
    if (!f)
        return -EBADF;
    long r = file_read(f, (char *)buf, len);
    file_put(f);
    return r;
}

long sys_open(struct trapframe *tf)
{
    char path[USER_PATH_MAX];
    long r = copy_string_from_user(path, SYSARG0(tf), sizeof path);
    if (r < 0)
        return r;
    int flags = (int)SYSARG1(tf);
    uint32_t mode = (uint32_t)SYSARG2(tf);
    struct file *f;
    r = vfs_open(path, flags, mode, &f);
    if (r < 0)
        return r;
    r = fdtable_install(cur_fds(), f, 0);
    if (r < 0)
        file_put(f);
    else if (flags & O_CLOEXEC)
        fdtable_set_cloexec(cur_fds(), (int)r, true);
    return r;
}

/* fcntl(fd, cmd, arg): descriptor flags, file status flags, dup. */
long sys_fcntl(struct trapframe *tf)
{
    int fd = (int)SYSARG0(tf), cmd = (int)SYSARG1(tf);
    long arg = (long)SYSARG2(tf);
    struct file *f = fdtable_get(cur_fds(), fd);
    if (!f)
        return -EBADF;
    long r = 0;
    switch (cmd) {
    case F_GETFD:
        r = fdtable_get_cloexec(cur_fds(), fd) ? FD_CLOEXEC : 0;
        break;
    case F_SETFD:
        fdtable_set_cloexec(cur_fds(), fd, (arg & FD_CLOEXEC) != 0);
        break;
    case F_GETFL:
        r = f->flags;
        break;
    case F_SETFL:
        f->flags = (f->flags & ~(O_NONBLOCK | O_APPEND)) | (int)(arg & (O_NONBLOCK | O_APPEND));
        break;
    case F_DUPFD:
    case F_DUPFD_CLOEXEC:
        file_ref(f);
        r = fdtable_install(cur_fds(), f, (int)arg);
        if (r < 0)
            file_put(f);
        else if (cmd == F_DUPFD_CLOEXEC)
            fdtable_set_cloexec(cur_fds(), (int)r, true);
        break;
    default:
        r = -EINVAL;
    }
    file_put(f);
    return r;
}

long sys_close(struct trapframe *tf)
{
    return fdtable_close(cur_fds(), (int)SYSARG0(tf));
}

long sys_lseek(struct trapframe *tf)
{
    struct file *f = fdtable_get(cur_fds(), (int)SYSARG0(tf));
    if (!f)
        return -EBADF;
    long r = file_lseek(f, (long)SYSARG1(tf), (int)SYSARG2(tf));
    file_put(f);
    return r;
}

long sys_dup(struct trapframe *tf)
{
    struct file *f = fdtable_get(cur_fds(), (int)SYSARG0(tf));
    if (!f)
        return -EBADF;
    long r = fdtable_install(cur_fds(), f, 0);
    if (r < 0)
        file_put(f);
    return r;
}

long sys_dup2(struct trapframe *tf)
{
    int oldfd = (int)SYSARG0(tf), newfd = (int)SYSARG1(tf);
    if (newfd < 0 || newfd >= OPEN_MAX)
        return -EBADF;
    struct file *f = fdtable_get(cur_fds(), oldfd);
    if (!f)
        return -EBADF;
    if (oldfd == newfd) {
        file_put(f);
        return newfd;
    }
    long r = fdtable_install_at(cur_fds(), f, newfd);
    if (r < 0)
        file_put(f);
    return r;
}

/* Fill the checked user struct stat at st for path; flags is 0 or
 * VFS_NOFOLLOW. */
static long stat_path(const char *path, unsigned flags, uintptr_t st)
{
    struct inode *ino;
    int r = vfs_lookup_path(path, flags, &ino, NULL, 0);
    if (r < 0)
        return r;
    inode_stat(ino, (struct stat *)st);
    inode_put(ino);
    return 0;
}

static long stat_common(struct trapframe *tf, unsigned flags)
{
    char path[USER_PATH_MAX];
    long r = copy_string_from_user(path, SYSARG0(tf), sizeof path);
    if (r < 0)
        return r;
    uintptr_t st = SYSARG1(tf);
    if (!user_range_ok(st, sizeof(struct stat), true))
        return -EFAULT;
    return stat_path(path, flags, st);
}

long sys_stat(struct trapframe *tf)
{
    return stat_common(tf, 0);
}

/* lstat(path, st): stat that reports a symbolic link itself. */
long sys_lstat(struct trapframe *tf)
{
    return stat_common(tf, VFS_NOFOLLOW);
}

long sys_fstat(struct trapframe *tf)
{
    uintptr_t st = SYSARG1(tf);
    if (!user_range_ok(st, sizeof(struct stat), true))
        return -EFAULT;
    struct file *f = fdtable_get(cur_fds(), (int)SYSARG0(tf));
    if (!f)
        return -EBADF;
    if (f->inode) {
        inode_stat(f->inode, (struct stat *)st);
    } else {
        /* Pipes, sockets and the other objects without an inode keep no
         * owner. They are reported as the caller's own. */
        struct cred c;
        cred_get_current(&c);
        memset((void *)st, 0, sizeof(struct stat));
        ((struct stat *)st)->st_mode = S_IFIFO | 0600;
        ((struct stat *)st)->st_uid = c.euid;
        ((struct stat *)st)->st_gid = c.egid;
    }
    file_put(f);
    return 0;
}

long sys_getdents(struct trapframe *tf)
{
    uintptr_t buf = SYSARG1(tf);
    size_t count = SYSARG2(tf);
    if (!user_range_ok(buf, count, true))
        return -EFAULT;
    if (count < sizeof(struct dirent))
        return -EINVAL;
    struct file *f = fdtable_get(cur_fds(), (int)SYSARG0(tf));
    if (!f)
        return -EBADF;
    long r = file_getdents(f, (struct dirent *)buf, count);
    file_put(f);
    return r;
}

/* Helpers for the single path operations. */
static long path_op(struct trapframe *tf, int (*fn)(const char *))
{
    char path[USER_PATH_MAX];
    long r = copy_string_from_user(path, SYSARG0(tf), sizeof path);
    if (r < 0)
        return r;
    return fn(path);
}

static long path2_op(struct trapframe *tf, int (*fn)(const char *, const char *))
{
    char a[USER_PATH_MAX], b[USER_PATH_MAX];
    long r = copy_string_from_user(a, SYSARG0(tf), sizeof a);
    if (r < 0)
        return r;
    r = copy_string_from_user(b, SYSARG1(tf), sizeof b);
    if (r < 0)
        return r;
    return fn(a, b);
}

/* mkdir(path, mode): mode is masked by the umask in vfs_mkdir. */
long sys_mkdir(struct trapframe *tf)
{
    char path[USER_PATH_MAX];
    long r = copy_string_from_user(path, SYSARG0(tf), sizeof path);
    if (r < 0)
        return r;
    return vfs_mkdir(path, (uint32_t)SYSARG1(tf));
}

long sys_unlink(struct trapframe *tf)
{
    return path_op(tf, vfs_unlink);
}

long sys_rmdir(struct trapframe *tf)
{
    return path_op(tf, vfs_rmdir);
}

long sys_rename(struct trapframe *tf)
{
    return path2_op(tf, vfs_rename);
}

/* Copy the user path of an *at system call into buf, prefixed with the path
 * of the directory dirfd when the path is relative and dirfd is not
 * AT_FDCWD. The directory must have been opened by name (file.path). */
static long copy_path_at(int dirfd, uintptr_t upath, char *buf, size_t size)
{
    char path[USER_PATH_MAX];
    long r = copy_string_from_user(path, upath, sizeof path);
    if (r < 0)
        return r;
    if (dirfd == AT_FDCWD || path[0] == '/') {
        strlcpy(buf, path, size);
        return 0;
    }
    struct file *f = fdtable_get(cur_fds(), dirfd);
    if (!f)
        return -EBADF;
    if (!f->inode || !S_ISDIR(f->inode->mode) || !f->path) {
        file_put(f);
        return -ENOTDIR;
    }
    int n = ksnprintf(buf, size, "%s/%s", f->path, path);
    file_put(f);
    return n < 0 || (size_t)n >= size ? -ENAMETOOLONG : 0;
}

/* openat(dirfd, path, flags, mode): open relative to a directory descriptor. */
long sys_openat(struct trapframe *tf)
{
    char path[USER_PATH_MAX];
    long r = copy_path_at((int)SYSARG0(tf), SYSARG1(tf), path, sizeof path);
    if (r < 0)
        return r;
    int flags = (int)SYSARG2(tf);
    uint32_t mode = (uint32_t)SYSARG3(tf);
    struct file *f;
    r = vfs_open(path, flags, mode, &f);
    if (r < 0)
        return r;
    r = fdtable_install(cur_fds(), f, 0);
    if (r < 0)
        file_put(f);
    else if (flags & O_CLOEXEC)
        fdtable_set_cloexec(cur_fds(), (int)r, true);
    return r;
}

/* fstatat(dirfd, path, st, flags): stat relative to a directory descriptor.
 * With AT_SYMLINK_NOFOLLOW a symbolic link in the last component is
 * reported itself. */
long sys_fstatat(struct trapframe *tf)
{
    char path[USER_PATH_MAX];
    long r = copy_path_at((int)SYSARG0(tf), SYSARG1(tf), path, sizeof path);
    if (r < 0)
        return r;
    uintptr_t st = SYSARG2(tf);
    if (!user_range_ok(st, sizeof(struct stat), true))
        return -EFAULT;
    if (SYSARG3(tf) & ~(uintptr_t)AT_SYMLINK_NOFOLLOW)
        return -EINVAL;
    return stat_path(path, SYSARG3(tf) & AT_SYMLINK_NOFOLLOW ? VFS_NOFOLLOW : 0, st);
}

/* symlinkat(target, dirfd, path): create path, resolved as in openat, as a
 * symbolic link holding target. */
long sys_symlinkat(struct trapframe *tf)
{
    char target[USER_PATH_MAX], path[USER_PATH_MAX];
    long r = copy_string_from_user(target, SYSARG0(tf), sizeof target);
    if (r < 0)
        return r;
    r = copy_path_at((int)SYSARG1(tf), SYSARG2(tf), path, sizeof path);
    if (r < 0)
        return r;
    return vfs_symlink(target, path);
}

long sys_symlink(struct trapframe *tf)
{
    return path2_op(tf, vfs_symlink);
}

/* Copy the target of the link at path to the user buffer buf of size
 * bytes, without a NUL, and return the number of bytes copied. */
static long readlink_common(const char *path, uintptr_t buf, size_t size)
{
    if ((long)size <= 0)
        return -EINVAL;
    if (!user_range_ok(buf, size, true))
        return -EFAULT;
    char target[VFS_SYMLINK_MAX + 1];
    int r = vfs_readlink(path, target, sizeof target);
    if (r < 0)
        return r;
    size_t n = MIN((size_t)r, size);
    /* User memory is written with no lock held: the copy may fault. */
    memcpy((char *)buf, target, n);
    return (long)n;
}

long sys_readlink(struct trapframe *tf)
{
    char path[USER_PATH_MAX];
    long r = copy_string_from_user(path, SYSARG0(tf), sizeof path);
    if (r < 0)
        return r;
    return readlink_common(path, SYSARG1(tf), SYSARG2(tf));
}

/* readlinkat(dirfd, path, buf, size): readlink resolved as in openat. */
long sys_readlinkat(struct trapframe *tf)
{
    char path[USER_PATH_MAX];
    long r = copy_path_at((int)SYSARG0(tf), SYSARG1(tf), path, sizeof path);
    if (r < 0)
        return r;
    return readlink_common(path, SYSARG2(tf), SYSARG3(tf));
}

/* utimensat(dirfd, path, times, flags): sets the modification time of the
 * file at path, resolved relative to dirfd as in openat, or of a symbolic
 * link itself with AT_SYMLINK_NOFOLLOW; times is NULL for
 * the current time or two timespecs of which
 * the second is the modification time, with UTIME_NOW and UTIME_OMIT in
 * tv_nsec. The access time is not stored and is ignored. Inode times are
 * nanoseconds, so the given time is kept exactly by mfs. */
long sys_utimensat(struct trapframe *tf)
{
    if (SYSARG3(tf) & ~(uintptr_t)AT_SYMLINK_NOFOLLOW)
        return -EINVAL;
    char path[USER_PATH_MAX];
    long r = copy_path_at((int)SYSARG0(tf), SYSARG1(tf), path, sizeof path);
    if (r < 0)
        return r;
    int64_t mtime = vfs_now();
    unsigned now = VFS_UTIME_NOW;
    uintptr_t times = SYSARG2(tf);
    if (times) {
        struct timespec ts[2];
        if (!user_range_ok(times, sizeof ts, false))
            return -EFAULT;
        memcpy(ts, (const void *)times, sizeof ts);
        if (ts[1].tv_nsec == UTIME_OMIT)
            return 0;
        if (ts[1].tv_nsec != UTIME_NOW) {
            if (ts[1].tv_nsec < 0 || ts[1].tv_nsec >= 1000000000)
                return -EINVAL;
            mtime = ts[1].tv_sec * 1000000000 + ts[1].tv_nsec;
            now = 0;
        }
    }
    return vfs_utimens(path, mtime, (SYSARG3(tf) & AT_SYMLINK_NOFOLLOW ? VFS_NOFOLLOW : 0) | now);
}

long sys_link(struct trapframe *tf)
{
    return path2_op(tf, vfs_link);
}

static long pipe_common(uintptr_t fds, int flags);

long sys_pipe(struct trapframe *tf)
{
    return pipe_common(SYSARG0(tf), 0);
}

/* pipe2(fds, flags): O_NONBLOCK and O_CLOEXEC. */
long sys_pipe2(struct trapframe *tf)
{
    return pipe_common(SYSARG0(tf), (int)SYSARG1(tf) & (O_NONBLOCK | O_CLOEXEC));
}

static long pipe_common(uintptr_t fds, int flags)
{
    if (!user_range_ok(fds, 2 * sizeof(int), true))
        return -EFAULT;
    struct file *rd, *wr;
    int r = pipe_create(&rd, &wr);
    if (r < 0)
        return r;
    int rfd = fdtable_install(cur_fds(), rd, 0);
    if (rfd < 0) {
        file_put(rd);
        file_put(wr);
        return rfd;
    }
    int wfd = fdtable_install(cur_fds(), wr, 0);
    if (wfd < 0) {
        fdtable_close(cur_fds(), rfd);
        file_put(wr);
        return wfd;
    }
    rd->flags |= flags & O_NONBLOCK;
    wr->flags |= flags & O_NONBLOCK;
    if (flags & O_CLOEXEC) {
        fdtable_set_cloexec(cur_fds(), rfd, true);
        fdtable_set_cloexec(cur_fds(), wfd, true);
    }
    ((int *)fds)[0] = rfd;
    ((int *)fds)[1] = wfd;
    return 0;
}

long sys_mount(struct trapframe *tf)
{
    if (!cred_current_is_root())
        return -EPERM;
    char source[USER_PATH_MAX], target[USER_PATH_MAX], type[32];
    long r = copy_string_from_user(source, SYSARG0(tf), sizeof source);
    if (r < 0)
        return r;
    r = copy_string_from_user(target, SYSARG1(tf), sizeof target);
    if (r < 0)
        return r;
    r = copy_string_from_user(type, SYSARG2(tf), sizeof type);
    if (r < 0)
        return r;
    /* The fourth argument, when not NULL, is a comma separated option
     * string for the filesystem (U1). */
    char options[128] = "";
    if (SYSARG3(tf)) {
        r = copy_string_from_user(options, SYSARG3(tf), sizeof options);
        if (r < 0)
            return r;
    }
    return vfs_mount(type, source, target, options);
}

/* faccessat(dirfd, path, mode, flags): F_OK (0) tests existence, R_OK,
 * W_OK and X_OK (4, 2, 1) permission, with the real ids unless flags has
 * AT_EACCESS (U2). */
long sys_faccessat(struct trapframe *tf)
{
    uintptr_t flags = SYSARG3(tf);
    if (flags & ~(uintptr_t)(AT_SYMLINK_NOFOLLOW | AT_EACCESS))
        return -EINVAL;
    unsigned mask = (unsigned)SYSARG2(tf);
    if (mask & ~7u)
        return -EINVAL;
    char path[USER_PATH_MAX];
    long r = copy_path_at((int)SYSARG0(tf), SYSARG1(tf), path, sizeof path);
    if (r < 0)
        return r;
    return vfs_access(path, (int)mask, (flags & AT_SYMLINK_NOFOLLOW ? VFS_NOFOLLOW : 0) |
                                       (flags & AT_EACCESS ? VFS_EACCESS : 0));
}

/* fchmodat(dirfd, path, mode, flags) and fchownat(dirfd, path, uid, gid,
 * flags), with AT_SYMLINK_NOFOLLOW to change a symbolic link itself. */
long sys_fchmodat(struct trapframe *tf)
{
    if (SYSARG3(tf) & ~(uintptr_t)AT_SYMLINK_NOFOLLOW)
        return -EINVAL;
    char path[USER_PATH_MAX];
    long r = copy_path_at((int)SYSARG0(tf), SYSARG1(tf), path, sizeof path);
    if (r < 0)
        return r;
    return vfs_chmod(path, (uint32_t)SYSARG2(tf), SYSARG3(tf) & AT_SYMLINK_NOFOLLOW ? VFS_NOFOLLOW : 0);
}

long sys_fchownat(struct trapframe *tf)
{
    if (SYSARG4(tf) & ~(uintptr_t)AT_SYMLINK_NOFOLLOW)
        return -EINVAL;
    char path[USER_PATH_MAX];
    long r = copy_path_at((int)SYSARG0(tf), SYSARG1(tf), path, sizeof path);
    if (r < 0)
        return r;
    return vfs_chown(path, (uint32_t)SYSARG2(tf), (uint32_t)SYSARG3(tf),
                     SYSARG4(tf) & AT_SYMLINK_NOFOLLOW ? VFS_NOFOLLOW : 0);
}

/* fchmod(fd, mode) and fchown(fd, uid, gid) act on the inode of an open
 * file. Objects without an inode cannot be changed. */
long sys_fchmod(struct trapframe *tf)
{
    struct file *f = fdtable_get(cur_fds(), (int)SYSARG0(tf));
    if (!f)
        return -EBADF;
    long r = f->inode ? vfs_chmod_inode(f->inode, (uint32_t)SYSARG1(tf)) : -EINVAL;
    file_put(f);
    return r;
}

long sys_fchown(struct trapframe *tf)
{
    struct file *f = fdtable_get(cur_fds(), (int)SYSARG0(tf));
    if (!f)
        return -EBADF;
    long r = f->inode ? vfs_chown_inode(f->inode, (uint32_t)SYSARG1(tf), (uint32_t)SYSARG2(tf)) : -EINVAL;
    file_put(f);
    return r;
}

long sys_umount(struct trapframe *tf)
{
    if (!cred_current_is_root())
        return -EPERM;
    return path_op(tf, vfs_umount);
}

long sys_sync(struct trapframe *tf)
{
    return vfs_sync();
}

long sys_chdir(struct trapframe *tf)
{
    char path[USER_PATH_MAX];
    long r = copy_string_from_user(path, SYSARG0(tf), sizeof path);
    if (r < 0)
        return r;
    /* The working directory is kept as the path of the directory without
     * symbolic links, which getcwd returns. */
    struct proc *p = thread_current()->proc;
    char resolved[PROC_CWD_LEN];
    struct inode *ino;
    r = vfs_lookup_path(path, 0, &ino, resolved, sizeof resolved);
    if (r < 0)
        return r;
    bool is_dir = S_ISDIR(ino->mode);
    struct cred c;
    cred_get_current(&c);
    r = is_dir ? vfs_permission(ino, MAY_EXEC, &c) : -ENOTDIR;
    inode_put(ino);
    if (r < 0)
        return r;
    spin_lock(&p->lock);
    strlcpy(p->cwd, resolved, sizeof p->cwd);
    spin_unlock(&p->lock);
    return 0;
}

long sys_getcwd(struct trapframe *tf)
{
    uintptr_t buf = SYSARG0(tf);
    size_t size = SYSARG1(tf);
    struct proc *p = thread_current()->proc;
    if (!user_range_ok(buf, size, true))
        return -EFAULT;
    char cwd[PROC_CWD_LEN];
    spin_lock(&p->lock);
    strlcpy(cwd, p->cwd, sizeof cwd);
    spin_unlock(&p->lock);
    size_t n = strlen(cwd);
    if (n + 1 > size)
        return -ERANGE;
    /* User memory is touched without a spinlock held: the copy may fault
     * on a swapped page. */
    memcpy((char *)buf, cwd, n + 1);
    return (long)n;
}

long sys_ioctl(struct trapframe *tf)
{
    struct file *f = fdtable_get(cur_fds(), (int)SYSARG0(tf));
    if (!f)
        return -EBADF;
    long r = -ENOTTY;
    if (f->ops && f->ops->ioctl)
        r = f->ops->ioctl(f, SYSARG1(tf), SYSARG2(tf));
    file_put(f);
    return r;
}
