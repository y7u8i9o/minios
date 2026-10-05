/* The read only ISO 9660 file system with the Rock Ridge extensions (R5 of
 * docs/plan/release-0.5.0.md, docs/design/iso9660.md).
 *
 * The search for the primary volume descriptor, the layouts of the volume
 * descriptor and of the directory record, the walk over the records of a
 * directory with the zero padding at the end of each logical block, the
 * names of the records with and without the Rock Ridge NM entry, and the
 * collection of the extents of a multi-extent file adapt
 * common/fs/iso9660.s2.c of Limine 10.8.5:
 *
 *   Copyright (C) 2019-2026 Mintsuki and contributors.
 *   BSD 2-Clause licence, third_party/limine/LICENSE.
 *
 * Limine reads a file by its path and stops the boot on an error. The
 * adaptation returns errors instead, and adds the VFS operations, the
 * inode numbers, the System Use Sharing Protocol (SP, CE, ST) and the
 * Rock Ridge entries other than NM: PX, PN, SL, TF, RE and CL, and NM
 * names that continue over several entries. */
#define KLOG_SUBSYS "iso9660"
#include <fs/iso9660.h>
#include <fs/vfs.h>
#include <block/blockdev.h>
#include <block/bcache.h>
#include <mm/slab.h>
#include <lib/date.h>
#include <lib/string.h>
#include <klog.h>
#include <errno.h>

#define ISO_FIRST_VD            16      /* the logical sector of the first volume descriptor */
#define ISO_VD_SIZE             2048
#define ISO_MAX_VDS             256     /* volume descriptors searched */
#define ISO_VD_PRIMARY          1
#define ISO_VD_TERMINATOR       255
#define ISO_MAX_EXTENTS         65536   /* extents of one multi-extent file */
#define ISO_MAX_CE              16      /* continuation areas of one record */
#define ISO_MAX_CE_LEN          65536

#define FLAG_DIRECTORY          0x02
#define FLAG_ASSOCIATED         0x04
#define FLAG_MULTI_EXTENT       0x80

/* Values stored in both byte orders. The little endian half is read. */
struct both16 {
    uint16_t le, be;
} __packed;

struct both32 {
    uint32_t le, be;
} __packed;

/* A directory record (ECMA-119 9.1). */
struct iso_dirent {
    uint8_t length;
    uint8_t ext_attr_length;
    struct both32 extent;
    struct both32 size;
    uint8_t date[7];
    uint8_t flags;
    uint8_t unit_size;
    uint8_t gap_size;
    struct both16 volume_seq;
    uint8_t name_len;
    char name[];
} __packed;

#define DIRENT_HEADER   33

/* The primary volume descriptor (ECMA-119 8.4), the fields read. */
#define PVD_VOLUME_ID   40
#define PVD_SPACE_SIZE  80
#define PVD_BLOCK_SIZE  128
#define PVD_ROOT        156

/* A mounted volume. The fields are set by the mount and read without a
 * lock afterwards. */
struct iso_sb {
    struct blockdev *dev;
    uint32_t block;                 /* the logical block size, 2048 */
    uint64_t space_blocks;
    bool rock_ridge;
    unsigned skip;                  /* bytes before the SUSP entries, from SP */
    uint64_t root_extent;           /* byte offset of the root directory */
    char volume_id[33];
};

/* The data of an inode. A file has its extents, a symbolic link its
 * target. Set by read_inode and freed with the inode. */
struct iso_extent {
    uint64_t start, len;            /* bytes */
};

struct iso_inode {
    uint32_t nextents;
    struct iso_extent *extents;
    char *link;
};

/* The attributes of a directory record, with its Rock Ridge entries. */
struct iso_attr {
    char name[NAME_MAX + 1];
    size_t name_len;
    bool dot, dotdot;               /* the records "." and ".." */
    bool dir, multi, associated;
    uint64_t extent, size;
    bool has_px;
    uint32_t mode, nlink, uid, gid;
    uint64_t rdev;
    int64_t mtime;                  /* nanoseconds */
    bool relocated;                 /* RE: shown at its CL record instead */
    uint64_t child_link;            /* CL: byte offset of the relocated directory, 0 without */
    char *link;                     /* SL target when asked for, kmalloc'd */
    size_t link_len;
};

static const struct inode_ops iso_dir_ops;
static const struct inode_ops iso_link_ops;
static const struct file_ops iso_file_fops;
static const struct file_ops iso_dir_fops;

