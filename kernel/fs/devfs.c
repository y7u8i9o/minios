#define KLOG_SUBSYS "devfs"
#include <fs/devfs.h>
#include <fs/vfs.h>
#include <drivers/tty.h>
#include <sched/proc.h>
#include <sched/thread.h>
#include <mm/vma.h>
#include <drivers/fbcon.h>
#include <console.h>
#include <mm/slab.h>
#include <lib/string.h>
#include <klog.h>
#include <errno.h>

/* Device nodes. The list is protected by devfs_lock. Nodes are never
 * removed, so an inode may keep a pointer to its node without a lock.
 * A node registered with S_IFDIR is a directory; parent is the inode of
 * the directory holding the node (ROOT_INO for /dev itself). */
struct devnode {
    char name[32];
    uint64_t ino;
    uint64_t parent;
    uint32_t mode;
    const struct file_ops *fops;
    void *priv;
    uint64_t size;
    struct list_head link;
};

#define ROOT_INO 1
static LIST_HEAD(devnodes);
static DEFINE_SPINLOCK(devfs_lock);
static uint64_t next_ino = 2;
static struct superblock *devfs_sb;

static const struct inode_ops devfs_dir_ops;
static const struct file_ops devfs_dir_fops;

static struct devnode *devnode_find(uint64_t parent, const char *name, size_t len)
{
    struct list_head *pos;
    struct devnode *found = NULL;
    spin_lock(&devfs_lock);
    list_for_each(pos, &devnodes) {
        struct devnode *n = list_entry(pos, struct devnode, link);
        if (n->parent == parent && strlen(n->name) == len && memcmp(n->name, name, len) == 0) {
            found = n;
            break;
        }
    }
    spin_unlock(&devfs_lock);
    return found;
}

static struct devnode *devnode_by_ino(uint64_t ino)
{
    struct list_head *pos;
    struct devnode *found = NULL;
    spin_lock(&devfs_lock);
    list_for_each(pos, &devnodes) {
        struct devnode *n = list_entry(pos, struct devnode, link);
        if (n->ino == ino) {
            found = n;
            break;
        }
    }
    spin_unlock(&devfs_lock);
    return found;
}

int devfs_register(const char *name, uint32_t mode, const struct file_ops *fops, void *priv,
                   uint64_t size)
{
    /* "dir/name" registers under a directory node registered before. */
    uint64_t parent = ROOT_INO;
    const char *slash = strchr(name, '/');
    if (slash) {
        struct devnode *dir = devnode_find(ROOT_INO, name, (size_t)(slash - name));
        if (!dir || !S_ISDIR(dir->mode))
            return -ENOENT;
        parent = dir->ino;
        name = slash + 1;
    }
    if (devnode_find(parent, name, strlen(name)))
        return -EEXIST;
    struct devnode *n = kzalloc(sizeof *n);
    if (!n)
        return -ENOMEM;
    strlcpy(n->name, name, sizeof n->name);
    n->parent = parent;
    n->mode = mode;
    n->fops = fops;
    n->priv = priv;
    n->size = size;
    spin_lock(&devfs_lock);
    n->ino = next_ino++;
    list_add_tail(&n->link, &devnodes);
    spin_unlock(&devfs_lock);
    if (S_ISDIR(mode))
        klog_info("/dev/%s/ registered", name);
    else if (parent == ROOT_INO)
        klog_info("/dev/%s registered", name);
    return 0;
}

static int devfs_lookup(struct inode *dir, const char *name, size_t len, struct inode **out)
{
    struct devnode *n = devnode_find(dir->ino, name, len);
    if (!n)
        return -ENOENT;
    *out = inode_get(dir->sb, n->ino);
    return *out ? 0 : -ENOMEM;
}

static long devfs_getdents(struct file *f, struct dirent *buf, size_t count)
{
    size_t max = count / sizeof(struct dirent);
    size_t filled = 0, index = 0;
    struct list_head *pos;
    uint64_t parent = f->inode->ino;
    spin_lock(&devfs_lock);
    list_for_each(pos, &devnodes) {
        struct devnode *n = list_entry(pos, struct devnode, link);
        if (n->parent != parent)
            continue;
        if (filled == max)
            break;
        if (index++ < f->pos)
            continue;
        buf[filled].d_ino = n->ino;
        buf[filled].d_type = vfs_mode_to_dtype(n->mode);
        strlcpy(buf[filled].d_name, n->name, sizeof buf[filled].d_name);
        filled++;
        f->pos++;
    }
    spin_unlock(&devfs_lock);
    return (long)(filled * sizeof(struct dirent));
}

static const struct inode_ops devfs_dir_ops = {
    .lookup = devfs_lookup,
};
static const struct file_ops devfs_dir_fops = {
    .getdents = devfs_getdents,
};

