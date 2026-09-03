#define KLOG_SUBSYS "mfs"
/* The mfs journal (M36): a write ahead log of metadata blocks.
 *
 * Every modifying operation runs between mfs_journal_begin and
 * mfs_journal_end. Metadata buffers it changes are pinned in the block
 * cache instead of being written back; data blocks are written back
 * normally. When the last operation of a group ends, the committing thread
 * writes the dirty data blocks (so that no committed metadata points at
 * unwritten data), then the pinned blocks to the journal slots, then the
 * header with the count and checksum, then the blocks to their home
 * locations, and finally the header with count zero. A crash before the
 * header is durable loses the group entirely; a crash after it is repaired
 * at the next mount by mfs_journal_recover, which copies the slots home. */
#include "mfs.h"
#include <sched/thread.h>
#include <mm/slab.h>
#include <lib/string.h>
#include <lib/crc32.h>
#include <kassert.h>
#include <klog.h>
#include <errno.h>

static int raw_rw(struct mfs_sb *m, uint64_t block, void *data, bool write)
{
    uint32_t spb = MFS_BLOCK_SIZE / m->dev->sector_size;
    return write ? blockdev_write(m->dev, block * spb, spb, data)
                 : blockdev_read(m->dev, block * spb, spb, data);
}

static int dev_flush(struct mfs_sb *m)
{
    return m->dev->flush ? m->dev->flush(m->dev) : 0;
}

int mfs_journal_init(struct mfs_sb *m)
{
    struct mfs_journal *j = &m->journal;
    memset(j, 0, sizeof *j);
    spinlock_init(&j->lock, "mfs_journal");
    waitq_init(&j->wq, "mfs_journal");
    j->header = kmalloc(MFS_BLOCK_SIZE);
    if (!j->header)
        return -ENOMEM;
    return 0;
}

void mfs_journal_destroy(struct mfs_sb *m)
{
    kassert(m->journal.nbufs == 0);
    kfree(m->journal.header);
    m->journal.header = NULL;
}

/* Checksum of the header (its checksum field zero) followed by the slot
 * contents given one at a time. */
static uint32_t header_crc(const struct mfs_journal_header *h)
{
    struct mfs_journal_header copy = *h;
    copy.checksum = 0;
    return crc32(0, &copy, sizeof copy);
}

static int write_header(struct mfs_sb *m, uint32_t count, uint32_t checksum)
{
    struct mfs_journal_header *h = m->journal.header;
    h->count = count;
    h->checksum = checksum;
    int r = raw_rw(m, m->sb.journal_start, h, true);
    if (r < 0)
        return r;
    return dev_flush(m);
}

int mfs_journal_recover(struct mfs_sb *m)
{
    struct mfs_journal *j = &m->journal;
    struct mfs_journal_header *h = j->header;
    uint8_t *slot = kmalloc(MFS_BLOCK_SIZE);
    if (!slot)
        return -ENOMEM;
    int r = raw_rw(m, m->sb.journal_start, h, false);
    if (r < 0)
        goto out;
    if (h->magic != MFS_JOURNAL_MAGIC) {
        klog_warn("%s: journal header invalid, starting a new one", m->dev->name);
        memset(h, 0, MFS_BLOCK_SIZE);
        h->magic = MFS_JOURNAL_MAGIC;
        h->sequence = 1;
        r = write_header(m, 0, 0);
        goto out;
    }
    j->sequence = h->sequence;
    if (h->count == 0) {
        r = 0;
        goto out;
    }
    bool valid = h->count <= MFS_JOURNAL_SLOTS;
    for (uint32_t i = 0; valid && i < h->count; i++)
        if (h->block[i] >= m->sb.nblocks ||
            (h->block[i] >= m->sb.journal_start && h->block[i] < m->sb.data_start))
            valid = false;
    uint32_t crc = valid ? header_crc(h) : 0;
    for (uint32_t i = 0; valid && i < h->count; i++) {
        r = raw_rw(m, m->sb.journal_start + 1 + i, slot, false);
        if (r < 0)
            goto out;
        crc = crc32(crc, slot, MFS_BLOCK_SIZE);
    }
    if (!valid || crc != h->checksum) {
        klog_warn("%s: discarding an incomplete transaction of %u blocks", m->dev->name, h->count);
        r = write_header(m, 0, 0);
        goto out;
    }
    uint32_t n = h->count;
    for (uint32_t i = 0; i < n; i++) {
        r = raw_rw(m, m->sb.journal_start + 1 + i, slot, false);
        if (r < 0)
            goto out;
        /* Through the cache, so that a block read before recovery (the
         * superblock) is updated as well. */
        struct buf *b = bread(m->dev, h->block[i]);
        if (!b) {
            r = -EIO;
            goto out;
        }
        memcpy(b->data, slot, MFS_BLOCK_SIZE);
        r = bwrite_now(b);
        brelse(b);
        if (r < 0)
            goto out;
    }
    r = dev_flush(m);
    if (r == 0)
        r = write_header(m, 0, 0);
    if (r == 0)
        r = (int)n;
out:
    kfree(slot);
    return r;
}

/* ---- transactions ---- */

