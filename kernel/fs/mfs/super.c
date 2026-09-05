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

int mfs_super_journal(struct mfs_sb *m)
{
    kassert(mutex_held(&m->lock));
    struct buf *b = bread(m->dev, 0);
    if (!b)
        return -EIO;
    memcpy(b->data, &m->sb, sizeof m->sb);
    mfs_journal_write(m, b);
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
    i->mtime = (int64_t)d->mtime;
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
    d->mtime = (uint64_t)ino->mtime;
    memcpy(d->direct, info->direct, sizeof d->direct);
    d->indirect = info->indirect;
    d->dindirect = info->dindirect;
    mfs_journal_write(m, b);
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
    d->mtime = (uint64_t)vfs_now();
    mfs_journal_write(m, b);
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
        /* The last reference to an unlinked inode may go away outside any
         * operation (the close of an unlinked file); release its storage
         * in a transaction of its own then. */
        mfs_journal_begin(m);
        mfs_free_all_blocks(ino);
        mfs_free_inode(m, (uint32_t)ino->ino);
        mfs_journal_end(m);
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
    if (m->journal.crash)
        return 0;                   /* simulated power loss: nothing reaches the disk */
    int r = mfs_journal_flush(m);
    int e = mfs_write_super(m);
    if (e == 0)
        e = bcache_sync(m->dev);
    return r ? r : e;
}

static void mfs_unmount(struct superblock *sb)
{
    struct mfs_sb *m = mfs_of(sb);
    if (m->journal.crash) {
        mfs_journal_discard(m);
        klog_warn("%s unmounted after a simulated crash", m->dev->name);
    } else {
        mfs_journal_flush(m);
        mutex_lock(&m->lock);
        m->sb.flags |= MFS_FLAG_CLEAN;
        mutex_unlock(&m->lock);
        mfs_write_super(m);
        bcache_sync(m->dev);
        klog_info("%s unmounted clean, %lu free blocks", m->dev->name, m->sb.free_blocks);
    }
    mfs_journal_destroy(m);
    kfree(m);
    kfree(sb);
}

static void mfs_op_begin(struct superblock *sb)
{
    mfs_journal_begin(mfs_of(sb));
}

static void mfs_op_end(struct superblock *sb)
{
    mfs_journal_end(mfs_of(sb));
}

static const struct sb_ops mfs_sb_ops = {
    .read_inode = mfs_read_inode,
    .put_inode = mfs_put_inode,
    .free_inode = mfs_drop_inode,
    .sync = mfs_sync,
    .unmount = mfs_unmount,
    .op_begin = mfs_op_begin,
    .op_end = mfs_op_end,
};

static int read_super(struct mfs_sb *m)
{
    struct buf *b = bread(m->dev, 0);
    if (!b)
        return -EIO;
    memcpy(&m->sb, b->data, sizeof m->sb);
    brelse(b);
    return 0;
}

static int mfs_mount(const struct fs_type *type, const char *source, struct superblock **out)
{
    struct blockdev *dev = blockdev_find(source);
    if (!dev)
        return -ENODEV;
    struct mfs_sb *m = kzalloc(sizeof *m);
    if (!m)
        return -ENOMEM;
    m->dev = dev;
    int r = read_super(m);
    if (r < 0) {
        kfree(m);
        return r;
    }
    if (m->sb.magic != MFS_MAGIC || m->sb.block_size != MFS_BLOCK_SIZE) {
        kfree(m);
        return -EINVAL;
    }
    if (m->sb.version != MFS_VERSION) {
        klog_error("%s: format version %u, expected %u (rebuild the image)", source,
                   m->sb.version, MFS_VERSION);
        kfree(m);
        return -EINVAL;
    }
    if (m->sb.nblocks * MFS_BLOCK_SIZE > blockdev_size(dev) ||
        m->sb.journal_blocks != MFS_JOURNAL_BLOCKS ||
        m->sb.data_start != m->sb.journal_start + m->sb.journal_blocks ||
        m->sb.data_start >= m->sb.nblocks) {
        klog_error("%s: superblock geometry invalid", source);
        kfree(m);
        return -EINVAL;
    }
    mutex_init(&m->lock, "mfs");
    r = mfs_journal_init(m);
    if (r < 0) {
        kfree(m);
        return r;
    }
    bool was_clean = m->sb.flags & MFS_FLAG_CLEAN;
    int replayed = mfs_journal_recover(m);
    if (replayed < 0) {
        klog_error("%s: journal recovery failed: %d", source, replayed);
        mfs_journal_destroy(m);
        kfree(m);
        return replayed;
    }
    if (replayed > 0) {
        klog_info("%s: journal replayed, %d blocks", source, replayed);
        r = read_super(m);      /* the transaction may have held block 0 */
        if (r < 0) {
            mfs_journal_destroy(m);
            kfree(m);
            return r;
        }
    }
    struct superblock *sb = sb_alloc(type, &mfs_sb_ops);
    if (!sb) {
        mfs_journal_destroy(m);
        kfree(m);
        return -ENOMEM;
    }
    sb->priv = m;
    sb->root_ino = MFS_ROOT_INO;
    sb->dev = dev->nsectors;    /* any stable non zero identifier */
    if (!was_clean)
        klog_warn("%s: previous shutdown was unclean", source);
    m->sb.flags &= ~MFS_FLAG_CLEAN;
    m->sb.mount_count++;
    r = mfs_write_super(m);
    if (r == 0)
        r = bcache_sync(dev);
    if (r < 0) {
        kfree(sb);
        mfs_journal_destroy(m);
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
