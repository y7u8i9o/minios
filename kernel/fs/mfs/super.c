#define KLOG_SUBSYS "mfs"
#include "mfs.h"
#include <mm/slab.h>
#include <lib/string.h>
#include <kassert.h>
#include <klog.h>
#include <errno.h>

static struct mfs_sb *mfs_of(struct superblock *sb)
{
    return sb->priv;
}

int mfs_write_super(struct mfs_sb *m)
{
    struct buf *b = bread(m->dev, 0);
    if (!b)
        return -EIO;
    mutex_lock(&m->lock);
    memcpy(b->data, &m->sb, sizeof m->sb);
    mutex_unlock(&m->lock);
    bwrite(b);
    brelse(b);
    return 0;
}

/* Locate the on disk inode: block and offset inside it. */
static struct buf *dinode_buf(struct mfs_sb *m, uint64_t ino, struct mfs_dinode **out)
{
    if (ino == 0 || ino >= m->sb.ninodes)
        return NULL;
    struct buf *b = bread(m->dev, m->sb.inode_table_start + ino / MFS_INODES_PER_BLOCK);
    if (!b)
        return NULL;
    *out = (struct mfs_dinode *)(b->data + (ino % MFS_INODES_PER_BLOCK) * MFS_INODE_SIZE);
    return b;
}

static int mfs_read_inode(struct superblock *sb, uint64_t ino, struct inode *i)
{
    struct mfs_sb *m = mfs_of(sb);
    struct mfs_dinode *d;
    struct buf *b = dinode_buf(m, ino, &d);
    if (!b)
        return -EIO;
    struct mfs_inode_info *info = kzalloc(sizeof *info);
    if (!info) {
        brelse(b);
        return -ENOMEM;
    }
    i->mode = d->mode;
    i->nlink = d->nlink;
    i->size = d->size;
    memcpy(info->direct, d->direct, sizeof info->direct);
    info->indirect = d->indirect;
    info->dindirect = d->dindirect;
    brelse(b);
    i->priv = info;
    if (S_ISDIR(i->mode)) {
        i->ops = &mfs_dir_ops;
        i->fops = &mfs_dir_fops;
    } else {
        i->ops = &mfs_dir_ops;      /* only truncate applies to files */
        i->fops = &mfs_file_fops;
    }
    return 0;
}

int mfs_inode_flush(struct inode *ino)
{
    struct mfs_sb *m = mfs_of(ino->sb);
    struct mfs_inode_info *info = ino->priv;
    struct mfs_dinode *d;
    struct buf *b = dinode_buf(m, ino->ino, &d);
    if (!b)
        return -EIO;
    d->mode = ino->mode;
    d->nlink = ino->nlink;
    d->size = ino->size;
    memcpy(d->direct, info->direct, sizeof d->direct);
    d->indirect = info->indirect;
    d->dindirect = info->dindirect;
    bwrite(b);
    brelse(b);
    return 0;
}

struct inode *mfs_inode_new(struct superblock *sb, uint32_t mode, uint32_t nlink)
{
    struct mfs_sb *m = mfs_of(sb);
    uint32_t num = mfs_alloc_inode(m);
    if (!num)
        return NULL;
    struct mfs_dinode *d;
    struct buf *b = dinode_buf(m, num, &d);
    if (!b) {
        mfs_free_inode(m, num);
        return NULL;
    }
    memset(d, 0, sizeof *d);
    d->mode = mode;
    d->nlink = nlink;
    bwrite(b);
    brelse(b);
    struct inode *ino = inode_get(sb, num);
    if (!ino)
        mfs_free_inode(m, num);
    return ino;
}

static void mfs_put_inode(struct inode *ino)
{
    struct mfs_sb *m = mfs_of(ino->sb);
    if (ino->nlink == 0) {
        mfs_free_all_blocks(ino);
        mfs_free_inode(m, (uint32_t)ino->ino);
    }
    kfree(ino->priv);
}

static void mfs_drop_inode(struct inode *ino)
{
    kfree(ino->priv);
}

static int mfs_sync(struct superblock *sb)
{
    struct mfs_sb *m = mfs_of(sb);
    int r = mfs_write_super(m);
    int e = bcache_sync(m->dev);
    return r ? r : e;
}

static void mfs_unmount(struct superblock *sb)
{
    struct mfs_sb *m = mfs_of(sb);
    mutex_lock(&m->lock);
    m->sb.flags |= MFS_FLAG_CLEAN;
    mutex_unlock(&m->lock);
    mfs_write_super(m);
    bcache_sync(m->dev);
    klog_info("%s unmounted clean, %lu free blocks", m->dev->name, m->sb.free_blocks);
    kfree(m);
    kfree(sb);
}

static const struct sb_ops mfs_sb_ops = {
    .read_inode = mfs_read_inode,
    .put_inode = mfs_put_inode,
    .free_inode = mfs_drop_inode,
    .sync = mfs_sync,
    .unmount = mfs_unmount,
};

static int mfs_mount(const struct fs_type *type, const char *source, struct superblock **out)
{
    struct blockdev *dev = blockdev_find(source);
    if (!dev)
        return -ENODEV;
    struct buf *b = bread(dev, 0);
    if (!b)
        return -EIO;
    struct mfs_sb *m = kzalloc(sizeof *m);
    if (!m) {
        brelse(b);
        return -ENOMEM;
    }
    memcpy(&m->sb, b->data, sizeof m->sb);
    brelse(b);
    if (m->sb.magic != MFS_MAGIC || m->sb.version != MFS_VERSION ||
        m->sb.block_size != MFS_BLOCK_SIZE) {
        kfree(m);
        return -EINVAL;
    }
    if (m->sb.nblocks * MFS_BLOCK_SIZE > blockdev_size(dev)) {
        klog_error("%s: filesystem larger than device", source);
        kfree(m);
        return -EINVAL;
    }
    m->dev = dev;
    mutex_init(&m->lock, "mfs");
    struct superblock *sb = sb_alloc(type, &mfs_sb_ops);
    if (!sb) {
        kfree(m);
        return -ENOMEM;
    }
    sb->priv = m;
    sb->root_ino = MFS_ROOT_INO;
    sb->dev = dev->nsectors;    /* any stable non zero identifier */
    bool was_clean = m->sb.flags & MFS_FLAG_CLEAN;
    if (!was_clean)
        klog_warn("%s: previous shutdown was unclean", source);
    m->sb.flags &= ~MFS_FLAG_CLEAN;
    m->sb.mount_count++;
    int r = mfs_write_super(m);
    if (r == 0)
        r = bcache_sync(dev);
    if (r < 0) {
        kfree(sb);
        kfree(m);
        return r;
    }
    klog_info("%s: %lu blocks, %u inodes, %lu free blocks, mount %u, %s", source,
              m->sb.nblocks, m->sb.ninodes, m->sb.free_blocks, m->sb.mount_count,
              was_clean ? "clean" : "unclean");
    *out = sb;
    return 0;
}

static struct fs_type mfs_type = {
    .name = "mfs",
    .mount = mfs_mount,
};

void mfs_init(void)
{
    vfs_register_fs(&mfs_type);
}
