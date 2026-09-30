#define KLOG_SUBSYS "fat"
/* Directories: entry iteration with long names, lookup, create, mkdir,
 * unlink, rmdir, rename and getdents. The VFS holds the directory mutex
 * around every operation here. */
#include "fat.h"
#include <mm/slab.h>
#include <lib/string.h>
#include <kassert.h>
#include <klog.h>
#include <errno.h>

#define ENTRY_SIZE 32

/* ---- entry addressing ---- */

static uint32_t entries_per_cluster(struct fat_sb *m)
{
    return m->cluster_bytes / ENTRY_SIZE;
}

/* Byte offset of entry index of dir. Returns 1 when it exists, 0 when it
 * lies beyond the end (and alloc is not set), or -errno. */
static int entry_off(struct inode *dir, uint32_t index, bool alloc, uint64_t *off)
{
    struct fat_sb *m = fat_of(dir);
    struct fat_inode_info *info = dir->priv;
    if (dir->ino == FAT_ROOT_INO && m->type != 32) {
        if (index >= m->root_entries)
            return alloc ? -ENOSPC : 0;
        *off = (uint64_t)m->root_start * FAT_SECTOR_SIZE + (uint64_t)index * ENTRY_SIZE;
        return 1;
    }
    uint32_t per = entries_per_cluster(m);
    uint32_t c = fat_cluster_at(dir, index / per, alloc);
    if (!c)
        return alloc ? -ENOSPC : 0;
    *off = fat_cluster_off(m, c) + (uint64_t)(index % per) * ENTRY_SIZE;
    (void)info;
    return 1;
}

static int read_raw(struct inode *dir, uint32_t index, struct fat_dirent *raw, uint64_t *off)
{
    int r = entry_off(dir, index, false, off);
    if (r <= 0)
        return r;
    r = fat_read(fat_of(dir), *off, raw, ENTRY_SIZE);
    return r < 0 ? r : 1;
}

/* ---- names ---- */

static int to_lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static int to_upper(int c)
{
    return c >= 'a' && c <= 'z' ? c - 32 : c;
}

static bool names_equal(const char *a, size_t alen, const char *b)
{
    size_t i = 0;
    for (; i < alen && b[i]; i++)
        if (to_lower((unsigned char)a[i]) != to_lower((unsigned char)b[i]))
            return false;
    return i == alen && b[i] == '\0';
}

static void utf8_from_utf16(const uint16_t *s, size_t n, char *out, size_t max)
{
    size_t o = 0;
    for (size_t i = 0; i < n && o + 4 < max; i++) {
        uint32_t cp = s[i];
        if (cp < 0x80) {
            out[o++] = (char)cp;
        } else if (cp < 0x800) {
            out[o++] = (char)(0xc0 | cp >> 6);
            out[o++] = (char)(0x80 | (cp & 0x3f));
        } else {
            out[o++] = (char)(0xe0 | cp >> 12);
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3f));
            out[o++] = (char)(0x80 | (cp & 0x3f));
        }
    }
    out[o] = '\0';
}

static size_t utf16_from_utf8(const char *s, size_t len, uint16_t *out, size_t max)
{
    size_t n = 0, i = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (i < len && n < max) {
        uint32_t cp;
        if (p[i] < 0x80) {
            cp = p[i++];
        } else if ((p[i] & 0xe0) == 0xc0 && i + 1 < len) {
            cp = ((p[i] & 0x1f) << 6) | (p[i + 1] & 0x3f);
            i += 2;
        } else if ((p[i] & 0xf0) == 0xe0 && i + 2 < len) {
            cp = ((p[i] & 0x0f) << 12) | ((p[i + 1] & 0x3f) << 6) | (p[i + 2] & 0x3f);
            i += 3;
        } else {
            cp = '_';
            i++;
        }
        out[n++] = (uint16_t)cp;
    }
    return n;
}

static void short_to_display(const uint8_t raw[11], char *out)
{
    size_t o = 0;
    for (int i = 0; i < 8 && raw[i] != ' '; i++)
        out[o++] = (char)to_lower(raw[i]);
    if (raw[8] != ' ') {
        out[o++] = '.';
        for (int i = 8; i < 11 && raw[i] != ' '; i++)
            out[o++] = (char)to_lower(raw[i]);
    }
    out[o] = '\0';
    if ((uint8_t)out[0] == FAT_NAME_E5)
        out[0] = (char)0xe5;
}

