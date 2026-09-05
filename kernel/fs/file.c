#define KLOG_SUBSYS "file"
#include <fs/vfs.h>
#include <mm/filemap.h>
#include <sched/thread.h>
#include <sched/proc.h>
#include <ipc/signal.h>
#include <fs/fdtable.h>
#include <mm/slab.h>
#include <lib/string.h>
#include <kassert.h>
#include <klog.h>
#include <errno.h>

struct file *file_alloc(struct inode *ino, const struct file_ops *ops, int flags)
{
    struct file *f = kzalloc(sizeof *f);
    if (!f)
        return NULL;
    f->inode = ino;
    f->ops = ops;
    f->flags = flags;
    refcount_set(&f->refcount, 1);
    mutex_init(&f->lock, "file");
    return f;
}

void file_ref(struct file *f)
{
    kassert(refcount_read(&f->refcount) != 0);
    refcount_inc(&f->refcount);
}

static void file_free_rcu(struct rcu_head *head)
{
    struct file *f = container_of(head, struct file, rcu);
    kfree(f);
}

void file_put(struct file *f)
{
    kassert(refcount_read(&f->refcount) != 0);
    if (!refcount_dec_and_test(&f->refcount))
        return;
    if (f->ops && f->ops->release)
        f->ops->release(f);
    if (f->inode)
        inode_put(f->inode);
    rcu_call(&f->rcu, file_free_rcu);
}

long file_read(struct file *f, char *buf, size_t n)
{
    if ((f->flags & O_ACCMODE) == O_WRONLY)
        return -EBADF;
    if (f->inode && S_ISDIR(f->inode->mode))
        return -EISDIR;
    if (!f->ops || !f->ops->read)
        return -EINVAL;
    if (n == 0)
        return 0;
    mutex_lock(&f->lock);
    uint64_t pos = f->pos;
    uint64_t start = pos;
    long r = f->ops->read(f, buf, n, &pos);
    if (r > 0 && f->inode && f->inode->mapping)
        filemap_read_overlay(f->inode, buf, start, (size_t)r);
    f->pos = pos;
    mutex_unlock(&f->lock);
    return r;
}

long file_write(struct file *f, const char *buf, size_t n)
{
    if ((f->flags & O_ACCMODE) == O_RDONLY)
        return -EBADF;
    if (f->inode && S_ISDIR(f->inode->mode))
        return -EISDIR;
    if (!f->ops || !f->ops->write)
        return -EINVAL;
    if (n == 0)
        return 0;
    struct superblock *sb = f->inode ? f->inode->sb : NULL;
    vfs_op_begin(sb);
    mutex_lock(&f->lock);
    uint64_t pos = f->pos;
    if ((f->flags & O_APPEND) && f->inode)
        pos = f->inode->size;
    if (f->inode && S_ISREG(f->inode->mode)) {
        /* RLIMIT_FSIZE: a regular file may not grow past the limit. */
        struct proc *p = thread_current()->proc;
        uint64_t limit = proc_rlimit_cur(p, RLIMIT_FSIZE);
        if (limit != RLIM_INFINITY && pos + n > limit) {
            if (pos >= limit) {
                mutex_unlock(&f->lock);
                vfs_op_end(sb);
                signal_send(p, SIGXFSZ);
                return -EFBIG;
            }
            n = (size_t)(limit - pos);
        }
    }
    long r = f->ops->write(f, buf, n, &pos);
    if (r > 0 && f->inode && f->inode->mapping)
        filemap_write_through(f->inode, buf, pos - (uint64_t)r, (size_t)r);
    f->pos = pos;
    mutex_unlock(&f->lock);
    vfs_op_end(sb);
    return r;
}

long file_lseek(struct file *f, long off, int whence)
{
    mutex_lock(&f->lock);
    long r = f->ops && f->ops->lseek ? f->ops->lseek(f, off, whence)
                                      : vfs_generic_lseek(f, off, whence);
    mutex_unlock(&f->lock);
    return r;
}

long file_getdents(struct file *f, struct dirent *buf, size_t count)
{
    if (!f->inode || !S_ISDIR(f->inode->mode))
        return -ENOTDIR;
    if (!f->ops || !f->ops->getdents)
        return -EINVAL;
    mutex_lock(&f->lock);
    long r = f->ops->getdents(f, buf, count);
    mutex_unlock(&f->lock);
    return r;
}

