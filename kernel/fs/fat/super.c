#define KLOG_SUBSYS "fat"
/* FAT12/16/32 (M36): mounting, inodes and the superblock operations. */
#include "fat.h"
#include <fs/fat.h>
#include <drivers/rtc.h>
#include <drivers/timer.h>
#include <mm/slab.h>
#include <lib/string.h>
#include <kassert.h>
#include <klog.h>
#include <errno.h>

int fat_rw(struct fat_sb *m, uint64_t off, void *buf, size_t n, bool write)
{
    uint8_t *p = buf;
    size_t done = 0;
    while (done < n) {
        uint64_t block = off / BCACHE_BLOCK_SIZE;
        size_t boff = off % BCACHE_BLOCK_SIZE;
        size_t chunk = MIN(n - done, BCACHE_BLOCK_SIZE - boff);
        struct buf *b = bread(m->dev, block);
        if (!b)
            return -EIO;
        if (write) {
            memcpy(b->data + boff, p + done, chunk);
            bwrite(b);
        } else {
            memcpy(p + done, b->data + boff, chunk);
        }
        brelse(b);
        done += chunk;
        off += chunk;
    }
    return 0;
}

/* Days since the epoch for a calendar date (proleptic Gregorian). */
static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* Seconds since the epoch from a directory entry's date and time. */
static int64_t fat_epoch(uint16_t date, uint16_t time)
{
    int y = 1980 + (date >> 9), mo = (date >> 5) & 15, d = date & 31;
    if (mo < 1 || mo > 12 || d < 1)
        return 0;
    return days_from_civil(y, mo, d) * 86400 + (time >> 11) * 3600 + ((time >> 5) & 63) * 60 + (time & 31) * 2;
}

/* Calendar date from days since the epoch (proleptic Gregorian). */
static void civil_from_days(int64_t z, int *y, int *mo, int *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *mo = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yy + (*mo <= 2));
}

void fat_now(uint16_t *date, uint16_t *time)
{
    uint64_t secs = (rtc_epoch_offset_ns() + timer_ns()) / 1000000000ull;
    int y, mo, d;
    civil_from_days((int64_t)(secs / 86400), &y, &mo, &d);
    uint64_t sod = secs % 86400;
    if (y < 1980)
        y = 1980;
    if (y > 2107)
        y = 2107;
    *date = (uint16_t)((y - 1980) << 9 | mo << 5 | d);
    *time = (uint16_t)((sod / 3600) << 11 | ((sod / 60) % 60) << 5 | (sod % 60) / 2);
}

/* ---- inodes ---- */

static uint32_t chain_length(struct fat_sb *m, uint32_t first)
{
    uint32_t n = 0, c = first;
    while (c >= 2 && c < m->nclusters + 2 && n <= m->nclusters) {
        n++;
        c = fat_get(m, c);
        if (fat_is_eoc(m, c))
            break;
    }
    return n;
}

static void set_ops(struct inode *i)
{
    i->ops = &fat_dir_ops;          /* truncate is the only file operation in it */
    i->fops = S_ISDIR(i->mode) ? &fat_dir_fops : &fat_file_fops;
}