static bool short_char_ok(int c)
{
    if (c < 0x20 || c > 0x7e)
        return false;
    return strchr("\"*+,/:;<=>?[\\]| .", c) == NULL;
}

/* A name that is already a valid upper case 8.3 name needs no long
 * name entries. */
static bool name_is_plain_83(const char *name, size_t len, uint8_t out[11])
{
    size_t dot = len;
    for (size_t i = 0; i < len; i++)
        if (name[i] == '.')
            dot = i;
    size_t base = dot, ext = dot < len ? len - dot - 1 : 0;
    if (base == 0 || base > 8 || ext > 3 || (dot < len && ext == 0))
        return false;
    memset(out, ' ', 11);
    for (size_t i = 0; i < base; i++) {
        int c = (unsigned char)name[i];
        if (!short_char_ok(c) || (c >= 'a' && c <= 'z'))
            return false;
        out[i] = (uint8_t)c;
    }
    for (size_t i = 0; i < ext; i++) {
        int c = (unsigned char)name[dot + 1 + i];
        if (!short_char_ok(c) || (c >= 'a' && c <= 'z'))
            return false;
        out[8 + i] = (uint8_t)c;
    }
    return true;
}

static bool short_name_used(struct inode *dir, const uint8_t name[11])
{
    struct fat_entry e;
    uint32_t index = 0;
    while (fat_dir_next(dir, &index, &e) > 0)
        if (memcmp(e.raw.name, name, 11) == 0)
            return true;
    return false;
}

/* Derive an unused short name: up to six upper case characters of the
 * base, ~N, and up to three of the extension. */
static int make_short_name(struct inode *dir, const char *name, size_t len, uint8_t out[11])
{
    if (name_is_plain_83(name, len, out))
        return 0;
    size_t dot = len;
    for (size_t i = 1; i < len; i++)
        if (name[i] == '.')
            dot = i;
    char base[8], ext[3];
    size_t bl = 0, el = 0;
    for (size_t i = 0; i < dot && bl < 6; i++) {
        int c = (unsigned char)name[i];
        if (c == '.' || c == ' ')
            continue;
        base[bl++] = short_char_ok(c) ? (char)to_upper(c) : '_';
    }
    for (size_t i = dot + 1; i < len && el < 3; i++) {
        int c = (unsigned char)name[i];
        if (c != ' ')
            ext[el++] = short_char_ok(c) ? (char)to_upper(c) : '_';
    }
    if (bl == 0)
        base[bl++] = '_';
    for (int n = 1; n < 100000; n++) {
        char tail[8];
        size_t tl = 0;
        tail[tl++] = '~';
        char digits[8];
        size_t dl = 0;
        for (int v = n; v; v /= 10)
            digits[dl++] = (char)('0' + v % 10);
        while (dl)
            tail[tl++] = digits[--dl];
        size_t keep = MIN(8 - tl, bl);
        memset(out, ' ', 11);
        memcpy(out, base, keep);
        memcpy(out + keep, tail, tl);
        memcpy(out + 8, ext, el);
        if (!short_name_used(dir, out))
            return 0;
    }
    return -EEXIST;
}

/* ---- iteration ---- */

