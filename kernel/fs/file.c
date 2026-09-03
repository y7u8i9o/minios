#define KLOG_SUBSYS "file"
#include <fs/vfs.h>
#include <fs/fdtable.h>
#include <mm/slab.h>
#include <lib/string.h>
#include <kassert.h>
#include <klog.h>
#include <errno.h>

/* Reference counts of every open file description. */
static DEFINE_SPINLOCK(files_lock);

struct file *file_alloc(struct inode *ino, const struct file_ops *ops, int flags)
{
    struct file *f = kzalloc(sizeof *f);
    if (!f)
        return NULL;
    f->inode = ino;
    f->ops = ops;
    f->flags = flags;
    f->refcount = 1;
    mutex_init(&f->lock, "file");
    return f;
}

void file_ref(struct file *f)
{
    spin_lock(&files_lock);
    f->refcount++;
    spin_unlock(&files_lock);
}

void file_put(struct file *f)
{
    spin_lock(&files_lock);
    kassert(f->refcount > 0);
    int left = --f->refcount;
    spin_unlock(&files_lock);
    if (left)
        return;
    if (f->ops && f->ops->release)
        f->ops->release(f);
    if (f->inode)
        inode_put(f->inode);
    kfree(f);
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
    long r = f->ops->read(f, buf, n, &pos);
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
    long r = f->ops->write(f, buf, n, &pos);
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
    spinlock_init(&t->lock, "fdtable");
}

int fdtable_install(struct fdtable *t, struct file *f, int min)
{
    if (min < 0 || min >= OPEN_MAX)
        return -EINVAL;
    spin_lock(&t->lock);
    for (int i = min; i < OPEN_MAX; i++) {
        if (!t->fds[i]) {
            t->fds[i] = f;
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
    struct file *old = t->fds[fd];
    t->fds[fd] = f;
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
    spin_lock(&t->lock);
    struct file *f = t->fds[fd];
    if (f)
        file_ref(f);
    spin_unlock(&t->lock);
    return f;
}

int fdtable_close(struct fdtable *t, int fd)
{
    if (fd < 0 || fd >= OPEN_MAX)
        return -EBADF;
    spin_lock(&t->lock);
    struct file *f = t->fds[fd];
    t->fds[fd] = NULL;
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
    spin_unlock(&src->lock);
}

void fdtable_close_all(struct fdtable *t)
{
    for (int i = 0; i < OPEN_MAX; i++)
        fdtable_close(t, i);
}
