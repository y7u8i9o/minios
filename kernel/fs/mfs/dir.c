#define KLOG_SUBSYS "mfs"
#include "mfs.h"
#include <lib/string.h>
#include <kassert.h>
#include <klog.h>
#include <errno.h>

/* Directory entries are fixed size slots. The caller has acquired dir->lock. */

static int entry_read(struct inode *dir, uint64_t slot, struct mfs_dirent *e)
{
    long r = mfs_read_locked(dir, (char *)e, MFS_DIRENT_SIZE, slot * MFS_DIRENT_SIZE);
    if (r < 0)
        return (int)r;
    return r == MFS_DIRENT_SIZE ? 0 : -EIO;
}

static int entry_write(struct inode *dir, uint64_t slot, const struct mfs_dirent *e)
{
    long r = mfs_write_locked(dir, (const char *)e, MFS_DIRENT_SIZE, slot * MFS_DIRENT_SIZE);
    if (r < 0)
        return (int)r;
    return r == MFS_DIRENT_SIZE ? 0 : -EIO;
}

static uint64_t nslots(struct inode *dir)
{
    return dir->size / MFS_DIRENT_SIZE;
}

/* Find name; returns the slot through *slot and the inode number, or 0. */
static uint32_t entry_find(struct inode *dir, const char *name, size_t len, uint64_t *slot)
{
    if (len > MFS_NAME_MAX)
        return 0;
    struct mfs_dirent e;
    for (uint64_t s = 0; s < nslots(dir); s++) {
        if (entry_read(dir, s, &e) < 0)
            return 0;
        if (e.ino && strlen(e.name) == len && memcmp(e.name, name, len) == 0) {
            if (slot)
                *slot = s;
            return e.ino;
        }
    }
    return 0;
}

static int entry_add(struct inode *dir, const char *name, size_t len, uint32_t ino)
{
    if (len == 0 || len > MFS_NAME_MAX)
        return -ENAMETOOLONG;
    struct mfs_dirent e;
    uint64_t s;
    for (s = 0; s < nslots(dir); s++) {
        int r = entry_read(dir, s, &e);
        if (r < 0)
            return r;
        if (!e.ino)
            break;
    }
    memset(&e, 0, sizeof e);
    e.ino = ino;
    memcpy(e.name, name, len);
    return entry_write(dir, s, &e);
}

static int entry_clear(struct inode *dir, uint64_t slot)
{
    struct mfs_dirent e;
    memset(&e, 0, sizeof e);
    return entry_write(dir, slot, &e);
}

static bool dir_is_empty(struct inode *dir)
{
    struct mfs_dirent e;
    for (uint64_t s = 0; s < nslots(dir); s++) {
        if (entry_read(dir, s, &e) < 0)
            return false;
        if (e.ino && strcmp(e.name, ".") != 0 && strcmp(e.name, "..") != 0)
            return false;
    }
    return true;
}

static int mfs_lookup(struct inode *dir, const char *name, size_t len, struct inode **out)
{
    uint32_t ino = entry_find(dir, name, len, NULL);
    if (!ino)
        return -ENOENT;
    *out = inode_get(dir->sb, ino);
    return *out ? 0 : -ENOMEM;
}

static int mfs_create(struct inode *dir, const char *name, size_t len, uint32_t mode, struct inode **out)
{
    if (entry_find(dir, name, len, NULL))
        return -EEXIST;
    struct inode *ino = mfs_inode_new(dir, mode, 1);
    if (!ino)
        return -ENOSPC;
    int r = entry_add(dir, name, len, (uint32_t)ino->ino);
    if (r < 0) {
        ino->nlink = 0;
        inode_put(ino);     /* frees the inode again */
        return r;
    }
    *out = ino;
    return 0;
}

static int mfs_mkdir(struct inode *dir, const char *name, size_t len, uint32_t mode)
{
    if (entry_find(dir, name, len, NULL))
        return -EEXIST;
    struct inode *sub = mfs_inode_new(dir, S_IFDIR | (mode & 07777), 2);
    if (!sub)
        return -ENOSPC;
    int r = entry_add(sub, ".", 1, (uint32_t)sub->ino);
    if (r == 0)
        r = entry_add(sub, "..", 2, (uint32_t)dir->ino);
    if (r == 0)
        r = entry_add(dir, name, len, (uint32_t)sub->ino);
    if (r < 0) {
        sub->nlink = 0;
        inode_put(sub);
        return r;
    }
    dir->nlink++;
    mfs_inode_flush(dir);
    inode_put(sub);
    return 0;
}

