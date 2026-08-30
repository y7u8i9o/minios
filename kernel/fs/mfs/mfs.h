#pragma once
/* Private definitions of the mfs implementation. */
#include <fs/vfs.h>
#include <fs/mfs_format.h>
#include <block/blockdev.h>
#include <block/bcache.h>
#include <sync/mutex.h>

/* Per mount state. lock protects the bitmaps and the counters in sb; the
 * other fields are constant after mount. */
struct mfs_sb {
    struct blockdev *dev;
    struct mfs_superblock sb;
    struct mutex lock;
};

/* Per inode state: the on disk block pointers. Protected by inode->lock. */
struct mfs_inode_info {
    uint32_t direct[MFS_NDIRECT];
    uint32_t indirect;
    uint32_t dindirect;
};

extern const struct inode_ops mfs_dir_ops;
extern const struct file_ops mfs_dir_fops;
extern const struct file_ops mfs_file_fops;

/* super.c */
void mfs_init(void);
int mfs_write_super(struct mfs_sb *m);
/* Write the inode metadata to the inode table. Caller holds ino->lock or
 * is the only user of a fresh inode. */
int mfs_inode_flush(struct inode *ino);
/* Create a fresh inode of the given mode with nlink links. Returns it
 * referenced, or NULL. */
struct inode *mfs_inode_new(struct superblock *sb, uint32_t mode, uint32_t nlink);

/* bitmap.c */
uint32_t mfs_alloc_block(struct mfs_sb *m);
void mfs_free_block(struct mfs_sb *m, uint32_t block);
uint32_t mfs_alloc_inode(struct mfs_sb *m);
void mfs_free_inode(struct mfs_sb *m, uint32_t ino);

/* inode.c: all take ino->lock held by the caller. */
long mfs_read_locked(struct inode *ino, char *buf, size_t n, uint64_t off);
long mfs_write_locked(struct inode *ino, const char *buf, size_t n, uint64_t off);
int mfs_truncate_locked(struct inode *ino, uint64_t size);
/* Release every data block; used when an unlinked inode goes away. */
void mfs_free_all_blocks(struct inode *ino);