static int devfs_read_inode(struct superblock *sb, uint64_t ino, struct inode *i)
{
    if (ino == ROOT_INO) {
        i->mode = S_IFDIR | 0755;
        i->nlink = 2;
        i->ops = &devfs_dir_ops;
        i->fops = &devfs_dir_fops;
        return 0;
    }
    struct devnode *n = devnode_by_ino(ino);
    if (!n)
        return -ENOENT;
    i->mode = n->mode;
    if (S_ISDIR(n->mode)) {
        i->nlink = 2;
        i->ops = &devfs_dir_ops;
        i->fops = &devfs_dir_fops;
        return 0;
    }
    i->nlink = 1;
    i->fops = n->fops;
    i->priv = n->priv;
    i->size = n->size;
    i->rdev = n->ino;
    return 0;
}

static const struct sb_ops devfs_sb_ops = {
    .read_inode = devfs_read_inode,
};

static int devfs_mount(const struct fs_type *type, const char *source, struct superblock **out)
{
    if (devfs_sb)
        return -EBUSY;
    struct superblock *sb = sb_alloc(type, &devfs_sb_ops);
    if (!sb)
        return -ENOMEM;
    sb->root_ino = ROOT_INO;
    devfs_sb = sb;
    *out = sb;
    return 0;
}

static struct fs_type devfs_type = {
    .name = "devfs",
    .mount = devfs_mount,
};

/* ---- built in devices ---- */

static long condev_read(struct file *f, char *buf, size_t n, uint64_t *pos)
{
    return tty_read(&console_tty, buf, n);
}

static long condev_write(struct file *f, const char *buf, size_t n, uint64_t *pos)
{
    console_write(buf, n);
    return (long)n;
}

static long null_read(struct file *f, char *buf, size_t n, uint64_t *pos)
{
    return 0;
}

static long null_write(struct file *f, const char *buf, size_t n, uint64_t *pos)
{
    return (long)n;
}

static long zero_read(struct file *f, char *buf, size_t n, uint64_t *pos)
{
    memset(buf, 0, n);
    return (long)n;
}

static long procdev_read(struct file *f, char *buf, size_t n, uint64_t *pos)
{
    char *text = kmalloc(4096);
    if (!text)
        return -ENOMEM;
    size_t len = proc_format_table(text, 4096);
    long r = 0;
    if (*pos < len) {
        size_t avail = len - *pos;
        if (n > avail)
            n = avail;
        memcpy(buf, text + *pos, n);
        *pos += n;
        r = (long)n;
    }
    kfree(text);
    return r;
}

static long condev_ioctl(struct file *f, unsigned long req, uintptr_t arg)
{
    return tty_ioctl(&console_tty, req, arg);
}

static const struct file_ops procdev_fops = { .read = procdev_read };

/* /dev/klog: the kernel log ring. The file position is the absolute
 * offset in the log; a read returns what was appended since. */
static long klogdev_read(struct file *f, char *buf, size_t n, uint64_t *pos)
{
    char tmp[512];
    size_t got = klog_ring_read(pos, tmp, n < sizeof tmp ? n : sizeof tmp);
    memcpy(buf, tmp, got);
    return (long)got;
}

static int klogdev_poll(struct file *f)
{
    return (klog_ring_head() > f->pos ? POLLIN : 0);
}

static struct poll_source *klogdev_source(struct file *f)
{
    return klog_poll_source();
}

static long klogdev_lseek(struct file *f, long off, int whence)
{
    if (whence == SEEK_END) {
        f->pos = klog_ring_head() + (uint64_t)off;
        return (long)f->pos;
    }
    if (whence == SEEK_SET) {
        f->pos = (uint64_t)off;
        return (long)f->pos;
    }
    return -EINVAL;
}

static const struct file_ops klogdev_fops = {
    .read = klogdev_read, .poll = klogdev_poll, .poll_source = klogdev_source,
    .lseek = klogdev_lseek
};
static int condev_poll(struct file *f)
{
    return tty_poll(&console_tty);
}

static struct poll_source *condev_source(struct file *f)
{
    return tty_poll_source(&console_tty);
}

static const struct file_ops condev_fops = { .read = condev_read, .write = condev_write,
                                             .ioctl = condev_ioctl, .poll = condev_poll,
                                             .poll_source = condev_source };

static const struct file_ops null_fops = { .read = null_read, .write = null_write };
static const struct file_ops zero_fops = { .read = zero_read, .write = null_write };

void devfs_init(void)
{
    vfs_register_fs(&devfs_type);
    devfs_register("console", S_IFCHR | 0666, &condev_fops, NULL, 0);
    devfs_register("klog", S_IFCHR | 0444, &klogdev_fops, NULL, 0);
    devfs_register("null", S_IFCHR | 0666, &null_fops, NULL, 0);
    devfs_register("zero", S_IFCHR | 0666, &zero_fops, NULL, 0);
    devfs_register("proc", S_IFCHR | 0444, &procdev_fops, NULL, 0);
}