static int fat_read_inode(struct superblock *sb, uint64_t ino, struct inode *i)
{
    struct fat_sb *m = sb->priv;
    struct fat_inode_info *info = kzalloc(sizeof *info);
    if (!info)
        return -ENOMEM;
    i->priv = info;
    if (ino == FAT_ROOT_INO) {
        i->mode = S_IFDIR | 0755;
        i->nlink = 2;
        info->attr = FAT_ATTR_DIRECTORY;
        if (m->type == 32) {
            info->first_cluster = m->root_cluster;
            i->size = (uint64_t)chain_length(m, m->root_cluster) * m->cluster_bytes;
        } else {
            i->size = (uint64_t)m->root_entries * 32;
        }
        set_ops(i);
        return 0;
    }
    struct fat_dirent e;
    uint64_t off = ino * 32;
    if (off + 32 > blockdev_size(m->dev) || fat_read(m, off, &e, sizeof e) < 0) {
        kfree(info);
        return -EIO;
    }
    if (e.name[0] == FAT_NAME_FREE || e.name[0] == FAT_NAME_DELETED || e.attr == FAT_ATTR_LFN ||
        (e.attr & FAT_ATTR_VOLUME)) {
        kfree(info);
        return -ENOENT;
    }
    info->entry_off = off;
    info->attr = e.attr;
    info->first_cluster = e.cluster_lo | (m->type == 32 ? (uint32_t)e.cluster_hi << 16 : 0);
    info->ctime = e.ctime;
    info->cdate = e.cdate;
    info->ctime_tenths = e.ctime_tenths;
    info->adate = e.adate;
    i->mtime = fat_epoch(e.mdate, e.mtime);
    if (e.attr & FAT_ATTR_DIRECTORY) {
        i->mode = S_IFDIR | 0755;
        i->nlink = 2;
        i->size = (uint64_t)chain_length(m, info->first_cluster) * m->cluster_bytes;
    } else {
        i->mode = S_IFREG | ((e.attr & FAT_ATTR_READ_ONLY) ? 0444 : 0644);
        i->nlink = 1;
        i->size = e.size;
    }
    set_ops(i);
    return 0;
}

int fat_inode_flush(struct inode *ino)
{
    struct fat_sb *m = fat_of(ino);
    struct fat_inode_info *info = ino->priv;
    if (!info->entry_off || info->unlinked)
        return 0;
    struct fat_dirent e;
    int r = fat_read(m, info->entry_off, &e, sizeof e);
    if (r < 0)
        return r;
    e.cluster_lo = (uint16_t)info->first_cluster;
    e.cluster_hi = m->type == 32 ? (uint16_t)(info->first_cluster >> 16) : 0;
    e.size = S_ISDIR(ino->mode) ? 0 : (uint32_t)ino->size;
    uint16_t date, time;
    fat_now(&date, &time);
    e.mdate = e.adate = date;
    e.mtime = time;
    ino->mtime = vfs_now();
    return fat_write(m, info->entry_off, &e, sizeof e);
}

static void fat_put_inode(struct inode *ino)
{
    struct fat_inode_info *info = ino->priv;
    if (info->unlinked)
        fat_inode_release(ino);
    kfree(info);
}

static void fat_drop_inode(struct inode *ino)
{
    kfree(ino->priv);
}

static int write_fsinfo(struct fat_sb *m)
{
    if (m->type != 32)
        return 0;
    struct fat_fsinfo fi;
    int r = fat_read(m, 1 * FAT_SECTOR_SIZE, &fi, sizeof fi);
    if (r < 0)
        return r;
    if (fi.lead_signature != FAT_FSINFO_LEAD || fi.struct_signature != FAT_FSINFO_STRUCT)
        return 0;
    mutex_lock(&m->lock);
    fi.free_clusters = m->free_clusters;
    fi.next_free = m->next_free;
    mutex_unlock(&m->lock);
    return fat_write(m, 1 * FAT_SECTOR_SIZE, &fi, sizeof fi);
}

static int fat_sync(struct superblock *sb)
{
    struct fat_sb *m = sb->priv;
    int r = write_fsinfo(m);
    int e = bcache_sync(m->dev);
    return r ? r : e;
}

static void fat_unmount(struct superblock *sb)
{
    struct fat_sb *m = sb->priv;
    fat_sync(sb);
    klog_info("%s unmounted, %u free clusters", m->dev->name, m->free_clusters);
    kfree(m);
    kfree(sb);
}

static int fat_statfs(struct superblock *sb, struct fs_space *space)
{
    struct fat_sb *m = sb->priv;
    mutex_lock(&m->lock);
    space->blocks = m->nclusters;
    space->free_blocks = m->free_clusters;
    space->block_size = m->cluster_bytes;
    mutex_unlock(&m->lock);
    return 0;
}

static const struct sb_ops fat_sb_ops = {
    .read_inode = fat_read_inode,
    .put_inode = fat_put_inode,
    .free_inode = fat_drop_inode,
    .sync = fat_sync,
    .unmount = fat_unmount,
    .statfs = fat_statfs,
};