int fat_dir_next(struct inode *dir, uint32_t *index, struct fat_entry *e)
{
    struct fat_sb *m = fat_of(dir);
    uint16_t lfn[FAT_LFN_ENTRIES_MAX * FAT_LFN_CHARS];
    uint32_t pieces = 0;
    uint8_t lfn_sum = 0;
    size_t lfn_len = 0;
    for (;;) {
        uint64_t off;
        int r = read_raw(dir, *index, &e->raw, &off);
        if (r <= 0)
            return r;
        (*index)++;
        uint8_t first = e->raw.name[0];
        if (first == FAT_NAME_FREE)
            return 0;
        if (first == FAT_NAME_DELETED) {
            pieces = 0;
            continue;
        }
        if (e->raw.attr == FAT_ATTR_LFN) {
            struct fat_lfn *l = (struct fat_lfn *)&e->raw;
            uint32_t seq = l->sequence & 0x3f;
            if (seq == 0 || seq > FAT_LFN_ENTRIES_MAX) {
                pieces = 0;
                continue;
            }
            if (l->sequence & FAT_LFN_LAST) {
                pieces = 0;
                lfn_len = seq * FAT_LFN_CHARS;
                lfn_sum = l->checksum;
            } else if (l->checksum != lfn_sum) {
                pieces = 0;
                continue;
            }
            uint16_t chars[FAT_LFN_CHARS];
            memcpy(chars, l->name1, 10);
            memcpy(chars + 5, l->name2, 12);
            memcpy(chars + 11, l->name3, 4);
            memcpy(lfn + (seq - 1) * FAT_LFN_CHARS, chars, sizeof chars);
            pieces++;
            continue;
        }
        if (e->raw.attr & FAT_ATTR_VOLUME) {
            pieces = 0;
            continue;
        }
        if (pieces && fat_short_checksum(e->raw.name) == lfn_sum) {
            size_t n = 0;
            while (n < lfn_len && lfn[n] != 0 && lfn[n] != 0xffff)
                n++;
            utf8_from_utf16(lfn, n, e->name, sizeof e->name);
            e->lfn_pieces = pieces;
        } else {
            short_to_display(e->raw.name, e->name);
            e->lfn_pieces = 0;
        }
        e->off = off;
        e->attr = e->raw.attr;
        e->size = e->raw.size;
        e->first_cluster = e->raw.cluster_lo | (m->type == 32 ? (uint32_t)e->raw.cluster_hi << 16 : 0);
        return 1;
    }
}

int fat_dir_find(struct inode *dir, const char *name, size_t len, struct fat_entry *e)
{
    uint32_t index = 0;
    int r;
    while ((r = fat_dir_next(dir, &index, e)) > 0)
        if (names_equal(name, len, e->name))
            return 1;
    return r < 0 ? r : 0;
}

/* ---- writing entries ---- */

static bool name_valid(const char *name, size_t len)
{
    if (len == 0 || len > FAT_LFN_MAX)
        return false;
    if ((len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.'))
        return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || strchr("\"*/:<>?\\|", c))
            return false;
    }
    return name[len - 1] != ' ' && name[len - 1] != '.';
}

/* Write a short entry (with long name pieces) for name into dir. raw
 * supplies attributes, cluster, size and times. Returns the byte offset
 * of the short entry through *out_off. */
static int add_entry(struct inode *dir, const char *name, size_t len, const struct fat_dirent *raw,
                     uint64_t *out_off)
{
    struct fat_sb *m = fat_of(dir);
    if (!name_valid(name, len))
        return len > FAT_LFN_MAX ? -ENAMETOOLONG : -EINVAL;
    uint8_t shortname[11];
    int r = make_short_name(dir, name, len, shortname);
    if (r < 0)
        return r;
    uint16_t lfn[FAT_LFN_MAX + 1];
    size_t ulen = utf16_from_utf8(name, len, lfn, FAT_LFN_MAX);
    uint8_t plain[11];
    uint32_t pieces = name_is_plain_83(name, len, plain) ? 0 : (uint32_t)((ulen + FAT_LFN_CHARS - 1) / FAT_LFN_CHARS);

    /* First run of pieces + 1 free entries; the run may extend past the
     * end of the directory, which grows on demand. */
    uint32_t start = 0, run = 0, index = 0;
    for (;;) {
        struct fat_dirent e;
        uint64_t off;
        r = read_raw(dir, index, &e, &off);
        if (r < 0)
            return r;
        if (r == 0 || e.name[0] == FAT_NAME_FREE) {
            if (run == 0)
                start = index;
            break;
        }
        if (e.name[0] == FAT_NAME_DELETED) {
            if (run == 0)
                start = index;
            if (++run == pieces + 1)
                break;
        } else {
            run = 0;
        }
        index++;
    }
    uint8_t sum = fat_short_checksum(shortname);
    for (uint32_t p = 0; p < pieces; p++) {
        uint32_t seq = pieces - p;
        struct fat_lfn l;
        memset(&l, 0xff, sizeof l);
        l.sequence = (uint8_t)(seq | (p == 0 ? FAT_LFN_LAST : 0));
        l.attr = FAT_ATTR_LFN;
        l.type = 0;
        l.checksum = sum;
        l.cluster = 0;
        uint16_t chars[FAT_LFN_CHARS];
        for (int k = 0; k < FAT_LFN_CHARS; k++) {
            size_t idx = (seq - 1) * FAT_LFN_CHARS + (size_t)k;
            chars[k] = idx < ulen ? lfn[idx] : idx == ulen ? 0 : 0xffff;
        }
        memcpy(l.name1, chars, 10);
        memcpy(l.name2, chars + 5, 12);
        memcpy(l.name3, chars + 11, 4);
        uint64_t off;
        r = entry_off(dir, start + p, true, &off);
        if (r < 0)
            return r;
        r = fat_write(m, off, &l, ENTRY_SIZE);
        if (r < 0)
            return r;
    }
    struct fat_dirent e = *raw;
    memcpy(e.name, shortname, 11);
    uint64_t off;
    r = entry_off(dir, start + pieces, true, &off);
    if (r < 0)
        return r;
    r = fat_write(m, off, &e, ENTRY_SIZE);
    if (r < 0)
        return r;
    *out_off = off;
    return 0;
}