void mfs_journal_begin(struct mfs_sb *m)
{
    struct thread *t = thread_current();
    if (t->fs_txn) {
        kassert(t->fs_txn == m);
        t->fs_txn_depth++;
        return;
    }
    struct mfs_journal *j = &m->journal;
    spin_lock(&j->lock);
    while (j->committing || j->flush_waiters ||
           j->reserved + MFS_JOURNAL_RESERVE > MFS_JOURNAL_SLOTS)
        waitq_wait(&j->wq, &j->lock);
    j->outstanding++;
    j->reserved += MFS_JOURNAL_RESERVE;
    spin_unlock(&j->lock);
    t->fs_txn = m;
    t->fs_txn_depth = 1;
}

void mfs_journal_write(struct mfs_sb *m, struct buf *b)
{
    struct mfs_journal *j = &m->journal;
    kassert(thread_current()->fs_txn == m);
    kassert(b->dev == m->dev);
    if (!bpin(b))
        return;                     /* already in the transaction */
    spin_lock(&j->lock);
    kassert(j->nbufs < MFS_JOURNAL_SLOTS);
    j->bufs[j->nbufs++] = b;
    spin_unlock(&j->lock);
}

/* Write the open transaction. Called with committing set and no
 * operation outstanding, so the pinned set does not change. */
static int commit(struct mfs_sb *m)
{
    struct mfs_journal *j = &m->journal;
    int n = j->nbufs;
    if (n == 0)
        return 0;
    if (j->crash == MFS_CRASH_BEFORE_COMMIT)
        return 0;
    /* Ordered data: the blocks the metadata points at go first. */
    int r = bcache_sync(m->dev);
    if (r < 0)
        return r;
    struct mfs_journal_header *h = j->header;
    memset(h, 0, MFS_BLOCK_SIZE);
    h->magic = MFS_JOURNAL_MAGIC;
    h->count = (uint32_t)n;
    h->sequence = ++j->sequence;
    for (int i = 0; i < n; i++)
        h->block[i] = (uint32_t)j->bufs[i]->block;
    uint32_t crc = header_crc(h);
    for (int i = 0; i < n; i++) {
        struct buf *b = j->bufs[i];
        mutex_lock(&b->lock);
        crc = crc32(crc, b->data, MFS_BLOCK_SIZE);
        r = raw_rw(m, m->sb.journal_start + 1 + (uint32_t)i, b->data, true);
        mutex_unlock(&b->lock);
        if (r < 0)
            return r;
    }
    r = dev_flush(m);
    if (r < 0)
        return r;
    r = write_header(m, (uint32_t)n, crc);
    if (r < 0)
        return r;
    if (j->crash == MFS_CRASH_AFTER_COMMIT)
        return 0;
    /* Checkpoint: the blocks go home, then the journal is emptied. */
    for (int i = 0; i < n; i++) {
        struct buf *b = j->bufs[i];
        mutex_lock(&b->lock);
        int e = bwrite_now(b);
        bunpin(b);
        mutex_unlock(&b->lock);
        if (e < 0 && r == 0)
            r = e;
    }
    j->nbufs = 0;
    if (r < 0)
        return r;
    r = dev_flush(m);
    if (r == 0)
        r = write_header(m, 0, 0);
    return r;
}

void mfs_journal_end(struct mfs_sb *m)
{
    struct thread *t = thread_current();
    kassert(t->fs_txn == m && t->fs_txn_depth > 0);
    if (--t->fs_txn_depth > 0)
        return;
    t->fs_txn = NULL;
    struct mfs_journal *j = &m->journal;
    spin_lock(&j->lock);
    j->outstanding--;
    j->reserved -= MFS_JOURNAL_RESERVE;
    bool do_commit = j->outstanding == 0 && j->nbufs > 0 && !j->committing;
    if (do_commit)
        j->committing = true;
    spin_unlock(&j->lock);
    if (do_commit) {
        int r = commit(m);
        if (r < 0)
            klog_error("%s: commit failed: %d", m->dev->name, r);
        spin_lock(&j->lock);
        j->committing = false;
        spin_unlock(&j->lock);
    }
    waitq_wake_all(&j->wq);
}

int mfs_journal_flush(struct mfs_sb *m)
{
    struct mfs_journal *j = &m->journal;
    kassert(thread_current()->fs_txn == NULL);
    spin_lock(&j->lock);
    j->flush_waiters++;
    while (j->outstanding || j->committing)
        waitq_wait(&j->wq, &j->lock);
    j->committing = true;
    spin_unlock(&j->lock);
    int r = commit(m);
    spin_lock(&j->lock);
    j->committing = false;
    j->flush_waiters--;
    spin_unlock(&j->lock);
    waitq_wake_all(&j->wq);
    return r;
}

/* ---- test hooks ---- */

void mfs_journal_set_crash(struct superblock *sb, int mode)
{
    struct mfs_sb *m = sb->priv;
    m->journal.crash = mode;
}

void mfs_journal_discard(struct mfs_sb *m)
{
    struct mfs_journal *j = &m->journal;
    for (int i = 0; i < j->nbufs; i++) {
        struct buf *b = j->bufs[i];
        mutex_lock(&b->lock);
        bforget(b);
        bunpin(b);
        mutex_unlock(&b->lock);
    }
    j->nbufs = 0;
    bcache_discard(m->dev);
}
