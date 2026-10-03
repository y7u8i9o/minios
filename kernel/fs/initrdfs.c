#define KLOG_SUBSYS "initrdfs"
#include <fs/initrdfs.h>
#include <fs/initrd.h>
#include <fs/vfs.h>
#include <lib/string.h>
#include <klog.h>
#include <errno.h>

/* Inode numbers: 1 is the root, entry i of the initrd table is i + 2. The
 * archive is immutable, so inodes carry a pointer to the entry and every
 * modifying operation is absent (the VFS reports EROFS). */
#define ROOT_INO 1

static const struct inode_ops initrd_dir_ops;
static const struct file_ops initrd_file_fops;
static const struct file_ops initrd_dir_fops;

static const char *inode_path(struct inode *ino)
{
    const struct initrd_entry *e = ino->priv;
    return e ? e->name : "";
}

/* Name of the child of dir path in entry name, or NULL if not a child. */
static const char *child_name(const char *dirpath, const char *name)
{
    size_t dl = strlen(dirpath);
    if (dl) {
        if (strncmp(name, dirpath, dl) != 0 || name[dl] != '/')
            return NULL;
        name += dl + 1;
    }
    if (!*name || strchr(name, '/'))
        return NULL;
    return name;
}

static int initrd_lookup_op(struct inode *dir, const char *name, size_t len, struct inode **out)
{
    const char *dirpath = inode_path(dir);
    for (size_t i = 0; i < initrd_count(); i++) {
        const char *c = child_name(dirpath, initrd_entry(i)->name);
        if (c && strlen(c) == len && memcmp(c, name, len) == 0) {
            *out = inode_get(dir->sb, i + 2);
            return *out ? 0 : -ENOMEM;
        }
    }
    return -ENOENT;
}

static long initrd_read(struct file *f, char *buf, size_t n, uint64_t *pos)
{
    const struct initrd_entry *e = f->inode->priv;
    if (*pos >= e->size)
        return 0;
    size_t left = e->size - *pos;
    if (n > left)
        n = left;
    memcpy(buf, e->data + *pos, n);
    *pos += n;
    return (long)n;
}

static long initrd_getdents(struct file *f, struct dirent *buf, size_t count)
{
    const char *dirpath = inode_path(f->inode);
    size_t max = count / sizeof(struct dirent);
    size_t filled = 0, index = 0;
    for (size_t i = 0; i < initrd_count() && filled < max; i++) {
        const char *c = child_name(dirpath, initrd_entry(i)->name);
        if (!c)
            continue;
        if (index++ < f->pos)
            continue;
        buf[filled].d_ino = i + 2;
        enum initrd_type type = initrd_entry(i)->type;
        buf[filled].d_type = type == INITRD_DIR ? DT_DIR : type == INITRD_LINK ? DT_LNK : DT_REG;
        strlcpy(buf[filled].d_name, c, sizeof buf[filled].d_name);
        filled++;
        f->pos++;
    }
    return (long)(filled * sizeof(struct dirent));
}

static int initrd_readlink(struct inode *ino, char *buf, size_t size)
{
    const struct initrd_entry *e = ino->priv;
    size_t n = MIN(size, e->size);
    memcpy(buf, e->data, n);
    return (int)n;
}

static const struct inode_ops initrd_dir_ops = {
    .lookup = initrd_lookup_op,
};
static const struct inode_ops initrd_link_ops = {
    .readlink = initrd_readlink,
};
static const struct file_ops initrd_file_fops = {
    .read = initrd_read,
};
static const struct file_ops initrd_dir_fops = {
    .getdents = initrd_getdents,
};

static int initrd_read_inode(struct superblock *sb, uint64_t ino, struct inode *i)
{
    if (ino == ROOT_INO) {
        i->mode = S_IFDIR | 0755;
        i->nlink = 2;
        i->ops = &initrd_dir_ops;
        i->fops = &initrd_dir_fops;
        i->priv = NULL;
        i->mtime = (int64_t)initrd_newest_mtime() * 1000000000;
        return 0;
    }
    const struct initrd_entry *e = initrd_entry(ino - 2);
    if (!e)
        return -ENOENT;
    i->priv = (void *)e;
    i->nlink = 1;
    i->mtime = (int64_t)e->mtime * 1000000000;
    i->uid = e->uid;
    i->gid = e->gid;
    if (e->type == INITRD_DIR) {
        i->mode = S_IFDIR | e->mode;
        i->ops = &initrd_dir_ops;
        i->fops = &initrd_dir_fops;
    } else if (e->type == INITRD_LINK) {
        i->mode = S_IFLNK | 0777;
        i->size = e->size;
        i->ops = &initrd_link_ops;
    } else {
        i->mode = S_IFREG | e->mode;
        i->size = e->size;
        i->fops = &initrd_file_fops;
    }
    return 0;
}

static const struct sb_ops initrd_sb_ops = {
    .read_inode = initrd_read_inode,
};

static int initrd_mount(const struct fs_type *type, const char *source, const char *options,
                        struct superblock **out)
{
    if (options[0])
        return -EINVAL;
    struct superblock *sb = sb_alloc(type, &initrd_sb_ops);
    if (!sb)
        return -ENOMEM;
    sb->root_ino = ROOT_INO;
    *out = sb;
    return 0;
}

static struct fs_type initrd_fs_type = {
    .name = "initrd",
    .mount = initrd_mount,
};

void initrdfs_init(void)
{
    vfs_register_fs(&initrd_fs_type);
}