/* Mark the short entry at off and its long name pieces as deleted. */
static int delete_entry(struct inode *dir, const struct fat_entry *e)
{
    struct fat_sb *m = fat_of(dir);
    uint8_t mark = FAT_NAME_DELETED;
    /* The pieces precede the short entry; walk back by index. */
    uint32_t index = 0;
    struct fat_entry cur;
    uint32_t found = 0;
    while (fat_dir_next(dir, &index, &cur) > 0) {
        if (cur.off == e->off) {
            found = index;      /* index after the short entry */
            break;
        }
    }
    if (!found)
        return -ENOENT;
    for (uint32_t i = found - 1 - e->lfn_pieces; i < found; i++) {
        uint64_t off;
        int r = entry_off(dir, i, false, &off);
        if (r <= 0)
            return r < 0 ? r : -EIO;
        r = fat_write(m, off, &mark, 1);
        if (r < 0)
            return r;
    }
    return 0;
}

static struct fat_dirent fresh_entry(uint8_t attr, uint32_t cluster, uint32_t size)
{
    struct fat_dirent e;
    memset(&e, 0, sizeof e);
    e.attr = attr;
    e.cluster_lo = (uint16_t)cluster;
    e.cluster_hi = (uint16_t)(cluster >> 16);
    e.size = size;
    uint16_t date, time;
    fat_now(&date, &time);
    e.mdate = e.cdate = e.adate = date;
    e.mtime = e.ctime = time;
    return e;
}

/* ---- inode operations ---- */

static int fat_lookup(struct inode *dir, const char *name, size_t len, struct inode **out)
{
    struct fat_entry e;
    int r = fat_dir_find(dir, name, len, &e);
    if (r < 0)
        return r;
    if (r == 0)
        return -ENOENT;
    *out = inode_get(dir->sb, e.off / ENTRY_SIZE);
    return *out ? 0 : -ENOMEM;
}

static int fat_create(struct inode *dir, const char *name, size_t len, uint32_t mode, struct inode **out)
{
    struct fat_entry e;
    int r = fat_dir_find(dir, name, len, &e);
    if (r < 0)
        return r;
    if (r > 0)
        return -EEXIST;
    struct fat_dirent raw = fresh_entry(FAT_ATTR_ARCHIVE | ((mode & 0200) ? 0 : FAT_ATTR_READ_ONLY), 0, 0);
    uint64_t off;
    r = add_entry(dir, name, len, &raw, &off);
    if (r < 0)
        return r;
    *out = inode_get(dir->sb, off / ENTRY_SIZE);
    return *out ? 0 : -ENOMEM;
}

static int fat_mkdir(struct inode *dir, const char *name, size_t len)
{
    struct fat_sb *m = fat_of(dir);
    struct fat_inode_info *pinfo = dir->priv;
    struct fat_entry e;
    int r = fat_dir_find(dir, name, len, &e);
    if (r < 0)
        return r;
    if (r > 0)
        return -EEXIST;
    if (!name_valid(name, len))
        return len > FAT_LFN_MAX ? -ENAMETOOLONG : -EINVAL;
    uint32_t c = fat_alloc_cluster(m, 0);
    if (!c)
        return -ENOSPC;
    /* "." and ".." at the start of the new cluster; the root is cluster 0
     * in a ".." entry on every type. */
    uint32_t parent = dir->ino == FAT_ROOT_INO ? 0 : pinfo->first_cluster;
    struct fat_dirent dot = fresh_entry(FAT_ATTR_DIRECTORY, c, 0);
    memcpy(dot.name, ".          ", 11);
    struct fat_dirent dotdot = fresh_entry(FAT_ATTR_DIRECTORY, parent, 0);
    memcpy(dotdot.name, "..         ", 11);
    uint64_t base = fat_cluster_off(m, c);
    r = fat_write(m, base, &dot, ENTRY_SIZE);
    if (r == 0)
        r = fat_write(m, base + ENTRY_SIZE, &dotdot, ENTRY_SIZE);
    if (r == 0) {
        struct fat_dirent raw = fresh_entry(FAT_ATTR_DIRECTORY, c, 0);
        uint64_t off;
        r = add_entry(dir, name, len, &raw, &off);
    }
    if (r < 0)
        fat_free_chain(m, c);
    return r;
}