/* ---- mount ---- */

static int fat_mount(const struct fs_type *type, const char *source, struct superblock **out)
{
    struct blockdev *dev = blockdev_find(source);
    if (!dev)
        return -ENODEV;
    struct fat_sb *m = kzalloc(sizeof *m);
    if (!m)
        return -ENOMEM;
    m->dev = dev;
    struct fat_bpb bpb;
    uint8_t sig[2];
    if (fat_read(m, 0, &bpb, sizeof bpb) < 0 || fat_read(m, 510, sig, 2) < 0) {
        kfree(m);
        return -EIO;
    }
    if (sig[0] != 0x55 || sig[1] != 0xaa || bpb.bytes_per_sector != FAT_SECTOR_SIZE ||
        bpb.sectors_per_cluster == 0 || (bpb.sectors_per_cluster & (bpb.sectors_per_cluster - 1)) ||
        bpb.nfats == 0 || bpb.nfats > 2 || bpb.reserved_sectors == 0) {
        kfree(m);
        return -EINVAL;
    }
    uint32_t total = bpb.total_sectors16 ? bpb.total_sectors16 : bpb.total_sectors32;
    uint32_t fat_size = bpb.fat_size16 ? bpb.fat_size16 : bpb.f32.fat_size32;
    if (total == 0 || fat_size == 0 || (uint64_t)total * FAT_SECTOR_SIZE > blockdev_size(dev)) {
        klog_error("%s: geometry invalid or larger than the device", source);
        kfree(m);
        return -EINVAL;
    }
    m->spc = bpb.sectors_per_cluster;
    m->cluster_bytes = m->spc * FAT_SECTOR_SIZE;
    m->nfats = bpb.nfats;
    m->fat_start = bpb.reserved_sectors;
    m->fat_sectors = fat_size;
    m->root_start = m->fat_start + m->nfats * fat_size;
    m->root_entries = bpb.root_entries;
    uint32_t root_sectors = (m->root_entries * 32 + FAT_SECTOR_SIZE - 1) / FAT_SECTOR_SIZE;
    m->data_start = m->root_start + root_sectors;
    if (m->data_start >= total) {
        kfree(m);
        return -EINVAL;
    }
    m->nclusters = (total - m->data_start) / m->spc;
    m->type = m->nclusters < FAT12_MAX_CLUSTERS ? 12 : m->nclusters < FAT16_MAX_CLUSTERS ? 16 : 32;
    if (m->type == 32) {
        if (bpb.fat_size16 != 0 || bpb.root_entries != 0 || bpb.f32.root_cluster < 2) {
            kfree(m);
            return -EINVAL;
        }
        m->root_cluster = bpb.f32.root_cluster;
    } else if (bpb.root_entries == 0) {
        kfree(m);
        return -EINVAL;
    }
    /* The table must cover every cluster. */
    uint64_t need = m->type == 12 ? ((uint64_t)m->nclusters + 2) * 3 / 2 : ((uint64_t)m->nclusters + 2) * (m->type == 16 ? 2 : 4);
    if (need > (uint64_t)fat_size * FAT_SECTOR_SIZE) {
        klog_error("%s: allocation table too small for %u clusters", source, m->nclusters);
        kfree(m);
        return -EINVAL;
    }
    mutex_init(&m->lock, "fat");
    m->next_free = 2;
    m->free_clusters = fat_count_free(m);
    struct superblock *sb = sb_alloc(type, &fat_sb_ops);
    if (!sb) {
        kfree(m);
        return -ENOMEM;
    }
    sb->priv = m;
    sb->root_ino = FAT_ROOT_INO;
    sb->dev = dev->nsectors + 1;
    klog_info("%s: FAT%d, %u clusters of %u bytes, %u free, %u FATs", source, m->type, m->nclusters,
              m->cluster_bytes, m->free_clusters, m->nfats);
    *out = sb;
    return 0;
}

static struct fs_type fat_type = {
    .name = "fat",
    .mount = fat_mount,
};

void fat_init(void)
{
    vfs_register_fs(&fat_type);
}