static struct iso_sb *iso_of(struct inode *ino)
{
    return ino->sb->priv;
}

/* Read n bytes at byte offset off of the volume through the block cache. */
static int iso_read(struct blockdev *dev, uint64_t off, void *buf, size_t n)
{
    uint8_t *out = buf;
    while (n) {
        uint64_t block = off / BCACHE_BLOCK_SIZE;
        size_t boff = (size_t)(off % BCACHE_BLOCK_SIZE);
        size_t chunk = MIN(n, (size_t)BCACHE_BLOCK_SIZE - boff);
        struct buf *b = bread(dev, block);
        if (!b)
            return -EIO;
        memcpy(out, b->data + boff, chunk);
        brelse(b);
        out += chunk;
        off += chunk;
        n -= chunk;
    }
    return 0;
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int digits(const uint8_t *p, int n)
{
    int v = 0;
    for (int i = 0; i < n; i++)
        v = v * 10 + (p[i] >= '0' && p[i] <= '9' ? p[i] - '0' : 0);
    return v;
}

/* Seconds since the epoch from a 7 byte date (ECMA-119 9.1.5): years
 * since 1900, month, day, hour, minute, second and the offset from UTC in
 * intervals of 15 minutes. */
static int64_t date7(const uint8_t *d)
{
    if (d[1] < 1 || d[1] > 12 || d[2] < 1)
        return 0;
    int64_t t = date_days_from_civil(1900 + d[0], d[1], d[2]) * 86400 + d[3] * 3600 + d[4] * 60 + d[5];
    return t - (int64_t)(int8_t)d[6] * 15 * 60;
}

/* Seconds since the epoch from a 17 byte date (ECMA-119 8.4.26.1):
 * "YYYYMMDDHHMMSScc" and the offset from UTC. */
static int64_t date17(const uint8_t *d)
{
    int y = digits(d, 4), mo = digits(d + 4, 2), day = digits(d + 6, 2);
    if (y == 0 || mo < 1 || mo > 12 || day < 1)
        return 0;
    int64_t t = date_days_from_civil(y, mo, day) * 86400 + digits(d + 8, 2) * 3600 + digits(d + 10, 2) * 60 +
                digits(d + 12, 2);
    return t - (int64_t)(int8_t)d[16] * 15 * 60;
}

/* Append to a symbolic link target, at most VFS_SYMLINK_MAX bytes. */
static void link_append(struct iso_attr *a, const char *s, size_t n)
{
    if (!a->link || a->link_len + n > VFS_SYMLINK_MAX)
        return;
    memcpy(a->link + a->link_len, s, n);
    a->link_len += n;
}

/* One SL entry: its components, joined with slashes. A component with
 * the continue flag runs on into the next one without a slash. *cont
 * carries that state from one entry to the next. */
static void parse_sl(struct iso_attr *a, const uint8_t *e, unsigned len, bool *cont)
{
    for (unsigned off = 5; off + 2 <= len;) {
        uint8_t cflags = e[off], clen = e[off + 1];
        if (off + 2u + clen > len)
            break;
        if (a->link_len && !*cont && a->link[a->link_len - 1] != '/')
            link_append(a, "/", 1);
        if (cflags & 0x08)
            link_append(a, "/", 1);
        else if (cflags & 0x02)
            link_append(a, ".", 1);
        else if (cflags & 0x04)
            link_append(a, "..", 2);
        else
            link_append(a, (const char *)e + off + 2, clen);
        *cont = cflags & 0x01;
        off += 2u + clen;
    }
}

/* Parse the System Use entries of a record and of its continuation areas
 * (SUSP 1.12, RRIP 1.12). A malformed entry ends the area. */
static void parse_susp(struct iso_sb *m, struct iso_attr *a, const uint8_t *area, unsigned len, bool want_link)
{
    uint8_t *ce_buf = NULL;
    bool nm_seen = false, sl_cont = false;
    for (unsigned areas = 0; areas <= ISO_MAX_CE; areas++) {
        uint64_t ce_off = 0;
        uint32_t ce_len = 0;
        for (unsigned off = 0; off + 4 <= len;) {
            const uint8_t *e = area + off;
            unsigned elen = e[2];
            if (elen < 4 || off + elen > len)
                break;
            uint16_t sig = (uint16_t)(e[0] << 8 | e[1]);
            switch (sig) {
            case ('C' << 8) | 'E':
                if (elen >= 28) {
                    ce_off = (uint64_t)le32(e + 4) * m->block + le32(e + 12);
                    ce_len = le32(e + 20);
                }
                break;
            case ('S' << 8) | 'T':
                off = len;
                continue;
            case ('P' << 8) | 'X':
                if (elen >= 36) {
                    a->has_px = true;
                    a->mode = le32(e + 4);
                    a->nlink = le32(e + 12);
                    a->uid = le32(e + 20);
                    a->gid = le32(e + 28);
                }
                break;
            case ('P' << 8) | 'N':
                if (elen >= 20)
                    a->rdev = (uint64_t)le32(e + 4) << 32 | le32(e + 12);
                break;
            case ('N' << 8) | 'M':
                if (elen >= 5) {
                    if (!nm_seen)
                        a->name_len = 0;
                    nm_seen = true;
                    if (e[4] & 0x02) {
                        a->name[0] = '.';
                        a->name_len = 1;
                    } else if (e[4] & 0x04) {
                        a->name[0] = a->name[1] = '.';
                        a->name_len = 2;
                    } else {
                        size_t n = MIN((size_t)(elen - 5), NAME_MAX - a->name_len);
                        memcpy(a->name + a->name_len, e + 5, n);
                        a->name_len += n;
                    }
                }
                break;
            case ('S' << 8) | 'L':
                if (want_link && elen >= 5)
                    parse_sl(a, e, elen, &sl_cont);
                break;
            case ('T' << 8) | 'F':
                if (elen >= 5) {
                    uint8_t flags = e[4];
                    unsigned size = (flags & 0x80) ? 17 : 7, p = 5;
                    for (unsigned bit = 0; bit < 7; bit++) {
                        if (!(flags & (1u << bit)))
                            continue;
                        if (p + size > elen)
                            break;
                        if (bit == 1)
                            a->mtime = (size == 17 ? date17(e + p) : date7(e + p)) * 1000000000;
                        p += size;
                    }
                }
                break;
            case ('R' << 8) | 'E':
                a->relocated = true;
                break;
            case ('C' << 8) | 'L':
                if (elen >= 12)
                    a->child_link = (uint64_t)le32(e + 4) * m->block;
                break;
            default:
                break;
            }
            off += elen;
        }
        if (!ce_len)
            break;
        /* The next continuation area replaces the current one. */
        ce_len = MIN(ce_len, (uint32_t)ISO_MAX_CE_LEN);
        kfree(ce_buf);
        ce_buf = kmalloc(ce_len);
        if (!ce_buf || iso_read(m->dev, ce_off, ce_buf, ce_len) < 0)
            break;
        area = ce_buf;
        len = ce_len;
    }
    kfree(ce_buf);
    if (a->link)
        a->link[a->link_len] = '\0';
}

/* The attributes of the directory record d. root_dot marks the "." record
 * of the root directory, whose system use area contains SP and is not
 * skipped. want_link collects the target of a symbolic link. */
static int parse_record(struct iso_sb *m, const struct iso_dirent *d, bool root_dot, bool want_link,
                        struct iso_attr *a)
{
    memset(a, 0, sizeof *a);
    a->extent = (uint64_t)d->extent.le * m->block;
    a->size = d->size.le;
    a->dir = d->flags & FLAG_DIRECTORY;
    a->multi = d->flags & FLAG_MULTI_EXTENT;
    a->associated = d->flags & FLAG_ASSOCIATED;
    a->mtime = date7(d->date) * 1000000000;
    a->dot = d->name_len == 1 && d->name[0] == 0;
    a->dotdot = d->name_len == 1 && d->name[0] == 1;

    /* The ISO name without the version suffix and without a final dot, in
     * lowercase (load_name of Limine). */
    size_t n = 0;
    for (size_t j = 0; j < d->name_len && n < NAME_MAX; j++) {
        char c = d->name[j];
        if (c == ';')
            break;
        a->name[n++] = c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
    }
    if (n && a->name[n - 1] == '.')
        n--;
    a->name_len = n;

    if (m->rock_ridge) {
        unsigned start = DIRENT_HEADER + d->name_len + ((d->name_len & 1) ? 0 : 1);
        if (!root_dot)
            start += m->skip;
        if (want_link)
            a->link = kmalloc(VFS_SYMLINK_MAX + 1);
        if (start < d->length)
            parse_susp(m, a, (const uint8_t *)d + start, d->length - start, want_link);
    }
    if (!a->has_px) {
        a->mode = a->dir ? S_IFDIR | 0555 : S_IFREG | 0444;
        a->nlink = a->dir ? 2 : 1;
    }
    /* A relocated directory appears at its CL record, which is a file
     * record with the attributes of the directory. */
    if (a->child_link) {
        a->dir = true;
        a->mode = (a->mode & 07777) | S_IFDIR;
    }
    a->name[a->name_len] = '\0';
    return 0;
}

/* Read the record at byte position pos into rec, which has room for 255
 * bytes. Records do not cross the end of a logical block. */
static int read_record(struct iso_sb *m, uint64_t pos, uint8_t *rec)
{
    int r = iso_read(m->dev, pos, rec, 1);
    if (r < 0)
        return r;
    if (rec[0] < DIRENT_HEADER || pos % m->block + rec[0] > m->block)
        return -EIO;
    r = iso_read(m->dev, pos, rec, rec[0]);
    if (r < 0)
        return r;
    const struct iso_dirent *d = (const void *)rec;
    return DIRENT_HEADER + d->name_len <= d->length ? 0 : -EIO;
}

/* Iteration over the records of a directory extent, one logical block at
 * a time (iso9660_next_entry of Limine). A record of length 0 pads the
 * rest of its block. */
struct dir_iter {
    struct iso_sb *m;
    uint64_t start, size, pos;
    uint64_t loaded;                /* offset of the block in buf, or UINT64_MAX */
    uint8_t *buf;
};

static int iter_init(struct dir_iter *it, struct iso_sb *m, uint64_t start, uint64_t size, uint64_t pos)
{
    it->m = m;
    it->start = start;
    it->size = size;
    it->pos = pos;
    it->loaded = UINT64_MAX;
    it->buf = kmalloc(m->block);
    return it->buf ? 0 : -ENOMEM;
}

static void iter_done(struct dir_iter *it)
{
    kfree(it->buf);
}

/* The next record, or NULL at the end; *rec_pos receives its byte
 * position on the volume. *err is set on a read error or a malformed
 * record. */
static const struct iso_dirent *iter_next(struct dir_iter *it, uint64_t *rec_pos, int *err)
{
    uint32_t bs = it->m->block;
    *err = 0;
    while (it->pos < it->size) {
        uint64_t block_off = it->pos / bs * bs;
        if (it->loaded != block_off) {
            if ((*err = iso_read(it->m->dev, it->start + block_off, it->buf, bs)) < 0)
                return NULL;
            it->loaded = block_off;
        }
        uint32_t in = (uint32_t)(it->pos - block_off);
        uint8_t len = it->buf[in];
        if (len == 0) {
            it->pos = block_off + bs;
            continue;
        }
        const struct iso_dirent *d = (const void *)(it->buf + in);
        if (len < DIRENT_HEADER || in + len > bs || DIRENT_HEADER + d->name_len > len) {
            *err = -EIO;
            return NULL;
        }
        *rec_pos = it->start + it->pos;
        it->pos += len;
        return d;
    }
    return NULL;
}

/* The inode number of a record: the extent of a directory, the record
 * position of everything else. A directory reached through CL has the
 * extent of the relocated directory. */
static uint64_t record_ino(const struct iso_attr *a, uint64_t rec_pos)
{
    if (a->child_link)
        return a->child_link;
    return a->dir ? a->extent : rec_pos;
}

/* The records that a directory lists: not ".", "..", associated files,
 * relocated directories or the further extents of a multi-extent file. */
static bool record_listed(const struct iso_attr *a, bool after_multi)
{
    return !a->dot && !a->dotdot && !a->associated && !a->relocated && !after_multi;
}

static int iso_lookup(struct inode *dir, const char *name, size_t len, struct inode **out)
{
    struct iso_sb *m = iso_of(dir);
    struct iso_inode *info = dir->priv;
    struct dir_iter it;
    int r = iter_init(&it, m, info->extents[0].start, info->extents[0].len, 0);
    if (r < 0)
        return r;
    struct iso_attr *a = kmalloc(sizeof *a);
    r = a ? -ENOENT : -ENOMEM;
    bool after_multi = false;
    uint64_t pos;
    const struct iso_dirent *d;
    int err = 0;
    while (a && (d = iter_next(&it, &pos, &err))) {
        parse_record(m, d, false, false, a);
        bool listed = record_listed(a, after_multi);
        after_multi = a->multi;
        if (listed && a->name_len == len && memcmp(a->name, name, len) == 0) {
            *out = inode_get(dir->sb, record_ino(a, pos));
            r = *out ? 0 : -ENOMEM;
            break;
        }
    }
    if (err)
        r = err;
    kfree(a);
    iter_done(&it);
    return r;
}

static uint8_t dirent_type(uint32_t mode)
{
    switch (mode & S_IFMT) {
    case S_IFDIR:
        return DT_DIR;
    case S_IFLNK:
        return DT_LNK;
    case S_IFCHR:
        return DT_CHR;
    case S_IFBLK:
        return DT_BLK;
    case S_IFIFO:
        return DT_FIFO;
    default:
        return DT_REG;
    }
}

/* f->pos is the byte offset of the next record in the directory extent,
 * which makes a listing linear in the size of the directory. A listing
 * stops only after the last extent of a multi-extent file, so f->pos never
 * points between the extents of one file. */
static long iso_getdents(struct file *f, struct dirent *buf, size_t count)
{
    struct iso_sb *m = iso_of(f->inode);
    struct iso_inode *info = f->inode->priv;
    size_t max = count / sizeof(struct dirent), filled = 0;
    if (!max)
        return -EINVAL;
    struct dir_iter it;
    int r = iter_init(&it, m, info->extents[0].start, info->extents[0].len, f->pos);
    if (r < 0)
        return r;
    struct iso_attr *a = kmalloc(sizeof *a);
    if (!a) {
        iter_done(&it);
        return -ENOMEM;
    }
    bool after_multi = false;
    uint64_t pos;
    const struct iso_dirent *d;
    int err = 0;
    while (filled < max || after_multi) {
        if (!(d = iter_next(&it, &pos, &err)))
            break;
        parse_record(m, d, false, false, a);
        bool listed = record_listed(a, after_multi);
        after_multi = a->multi;
        if (listed) {
            if (filled == max)
                break;
            buf[filled].d_ino = record_ino(a, pos);
            buf[filled].d_type = dirent_type(a->mode);
            memcpy(buf[filled].d_name, a->name, a->name_len + 1);
            filled++;
        }
        f->pos = it.pos;
    }
    kfree(a);
    iter_done(&it);
    if (err && !filled)
        return err;
    return (long)(filled * sizeof(struct dirent));
}

static long iso_file_read(struct file *f, char *buf, size_t n, uint64_t *pos)
{
    struct inode *ino = f->inode;
    struct iso_sb *m = iso_of(ino);
    struct iso_inode *info = ino->priv;
    if (*pos >= ino->size)
        return 0;
    n = MIN(n, (size_t)(ino->size - *pos));
    size_t done = 0;
    uint64_t ext_start = 0;
    for (uint32_t i = 0; i < info->nextents && done < n; i++) {
        uint64_t ext_end = ext_start + info->extents[i].len;
        if (*pos < ext_end) {
            uint64_t in = *pos - ext_start;
            size_t chunk = (size_t)MIN((uint64_t)(n - done), info->extents[i].len - in);
            int r = iso_read(m->dev, info->extents[i].start + in, buf + done, chunk);
            if (r < 0)
                return done ? (long)done : r;
            done += chunk;
            *pos += chunk;
        }
        ext_start = ext_end;
    }
    return (long)done;
}

static int iso_readlink(struct inode *ino, char *buf, size_t size)
{
    struct iso_inode *info = ino->priv;
    if (!info->link)
        return -EINVAL;
    size_t n = MIN(size, strlen(info->link));
    memcpy(buf, info->link, n);
    return (int)n;
}

static const struct inode_ops iso_dir_ops = {
    .lookup = iso_lookup,
};
static const struct inode_ops iso_link_ops = {
    .readlink = iso_readlink,
};
static const struct file_ops iso_file_fops = {
    .read = iso_file_read,
};
static const struct file_ops iso_dir_fops = {
    .getdents = iso_getdents,
};

static void iso_free_info(struct inode *ino)
{
    struct iso_inode *info = ino->priv;
    if (!info)
        return;
    kfree(info->extents);
    kfree(info->link);
    kfree(info);
    ino->priv = NULL;
}

/* The extents of the file whose first record is at pos: that record and
 * the records after it while each one has the multi-extent flag
 * (iso9660_open of Limine). A record of length 0 pads the rest of its
 * logical block, and the next record starts the next block. */
static int collect_extents(struct iso_sb *m, uint64_t pos, const struct iso_attr *first, struct iso_inode *info,
                           uint64_t *size)
{
    uint32_t cap = 4, n = 0;
    info->extents = kmalloc(cap * sizeof *info->extents);
    if (!info->extents)
        return -ENOMEM;
    info->extents[n++] = (struct iso_extent){ first->extent, first->size };
    *size = first->size;
    uint8_t rec[256];
    const struct iso_dirent *d = (const void *)rec;
    int r = read_record(m, pos, rec);
    bool more = first->multi;
    while (r == 0 && more && n < ISO_MAX_EXTENTS) {
        pos += rec[0];
        if (pos % m->block) {
            uint8_t len;
            if ((r = iso_read(m->dev, pos, &len, 1)) < 0)
                break;
            if (len == 0)
                pos = (pos / m->block + 1) * m->block;
        }
        if ((r = read_record(m, pos, rec)) < 0)
            break;
        if (n == cap) {
            struct iso_extent *grown = kmalloc(2 * cap * sizeof *grown);
            if (!grown) {
                r = -ENOMEM;
                break;
            }
            memcpy(grown, info->extents, cap * sizeof *grown);
            kfree(info->extents);
            info->extents = grown;
            cap *= 2;
        }
        info->extents[n++] = (struct iso_extent){ (uint64_t)d->extent.le * m->block, d->size.le };
        *size += d->size.le;
        more = d->flags & FLAG_MULTI_EXTENT;
    }
    info->nextents = n;
    return r;
}

/* An inode from its number: the extent of a directory, whose first
 * record is ".", or the position of the record of anything else. */
static int iso_read_inode(struct superblock *sb, uint64_t ino, struct inode *i)
{
    struct iso_sb *m = sb->priv;
    uint8_t rec[256];
    int r = read_record(m, ino, rec);
    if (r < 0)
        return r;
    const struct iso_dirent *d = (const void *)rec;
    struct iso_attr *a = kmalloc(sizeof *a);
    struct iso_inode *info = kzalloc(sizeof *info);
    if (!a || !info) {
        kfree(a);
        kfree(info);
        return -ENOMEM;
    }
    parse_record(m, d, ino == m->root_extent, true, a);
    i->priv = info;
    i->mode = a->mode;
    i->nlink = a->nlink ? a->nlink : 1;
    i->uid = a->uid;
    i->gid = a->gid;
    i->rdev = a->rdev;
    i->mtime = a->mtime;
    if (a->dot) {
        /* A directory: its "." record gives its extent and size. */
        if ((i->mode & S_IFMT) != S_IFDIR)
            i->mode = (i->mode & 07777) | S_IFDIR;
        info->extents = kmalloc(sizeof *info->extents);
        if (!info->extents) {
            r = -ENOMEM;
            goto out;
        }
        info->extents[0] = (struct iso_extent){ a->extent, a->size };
        info->nextents = 1;
        i->size = a->size;
        i->ops = &iso_dir_ops;
        i->fops = &iso_dir_fops;
    } else if (S_ISLNK(i->mode)) {
        info->link = a->link;
        a->link = NULL;
        i->size = info->link ? strlen(info->link) : 0;
        i->ops = &iso_link_ops;
    } else if (S_ISREG(i->mode)) {
        uint64_t size = 0;
        r = collect_extents(m, ino, a, info, &size);
        i->size = size;
        i->fops = &iso_file_fops;
    }
out:
    kfree(a->link);
    kfree(a);
    if (r < 0)
        iso_free_info(i);
    return r;
}

static void iso_put_inode(struct inode *ino)
{
    iso_free_info(ino);
}

static int iso_statfs(struct superblock *sb, struct fs_space *space)
{
    struct iso_sb *m = sb->priv;
    space->blocks = m->space_blocks;
    space->free_blocks = 0;
    space->block_size = m->block;
    return 0;
}

static void iso_unmount(struct superblock *sb)
{
    kfree(sb->priv);
}

static const struct sb_ops iso_sb_ops = {
    .read_inode = iso_read_inode,
    .put_inode = iso_put_inode,
    .free_inode = iso_put_inode,
    .statfs = iso_statfs,
    .unmount = iso_unmount,
};

/* Find the primary volume descriptor (iso9660_find_PVD of Limine). */
static int find_pvd(struct blockdev *dev, uint8_t *vd)
{
    for (unsigned i = 0; i < ISO_MAX_VDS; i++) {
        uint64_t off = (uint64_t)(ISO_FIRST_VD + i) * ISO_VD_SIZE;
        if (off + ISO_VD_SIZE > blockdev_size(dev))
            return -EINVAL;
        int r = iso_read(dev, off, vd, ISO_VD_SIZE);
        if (r < 0)
            return r;
        if (memcmp(vd + 1, "CD001", 5) != 0)
            return -EINVAL;
        if (vd[0] == ISO_VD_PRIMARY)
            return 0;
        if (vd[0] == ISO_VD_TERMINATOR)
            return -EINVAL;
    }
    return -EINVAL;
}

/* The volume identifier without its trailing blanks. */
static void volume_id(const uint8_t *vd, char *out)
{
    memcpy(out, vd + PVD_VOLUME_ID, 32);
    out[32] = '\0';
    for (int n = 32; n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\0'); n--)
        out[n - 1] = '\0';
}

/* Rock Ridge is in use when the "." record of the root directory starts
 * its system use area with SP (SUSP 5.3) and a following ER or any RRIP
 * entry names it. */
static void detect_rock_ridge(struct iso_sb *m)
{
    uint8_t rec[256];
    if (read_record(m, m->root_extent, rec) < 0)
        return;
    const struct iso_dirent *d = (const void *)rec;
    unsigned start = DIRENT_HEADER + d->name_len + ((d->name_len & 1) ? 0 : 1);
    if (start + 7 > d->length)
        return;
    const uint8_t *sp = rec + start;
    if (sp[0] != 'S' || sp[1] != 'P' || sp[2] < 7 || sp[4] != 0xbe || sp[5] != 0xef)
        return;
    m->rock_ridge = true;
    m->skip = sp[6];
}

static int iso_mount(const struct fs_type *type, const char *source, const char *options, struct superblock **out)
{
    if (options[0])
        return -EINVAL;
    struct blockdev *dev = blockdev_find(source);
    if (!dev)
        return -ENODEV;
    uint8_t *vd = kmalloc(ISO_VD_SIZE);
    struct iso_sb *m = kzalloc(sizeof *m);
    if (!vd || !m) {
        kfree(vd);
        kfree(m);
        return -ENOMEM;
    }
    int r = find_pvd(dev, vd);
    if (r < 0)
        goto fail;
    m->dev = dev;
    m->block = vd[PVD_BLOCK_SIZE] | vd[PVD_BLOCK_SIZE + 1] << 8;
    m->space_blocks = le32(vd + PVD_SPACE_SIZE);
    volume_id(vd, m->volume_id);
    const struct iso_dirent *root = (const void *)(vd + PVD_ROOT);
    if (m->block != 2048 || root->length < DIRENT_HEADER) {
        klog_warn("%s: logical blocks of %u bytes not supported", source, m->block);
        r = -EINVAL;
        goto fail;
    }
    m->root_extent = (uint64_t)root->extent.le * m->block;
    detect_rock_ridge(m);
    struct superblock *sb = sb_alloc(type, &iso_sb_ops);
    if (!sb) {
        r = -ENOMEM;
        goto fail;
    }
    sb->priv = m;
    sb->root_ino = m->root_extent;
    kfree(vd);
    klog_info("%s: volume %s, %lu blocks, %s", source, m->volume_id, m->space_blocks,
              m->rock_ridge ? "Rock Ridge" : "no Rock Ridge");
    *out = sb;
    return 0;
fail:
    kfree(vd);
    kfree(m);
    return r;
}

bool iso9660_has_label(struct blockdev *dev, const char *label)
{
    uint8_t *vd = kmalloc(ISO_VD_SIZE);
    if (!vd)
        return false;
    char id[33] = "";
    bool found = find_pvd(dev, vd) == 0;
    if (found)
        volume_id(vd, id);
    kfree(vd);
    return found && strcmp(id, label) == 0;
}

static struct fs_type iso_fs_type = {
    .name = "iso9660",
    .mount = iso_mount,
};

void iso9660_init(void)
{
    vfs_register_fs(&iso_fs_type);
}