/* Detach the entry of a directory child and let its inode release the
 * clusters when the last reference goes. */
static int drop_child(struct inode *dir, const struct fat_entry *e)
{
    struct inode *child = inode_get(dir->sb, e->off / ENTRY_SIZE);
    if (!child)
        return -ENOMEM;
    int r = delete_entry(dir, e);
    if (r == 0) {
        mutex_lock(&child->lock);
        struct fat_inode_info *info = child->priv;
        info->unlinked = true;
        child->nlink = 0;
        mutex_unlock(&child->lock);
    }
    inode_put(child);
    return r;
}

static bool dir_is_empty(struct inode *sub)
{
    struct fat_entry e;
    uint32_t index = 0;
    while (fat_dir_next(sub, &index, &e) > 0)
        if (strcmp(e.name, ".") != 0 && strcmp(e.name, "..") != 0)
            return false;
    return true;
}

static int fat_unlink(struct inode *dir, const char *name, size_t len)
{
    struct fat_entry e;
    int r = fat_dir_find(dir, name, len, &e);
    if (r < 0)
        return r;
    if (r == 0)
        return -ENOENT;
    if (e.attr & FAT_ATTR_DIRECTORY)
        return -EISDIR;
    return drop_child(dir, &e);
}

static int fat_rmdir(struct inode *dir, const char *name, size_t len)
{
    struct fat_entry e;
    int r = fat_dir_find(dir, name, len, &e);
    if (r < 0)
        return r;
    if (r == 0)
        return -ENOENT;
    if (!(e.attr & FAT_ATTR_DIRECTORY))
        return -ENOTDIR;
    struct inode *sub = inode_get(dir->sb, e.off / ENTRY_SIZE);
    if (!sub)
        return -ENOMEM;
    mutex_lock(&sub->lock);
    bool empty = dir_is_empty(sub);
    mutex_unlock(&sub->lock);
    inode_put(sub);
    if (!empty)
        return -ENOTEMPTY;
    return drop_child(dir, &e);
}

/* Find the cached inode for an entry offset, referenced, or NULL. */
static struct inode *cached_inode(struct superblock *sb, uint64_t ino)
{
    struct list_head *pos;
    struct inode *found = NULL;
    spin_lock(&sb->lock);
    list_for_each(pos, &sb->inodes) {
        struct inode *i = list_entry(pos, struct inode, link);
        if (i->ino == ino) {
            i->refcount++;
            found = i;
            break;
        }
    }
    spin_unlock(&sb->lock);
    return found;
}

