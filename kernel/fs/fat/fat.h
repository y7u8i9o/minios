#pragma once
/* Private definitions of the FAT driver (M36). Every access to the
 * device goes through the block cache by byte offset, so cluster and
 * sector sizes need not match the cache block size. */
#include <fs/vfs.h>
#include <fs/fat_format.h>
#include <block/blockdev.h>
#include <block/bcache.h>
#include <sync/mutex.h>

/* Per mount state. lock protects the allocation table, next_free and
 * free_clusters; the geometry is constant after mount. */
struct fat_sb {
    struct blockdev *dev;
    int type;                       /* 12, 16 or 32 */
    uint32_t spc;                   /* sectors per cluster */
    uint32_t cluster_bytes;
    uint32_t nfats;
    uint32_t fat_start;             /* sectors */
    uint32_t fat_sectors;
    uint32_t root_start;            /* FAT12/16 root region, sectors */
    uint32_t root_entries;
    uint32_t root_cluster;          /* FAT32 */
    uint32_t data_start;            /* sectors */
    uint32_t nclusters;             /* usable clusters, numbered 2 .. nclusters + 1 */
    uint32_t next_free;
    uint32_t free_clusters;
    /* The owner and the mask of every file, from the mount options uid=,
     * gid= and umask= (U1). FAT stores neither. */
    uint32_t uid, gid, umask;
    struct mutex lock;
};

/* Per inode state. Protected by inode->lock. entry_off is the byte
 * offset of the short directory entry on the device (0 for the root, which
 * has none); the inode number is entry_off / 32, so it changes when a
 * rename moves the entry. */
struct fat_inode_info {
    uint32_t first_cluster;
    uint64_t entry_off;
    uint8_t attr;
    bool unlinked;                  /* entry gone: never write it back */
    uint16_t ctime, cdate, ctime_tenths, adate;
    uint32_t walk_index;            /* cluster index and number of the last walk */
    uint32_t walk_cluster;
};

#define FAT_ROOT_INO 1

/* One directory entry as the iterator presents it. */
struct fat_entry {
    char name[FAT_LFN_MAX * 3 + 1];  /* UTF-8, long name or lower cased short name */
    uint64_t off;                    /* byte offset of the short entry */
    uint32_t lfn_pieces;             /* long name entries before it */
    uint32_t first_cluster;
    uint32_t size;
    uint8_t attr;
    struct fat_dirent raw;
};

extern const struct inode_ops fat_dir_ops;
extern const struct file_ops fat_dir_fops;
extern const struct file_ops fat_file_fops;

/* super.c */
int fat_rw(struct fat_sb *m, uint64_t off, void *buf, size_t n, bool write);
static inline int fat_read(struct fat_sb *m, uint64_t off, void *buf, size_t n)
{
    return fat_rw(m, off, buf, n, false);
}
static inline int fat_write(struct fat_sb *m, uint64_t off, const void *buf, size_t n)
{
    return fat_rw(m, off, (void *)buf, n, true);
}
/* Write size, first cluster and modification time to the entry. Caller
 * holds ino->lock. */
int fat_inode_flush(struct inode *ino);
int fat_inode_flush_time(struct inode *ino, int64_t mtime);
/* Current time in FAT form. */
void fat_now(uint16_t *date, uint16_t *time);
void fat_time_of(int64_t ns, uint16_t *date, uint16_t *time);
/* The st_mode of an entry with the attributes attr on the mount m. */
uint32_t fat_mode(const struct fat_sb *m, uint8_t attr);

static inline struct fat_sb *fat_of(struct inode *ino)
{
    return ino->sb->priv;
}

/* table.c */
uint32_t fat_get(struct fat_sb *m, uint32_t cluster);
int fat_set(struct fat_sb *m, uint32_t cluster, uint32_t value);
bool fat_is_eoc(struct fat_sb *m, uint32_t value);
uint32_t fat_eoc(struct fat_sb *m);
/* Allocate a zeroed cluster and link it after prev (0: none). 0 on ENOSPC. */
uint32_t fat_alloc_cluster(struct fat_sb *m, uint32_t prev);
/* Free a chain starting at first. */
void fat_free_chain(struct fat_sb *m, uint32_t first);
/* Cluster of index idx in the inode's chain, allocating when alloc is
 * set (and setting first_cluster). 0 for none. Caller holds ino->lock. */
uint32_t fat_cluster_at(struct inode *ino, uint32_t idx, bool alloc);
uint64_t fat_cluster_off(struct fat_sb *m, uint32_t cluster);
uint32_t fat_count_free(struct fat_sb *m);

/* file.c: caller holds ino->lock. */
int fat_truncate_locked(struct inode *ino, uint64_t size);

/* dir.c */
/* Iterate a directory: index is the entry index from 0, advanced past
 * the entry returned. Returns 1 with an entry, 0 at the end, -errno. */
int fat_dir_next(struct inode *dir, uint32_t *index, struct fat_entry *e);
int fat_dir_find(struct inode *dir, const char *name, size_t len, struct fat_entry *e);
/* Free every cluster of an inode whose entry is gone. */
void fat_inode_release(struct inode *ino);
