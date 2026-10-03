#pragma once
/* Private definitions of the mfs implementation. */
#include <fs/vfs.h>
#include <fs/mfs_format.h>
#include <block/blockdev.h>
#include <block/bcache.h>
#include <sync/mutex.h>
#include <sched/wait.h>

/* Blocks one operation may add to the transaction; op_begin waits until
 * this many slots are free. */
#define MFS_JOURNAL_RESERVE 16

/* The journal of one mount (M36). lock protects every counter and the
 * buffer list and is the condition lock of wq; the header block and the
 * sequence number are used only by the committing thread. */
struct mfs_journal {
    struct spinlock lock;
    struct waitq wq;
    int outstanding;                /* operations between op_begin and op_end */
    int reserved;                   /* slots reserved by those operations */
    int flush_waiters;              /* threads waiting to commit in mfs_journal_flush */
    bool committing;
    int nbufs;                      /* pinned buffers of the open transaction */
    struct buf *bufs[MFS_JOURNAL_SLOTS];
    uint64_t sequence;
    struct mfs_journal_header *header;  /* one block, written raw */
    int crash;                      /* test hook, see mfs_journal_set_crash */
};

#define MFS_CRASH_NONE          0
#define MFS_CRASH_BEFORE_COMMIT 1   /* nothing of the transaction reaches the disk */
#define MFS_CRASH_AFTER_COMMIT  2   /* the journal is durable, the home blocks are not */

/* Per mount state. lock protects the bitmaps and the counters in sb; the
 * other fields are constant after mount. */
struct mfs_sb {
    struct blockdev *dev;
    struct mfs_superblock sb;
    struct mutex lock;
    struct mfs_journal journal;
};

/* Per inode state: the on disk block pointers. Protected by inode->lock. */
struct mfs_inode_info {
    uint32_t direct[MFS_NDIRECT];
    uint32_t indirect;
    uint32_t dindirect;
};

extern const struct inode_ops mfs_dir_ops;
extern const struct inode_ops mfs_link_ops;
extern const struct file_ops mfs_dir_fops;
extern const struct file_ops mfs_file_fops;

/* super.c */
void mfs_init(void);
/* Write the superblock directly (mount and unmount, outside transactions). */
int mfs_write_super(struct mfs_sb *m);
/* Write the superblock counters through the journal. Caller holds m->lock. */
int mfs_super_journal(struct mfs_sb *m);
/* Write the inode metadata to the inode table. Caller holds ino->lock or
 * is the only user of a fresh inode. */
int mfs_inode_flush(struct inode *ino);
/* Create a fresh inode of the given mode with nlink links. Returns it
 * referenced, or NULL. */
struct inode *mfs_inode_new(struct inode *dir, uint32_t mode, uint32_t nlink);

/* bitmap.c */
uint32_t mfs_alloc_block(struct mfs_sb *m);
void mfs_free_block(struct mfs_sb *m, uint32_t block);
uint32_t mfs_alloc_inode(struct mfs_sb *m);
void mfs_free_inode(struct mfs_sb *m, uint32_t ino);
/* True when the inode bitmap marks ino allocated. */
int mfs_inode_allocated(struct mfs_sb *m, uint32_t ino);

/* inode.c: all take ino->lock held by the caller. */
long mfs_read_locked(struct inode *ino, char *buf, size_t n, uint64_t off);
long mfs_write_locked(struct inode *ino, const char *buf, size_t n, uint64_t off);
int mfs_truncate_locked(struct inode *ino, uint64_t size);
/* Release every data block; used when an unlinked inode goes away. */
void mfs_free_all_blocks(struct inode *ino);

/* journal.c */
int mfs_journal_init(struct mfs_sb *m);
void mfs_journal_destroy(struct mfs_sb *m);
/* Replay a committed transaction left in the journal. Returns the number
 * of blocks written, or -errno. */
int mfs_journal_recover(struct mfs_sb *m);
/* Enter and leave a transaction; nested calls by the same thread are
 * counted. The last op_end commits the group. */
void mfs_journal_begin(struct mfs_sb *m);
void mfs_journal_end(struct mfs_sb *m);
/* Add a locked, modified metadata buffer to the current transaction. */
void mfs_journal_write(struct mfs_sb *m, struct buf *b);
/* Wait for running operations, then commit whatever is pending. */
int mfs_journal_flush(struct mfs_sb *m);
/* Test hooks: make the next commit stop at the given point, and throw
 * away everything that was not written. */
void mfs_journal_set_crash(struct superblock *sb, int mode);
void mfs_journal_discard(struct mfs_sb *m);