static int mfs_unlink(struct inode *dir, const char *name, size_t len)
{
    uint64_t slot;
    uint32_t num = entry_find(dir, name, len, &slot);
    if (!num)
        return -ENOENT;
    struct inode *ino = inode_get(dir->sb, num);
    if (!ino)
        return -ENOMEM;
    if (S_ISDIR(ino->mode)) {
        inode_put(ino);
        return -EISDIR;
    }
    int r = entry_clear(dir, slot);
    if (r == 0) {
        mutex_lock(&ino->lock);
        ino->nlink--;
        mfs_inode_flush(ino);
        mutex_unlock(&ino->lock);
    }
    inode_put(ino);
    return r;
}

static int mfs_rmdir(struct inode *dir, const char *name, size_t len)
{
    if ((len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.'))
        return -EINVAL;
    uint64_t slot;
    uint32_t num = entry_find(dir, name, len, &slot);
    if (!num)
        return -ENOENT;
    struct inode *ino = inode_get(dir->sb, num);
    if (!ino)
        return -ENOMEM;
    int r = 0;
    mutex_lock(&ino->lock);
    if (!S_ISDIR(ino->mode))
        r = -ENOTDIR;
    else if (!dir_is_empty(ino))
        r = -ENOTEMPTY;
    if (r == 0)
        r = entry_clear(dir, slot);
    if (r == 0) {
        ino->nlink = 0;
        mfs_inode_flush(ino);
        dir->nlink--;
        mfs_inode_flush(dir);
    }
    mutex_unlock(&ino->lock);
    inode_put(ino);
    return r;
}

static int mfs_link(struct inode *dir, const char *name, size_t len, struct inode *target)
{
    if (entry_find(dir, name, len, NULL))
        return -EEXIST;
    int r = entry_add(dir, name, len, (uint32_t)target->ino);
    if (r < 0)
        return r;
    mutex_lock(&target->lock);
    target->nlink++;
    mfs_inode_flush(target);
    mutex_unlock(&target->lock);
    return 0;
}

static int mfs_rename(struct inode *olddir, const char *oldname, size_t oldlen,
                      struct inode *newdir, const char *newname, size_t newlen)
{
    uint64_t oldslot, newslot;
    uint32_t num = entry_find(olddir, oldname, oldlen, &oldslot);
    if (!num)
        return -ENOENT;
    struct inode *src = inode_get(olddir->sb, num);
    if (!src)
        return -ENOMEM;
    bool src_dir = S_ISDIR(src->mode);
    int r = 0;

    /* Replace an existing destination. */
    uint32_t tnum = entry_find(newdir, newname, newlen, &newslot);
    if (tnum == num) {
        inode_put(src);
        return 0;
    }
    if (tnum) {
        struct inode *target = inode_get(newdir->sb, tnum);
        if (!target) {
            inode_put(src);
            return -ENOMEM;
        }
        mutex_lock(&target->lock);
        bool tdir = S_ISDIR(target->mode);
        if (tdir && !src_dir)
            r = -EISDIR;
        else if (!tdir && src_dir)
            r = -ENOTDIR;
        else if (tdir && !dir_is_empty(target))
            r = -ENOTEMPTY;
        if (r == 0) {
            target->nlink = tdir ? 0 : target->nlink - 1;
            mfs_inode_flush(target);
            if (tdir) {
                newdir->nlink--;
                mfs_inode_flush(newdir);
            }
        }
        mutex_unlock(&target->lock);
        inode_put(target);
        if (r < 0) {
            inode_put(src);
            return r;
        }
    }

    /* Point the new name at the source, then drop the old name. */
    struct mfs_dirent e;
    memset(&e, 0, sizeof e);
    e.ino = num;
    memcpy(e.name, newname, MIN(newlen, (size_t)MFS_NAME_MAX));
    if (tnum)
        r = entry_write(newdir, newslot, &e);
    else
        r = entry_add(newdir, newname, newlen, num);
    if (r == 0)
        r = entry_clear(olddir, oldslot);
    if (r == 0 && src_dir && olddir != newdir) {
        /* Re-point ".." and move the link counts of the parents. */
        mutex_lock(&src->lock);
        uint64_t slot;
        if (entry_find(src, "..", 2, &slot)) {
            memset(&e, 0, sizeof e);
            e.ino = (uint32_t)newdir->ino;
            strlcpy(e.name, "..", sizeof e.name);
            entry_write(src, slot, &e);
        }
        mutex_unlock(&src->lock);
        olddir->nlink--;
        mfs_inode_flush(olddir);
        newdir->nlink++;
        mfs_inode_flush(newdir);
    }
    inode_put(src);
    return r;
}

/* A symbolic link retains its target in its first data block, written
 * through the journal like directory contents, so the link and its target
 * are committed in one transaction. */
static int mfs_symlink(struct inode *dir, const char *name, size_t len, const char *target, size_t tlen)
{
    if (entry_find(dir, name, len, NULL))
        return -EEXIST;
    if (tlen == 0 || tlen > MFS_SYMLINK_MAX)
        return -ENAMETOOLONG;
    struct inode *ino = mfs_inode_new(dir, S_IFLNK | 0777, 1);
    if (!ino)
        return -ENOSPC;
    mutex_lock(&ino->lock);
    long w = mfs_write_locked(ino, target, tlen, 0);
    mutex_unlock(&ino->lock);
    int r = w == (long)tlen ? 0 : w < 0 ? (int)w : -ENOSPC;
    if (r == 0)
        r = entry_add(dir, name, len, (uint32_t)ino->ino);
    if (r < 0)
        ino->nlink = 0;             /* the release frees the inode and its block */
    inode_put(ino);
    return r;
}

static int mfs_readlink(struct inode *ino, char *buf, size_t size)
{
    long r = mfs_read_locked(ino, buf, MIN(size, (size_t)ino->size), 0);
    return (int)r;
}

static int mfs_truncate(struct inode *ino, uint64_t size)
{
    return mfs_truncate_locked(ino, size);
}

static int mfs_setmtime(struct inode *ino, int64_t mtime)
{
    ino->mtime = mtime;
    return mfs_inode_flush(ino);
}

static int mfs_setattr(struct inode *ino, uint32_t mode, uint32_t uid, uint32_t gid)
{
    ino->mode = (ino->mode & S_IFMT) | mode;
    ino->uid = uid;
    ino->gid = gid;
    return mfs_inode_flush(ino);
}

const struct inode_ops mfs_dir_ops = {
    .lookup = mfs_lookup,
    .create = mfs_create,
    .mkdir = mfs_mkdir,
    .unlink = mfs_unlink,
    .rmdir = mfs_rmdir,
    .link = mfs_link,
    .symlink = mfs_symlink,
    .rename = mfs_rename,
    .truncate = mfs_truncate,
    .setmtime = mfs_setmtime,
    .setattr = mfs_setattr,
};

/* A symbolic link has no directory or file operations; utimensat with
 * AT_SYMLINK_NOFOLLOW sets its time. */
const struct inode_ops mfs_link_ops = {
    .readlink = mfs_readlink,
    .setmtime = mfs_setmtime,
    .setattr = mfs_setattr,
};

static long mfs_getdents(struct file *f, struct dirent *buf, size_t count)
{
    struct inode *dir = f->inode;
    size_t max = count / sizeof(struct dirent);
    size_t filled = 0;
    struct mfs_dirent e;
    mutex_lock(&dir->lock);
    while (filled < max && f->pos < nslots(dir)) {
        if (entry_read(dir, f->pos, &e) < 0)
            break;
        f->pos++;
        if (!e.ino)
            continue;
        struct inode *child = inode_get(dir->sb, e.ino);
        buf[filled].d_ino = e.ino;
        buf[filled].d_type = child && child->nlink ? vfs_mode_to_dtype(child->mode) : DT_UNKNOWN;
        if (child && !child->nlink)
            klog_error("%s: entry '%s' of directory %lu names inode %u, which has no links (run fsck)",
                       ((struct mfs_sb *)dir->sb->priv)->dev->name, e.name, dir->ino, e.ino);
        if (child)
            inode_put(child);
        strlcpy(buf[filled].d_name, e.name, sizeof buf[filled].d_name);
        filled++;
    }
    mutex_unlock(&dir->lock);
    return (long)(filled * sizeof(struct dirent));
}

const struct file_ops mfs_dir_fops = {
    .getdents = mfs_getdents,
};