static int fat_rename(struct inode *olddir, const char *oldname, size_t oldlen,
                      struct inode *newdir, const char *newname, size_t newlen)
{
    struct fat_sb *m = fat_of(olddir);
    struct fat_entry src, dst;
    int r = fat_dir_find(olddir, oldname, oldlen, &src);
    if (r < 0)
        return r;
    if (r == 0)
        return -ENOENT;
    bool src_dir = src.attr & FAT_ATTR_DIRECTORY;
    r = fat_dir_find(newdir, newname, newlen, &dst);
    if (r < 0)
        return r;
    if (r > 0) {
        if (dst.off == src.off)
            return 0;
        bool dst_dir = dst.attr & FAT_ATTR_DIRECTORY;
        if (dst_dir && !src_dir)
            return -EISDIR;
        if (!dst_dir && src_dir)
            return -ENOTDIR;
        if (dst_dir) {
            struct inode *sub = inode_get(newdir->sb, dst.off / ENTRY_SIZE);
            if (!sub)
                return -ENOMEM;
            mutex_lock(&sub->lock);
            bool empty = dir_is_empty(sub);
            mutex_unlock(&sub->lock);
            inode_put(sub);
            if (!empty)
                return -ENOTEMPTY;
        }
        r = drop_child(newdir, &dst);
        if (r < 0)
            return r;
    }
    /* The new entry carries the old entry's attributes, cluster, size and
     * times; only the name changes. */
    uint64_t newoff;
    r = add_entry(newdir, newname, newlen, &src.raw, &newoff);
    if (r < 0)
        return r;
    /* add_entry may have grown newdir; the source entry does not move. */
    r = delete_entry(olddir, &src);
    if (r < 0)
        return r;
    if (src_dir && olddir != newdir && src.first_cluster) {
        struct fat_inode_info *pinfo = newdir->priv;
        uint32_t parent = newdir->ino == FAT_ROOT_INO ? 0 : pinfo->first_cluster;
        struct fat_dirent dotdot;
        uint64_t off = fat_cluster_off(m, src.first_cluster) + ENTRY_SIZE;
        if (fat_read(m, off, &dotdot, ENTRY_SIZE) == 0 && memcmp(dotdot.name, "..         ", 11) == 0) {
            dotdot.cluster_lo = (uint16_t)parent;
            dotdot.cluster_hi = (uint16_t)(parent >> 16);
            fat_write(m, off, &dotdot, ENTRY_SIZE);
        }
    }
    /* An inode cached for the old entry now belongs to the new one. */
    struct inode *ino = cached_inode(olddir->sb, src.off / ENTRY_SIZE);
    if (ino) {
        mutex_lock(&ino->lock);
        struct fat_inode_info *info = ino->priv;
        info->entry_off = newoff;
        spin_lock(&ino->sb->lock);
        ino->ino = newoff / ENTRY_SIZE;
        spin_unlock(&ino->sb->lock);
        mutex_unlock(&ino->lock);
        inode_put(ino);
    }
    return 0;
}

static int fat_truncate(struct inode *ino, uint64_t size)
{
    return fat_truncate_locked(ino, size);
}

static int fat_setmtime(struct inode *ino, int64_t mtime)
{
    return fat_inode_flush_time(ino, mtime);
}

/* FAT has no entry type for a symbolic link. */
static int fat_symlink(struct inode *dir, const char *name, size_t len, const char *target, size_t tlen)
{
    return -EPERM;
}

const struct inode_ops fat_dir_ops = {
    .lookup = fat_lookup,
    .create = fat_create,
    .mkdir = fat_mkdir,
    .unlink = fat_unlink,
    .rmdir = fat_rmdir,
    .symlink = fat_symlink,
    .rename = fat_rename,
    .truncate = fat_truncate,
    .setmtime = fat_setmtime,
};

/* getdents: positions 0 and 1 are "." and ".." for every directory (the
 * FAT root has no such entries); position p + 2 is entry index p. */
static long fat_getdents(struct file *f, struct dirent *buf, size_t count)
{
    struct inode *dir = f->inode;
    size_t max = count / sizeof(struct dirent);
    size_t filled = 0;
    mutex_lock(&dir->lock);
    while (filled < max) {
        if (f->pos < 2) {
            buf[filled].d_ino = f->pos == 0 ? dir->ino : FAT_ROOT_INO;
            buf[filled].d_type = DT_DIR;
            strlcpy(buf[filled].d_name, f->pos == 0 ? "." : "..", sizeof buf[filled].d_name);
            f->pos++;
            filled++;
            continue;
        }
        uint32_t index = (uint32_t)(f->pos - 2);
        struct fat_entry e;
        int r = fat_dir_next(dir, &index, &e);
        if (r <= 0)
            break;
        f->pos = (uint64_t)index + 2;
        if (strcmp(e.name, ".") == 0 || strcmp(e.name, "..") == 0)
            continue;
        buf[filled].d_ino = e.off / ENTRY_SIZE;
        buf[filled].d_type = (e.attr & FAT_ATTR_DIRECTORY) ? DT_DIR : DT_REG;
        strlcpy(buf[filled].d_name, e.name, sizeof buf[filled].d_name);
        filled++;
    }
    mutex_unlock(&dir->lock);
    return (long)(filled * sizeof(struct dirent));
}

const struct file_ops fat_dir_fops = {
    .getdents = fat_getdents,
};

void fat_inode_release(struct inode *ino)
{
    struct fat_inode_info *info = ino->priv;
    if (info->first_cluster)
        fat_free_chain(fat_of(ino), info->first_cluster);
    info->first_cluster = 0;
}