/* ---- descriptor tables ---- */

void fdtable_init(struct fdtable *t)
{
    memset(t->fds, 0, sizeof t->fds);
    t->cloexec = 0;
    t->limit = OPEN_MAX;
    spinlock_init(&t->lock, "fdtable");
}

void fdtable_set_limit(struct fdtable *t, int limit)
{
    spin_lock(&t->lock);
    t->limit = limit < 0 ? 0 : limit > OPEN_MAX ? OPEN_MAX : limit;
    spin_unlock(&t->lock);
}

int fdtable_install(struct fdtable *t, struct file *f, int min)
{
    if (min < 0 || min >= OPEN_MAX)
        return -EINVAL;
    spin_lock(&t->lock);
    for (int i = min; i < t->limit; i++) {
        if (!t->fds[i]) {
            __atomic_store_n(&t->fds[i], f, __ATOMIC_RELEASE);
            t->cloexec &= ~(1ULL << i);
            spin_unlock(&t->lock);
            return i;
        }
    }
    spin_unlock(&t->lock);
    return -EMFILE;
}

int fdtable_install_at(struct fdtable *t, struct file *f, int fd)
{
    if (fd < 0 || fd >= OPEN_MAX)
        return -EBADF;
    spin_lock(&t->lock);
    if (fd >= t->limit) {
        spin_unlock(&t->lock);
        return -EBADF;
    }
    struct file *old = __atomic_load_n(&t->fds[fd], __ATOMIC_RELAXED);
    __atomic_store_n(&t->fds[fd], f, __ATOMIC_RELEASE);
    t->cloexec &= ~(1ULL << fd);
    spin_unlock(&t->lock);
    if (old)
        file_put(old);
    return fd;
}

struct file *fdtable_get(struct fdtable *t, int fd)
{
    if (fd < 0 || fd >= OPEN_MAX)
        return NULL;
    struct file *f;
    rcu_read_lock();
    for (;;) {
        f = __atomic_load_n(&t->fds[fd], __ATOMIC_ACQUIRE);
        if (!f || refcount_inc_not_zero(&f->refcount))
            break;
    }
    rcu_read_unlock();
    return f;
}

int fdtable_close(struct fdtable *t, int fd)
{
    if (fd < 0 || fd >= OPEN_MAX)
        return -EBADF;
    spin_lock(&t->lock);
    struct file *f = __atomic_load_n(&t->fds[fd], __ATOMIC_RELAXED);
    __atomic_store_n(&t->fds[fd], NULL, __ATOMIC_RELEASE);
    t->cloexec &= ~(1ULL << fd);
    spin_unlock(&t->lock);
    if (!f)
        return -EBADF;
    file_put(f);
    return 0;
}

void fdtable_set_cloexec(struct fdtable *t, int fd, bool on)
{
    if (fd < 0 || fd >= OPEN_MAX)
        return;
    spin_lock(&t->lock);
    if (on)
        t->cloexec |= 1ULL << fd;
    else
        t->cloexec &= ~(1ULL << fd);
    spin_unlock(&t->lock);
}

bool fdtable_get_cloexec(struct fdtable *t, int fd)
{
    if (fd < 0 || fd >= OPEN_MAX)
        return false;
    spin_lock(&t->lock);
    bool on = (t->cloexec >> fd) & 1;
    spin_unlock(&t->lock);
    return on;
}

void fdtable_close_exec(struct fdtable *t)
{
    for (int i = 0; i < OPEN_MAX; i++)
        if (fdtable_get_cloexec(t, i))
            fdtable_close(t, i);
}

void fdtable_copy(struct fdtable *dst, struct fdtable *src)
{
    spin_lock(&src->lock);
    for (int i = 0; i < OPEN_MAX; i++) {
        dst->fds[i] = src->fds[i];
        if (dst->fds[i])
            file_ref(dst->fds[i]);
    }
    dst->cloexec = src->cloexec;
    dst->limit = src->limit;
    spin_unlock(&src->lock);
}

void fdtable_close_all(struct fdtable *t)
{
    for (int i = 0; i < OPEN_MAX; i++)
        fdtable_close(t, i);
}
