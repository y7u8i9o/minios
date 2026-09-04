#define KLOG_SUBSYS "vfs"
#include <fs/vfs.h>
#include <mm/filemap.h>
#include <sched/thread.h>
#include <sched/proc.h>
#include <block/bcache.h>
#include <mm/slab.h>
#include <lib/string.h>
#include <lib/printf.h>
#include <kassert.h>
#include <klog.h>
#include <errno.h>

/* Registered filesystem types. Protected by fs_types_lock. */
static LIST_HEAD(fs_types);
static DEFINE_SPINLOCK(fs_types_lock);

/* Mount table, root mount first. Protected by mount_lock. */
static LIST_HEAD(mounts);
static DEFINE_SPINLOCK(mount_lock);
static struct mount *root_mount;

void vfs_init(void)
{
    klog_info("vfs ready");
}

int vfs_register_fs(struct fs_type *type)
{
    spin_lock(&fs_types_lock);
    list_add_tail(&type->link, &fs_types);
    spin_unlock(&fs_types_lock);
    klog_info("filesystem %s registered", type->name);
    return 0;
}

static struct fs_type *fs_type_find(const char *name)
{
    struct list_head *pos;
    struct fs_type *found = NULL;
    spin_lock(&fs_types_lock);
    list_for_each(pos, &fs_types) {
        struct fs_type *t = list_entry(pos, struct fs_type, link);
        if (strcmp(t->name, name) == 0) {
            found = t;
            break;
        }
    }
    spin_unlock(&fs_types_lock);
    return found;
}

/* ---- inode cache ---- */

struct superblock *sb_alloc(const struct fs_type *type, const struct sb_ops *ops)
{
    struct superblock *sb = kzalloc(sizeof *sb);
    if (!sb)
        return NULL;
    sb->type = type;
    sb->ops = ops;
    spinlock_init(&sb->lock, "superblock");
    list_init(&sb->inodes);
    return sb;
}

struct inode *inode_get(struct superblock *sb, uint64_t ino)
{
    struct list_head *pos;
    spin_lock(&sb->lock);
    list_for_each(pos, &sb->inodes) {
        struct inode *i = list_entry(pos, struct inode, link);
        if (i->ino == ino) {
            i->refcount++;
            spin_unlock(&sb->lock);
            return i;
        }
    }
    spin_unlock(&sb->lock);

    struct inode *i = kzalloc(sizeof *i);
    if (!i)
        return NULL;
    i->sb = sb;
    i->ino = ino;
    i->refcount = 1;
    mutex_init(&i->lock, "inode");
    int r = sb->ops->read_inode(sb, ino, i);
    if (r < 0) {
        kfree(i);
        return NULL;
    }
    /* Another thread may have read the same inode meanwhile. */
    spin_lock(&sb->lock);
    list_for_each(pos, &sb->inodes) {
        struct inode *other = list_entry(pos, struct inode, link);
        if (other->ino == ino) {
            other->refcount++;
            spin_unlock(&sb->lock);
            if (sb->ops->free_inode)
                sb->ops->free_inode(i);
            kfree(i);
            return other;
        }
    }
    list_add_tail(&i->link, &sb->inodes);
    spin_unlock(&sb->lock);
    return i;
}

void inode_ref(struct inode *ino)
{
    spin_lock(&ino->sb->lock);
    ino->refcount++;
    spin_unlock(&ino->sb->lock);
}

void inode_put(struct inode *ino)
{
    struct superblock *sb = ino->sb;
    spin_lock(&sb->lock);
    kassert(ino->refcount > 0);
    if (--ino->refcount > 0) {
        spin_unlock(&sb->lock);
        return;
    }
    list_del(&ino->link);
    spin_unlock(&sb->lock);
    if (sb->ops->put_inode)
        sb->ops->put_inode(ino);
    kfree(ino);
}

/* ---- paths ---- */

int vfs_canonicalize(const char *cwd, const char *path, char *out, size_t size)
{
    char tmp[VFS_PATH_MAX * 2];
    if (path[0] == '/')
        strlcpy(tmp, path, sizeof tmp);
    else
        ksnprintf(tmp, sizeof tmp, "%s/%s", cwd, path);

    size_t o = 0;
    out[o++] = '/';
    const char *p = tmp;
    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        const char *start = p;
        while (*p && *p != '/')
            p++;
        size_t n = (size_t)(p - start);
        if (n == 1 && start[0] == '.')
            continue;
        if (n == 2 && start[0] == '.' && start[1] == '.') {
            while (o > 1 && out[o - 1] != '/')
                o--;
            if (o > 1)
                o--;
            continue;
        }
        if (n > NAME_MAX)
            return -ENAMETOOLONG;
        if (o + 1 + n >= size)
            return -ENAMETOOLONG;
        if (o > 1)
            out[o++] = '/';
        memcpy(out + o, start, n);
        o += n;
    }
    out[o] = '\0';
    return 0;
}

static int canonicalize_cwd(const char *path, char *out, size_t size)
{
    struct proc *p = thread_current()->proc;
    char cwd[PROC_CWD_LEN];
    spin_lock(&p->lock);
    strlcpy(cwd, p->cwd, sizeof cwd);
    spin_unlock(&p->lock);
    return vfs_canonicalize(cwd, path, out, size);
}

/* If (sb, ino) is covered by a mount, return the root of that mount. */
static struct inode *cross_mount(struct inode *ino)
{
    struct list_head *pos;
    struct superblock *target = NULL;
    uint64_t root_ino = 0;
    spin_lock(&mount_lock);
    list_for_each(pos, &mounts) {
        struct mount *m = list_entry(pos, struct mount, link);
        if (m->parent_sb == ino->sb && m->parent_ino == ino->ino) {
            target = m->sb;
            root_ino = m->sb->root_ino;
            break;
        }
    }
    spin_unlock(&mount_lock);
    if (!target)
        return ino;
    struct inode *root = inode_get(target, root_ino);
    inode_put(ino);
    return root;
}

static struct inode *root_inode(void)
{
    kassert(root_mount != NULL);
    return inode_get(root_mount->sb, root_mount->sb->root_ino);
}

/* Look up one component under dir, taking dir->lock. */
static int lookup_child(struct inode *dir, const char *name, size_t len, struct inode **out)
{
    if (!S_ISDIR(dir->mode))
        return -ENOTDIR;
    if (!dir->ops || !dir->ops->lookup)
        return -ENOENT;
    mutex_lock(&dir->lock);
    int r = dir->ops->lookup(dir, name, len, out);
    mutex_unlock(&dir->lock);
    if (r < 0)
        return r;
    *out = cross_mount(*out);
    return *out ? 0 : -ENOMEM;
}

/* Walk canonical path; if stop_at_last, stop before the last component and
 * copy it to name. */
static int walk(const char *canon, bool stop_at_last, struct inode **out,
                char *name, size_t namesize)
{
    struct inode *cur = root_inode();
    if (!cur)
        return -ENOMEM;
    const char *p = canon + 1;
    while (*p) {
        const char *start = p;
        while (*p && *p != '/')
            p++;
        size_t n = (size_t)(p - start);
        while (*p == '/')
            p++;
        if (stop_at_last && !*p) {
            if (n >= namesize) {
                inode_put(cur);
                return -ENAMETOOLONG;
            }
            memcpy(name, start, n);
            name[n] = '\0';
            break;
        }
        struct inode *next;
        int r = lookup_child(cur, start, n, &next);
        inode_put(cur);
        if (r < 0)
            return r;
        cur = next;
    }
    *out = cur;
    return 0;
}

int vfs_lookup(const char *path, struct inode **out)
{
    char canon[VFS_PATH_MAX];
    int r = canonicalize_cwd(path, canon, sizeof canon);
    if (r < 0)
        return r;
    return walk(canon, false, out, NULL, 0);
}

int vfs_lookup_parent(const char *path, struct inode **dir, char *name, size_t namesize)
{
    char canon[VFS_PATH_MAX];
    int r = canonicalize_cwd(path, canon, sizeof canon);
    if (r < 0)
        return r;
    if (canon[1] == '\0')
        return -EINVAL;
    name[0] = '\0';
    r = walk(canon, true, dir, name, namesize);
    if (r < 0)
        return r;
    if (!S_ISDIR((*dir)->mode)) {
        inode_put(*dir);
        return -ENOTDIR;
    }
    return 0;
}

/* ---- mounts ---- */

int vfs_mount(const char *fstype, const char *source, const char *target)
{
    struct fs_type *type = fs_type_find(fstype);
    if (!type)
        return -ENODEV;
    char canon[VFS_PATH_MAX];
    int r = canonicalize_cwd(target, canon, sizeof canon);
    if (r < 0)
        return r;
    bool is_root = canon[1] == '\0';
    struct inode *point = NULL;
    if (!is_root) {
        r = vfs_lookup(canon, &point);
        if (r < 0)
            return r;
        if (!S_ISDIR(point->mode)) {
            inode_put(point);
            return -ENOTDIR;
        }
    } else if (root_mount) {
        return -EBUSY;
    }
    struct mount *m = kzalloc(sizeof *m);
    if (!m) {
        r = -ENOMEM;
        goto out;
    }
    r = type->mount(type, source, &m->sb);
    if (r < 0) {
        kfree(m);
        goto out;
    }
    strlcpy(m->path, canon, sizeof m->path);
    if (point) {
        m->parent_sb = point->sb;
        m->parent_ino = point->ino;
    }
    spin_lock(&mount_lock);
    struct list_head *pos;
    list_for_each(pos, &mounts) {
        struct mount *o = list_entry(pos, struct mount, link);
        if (strcmp(o->path, canon) == 0) {
            spin_unlock(&mount_lock);
            if (m->sb->ops->unmount)
                m->sb->ops->unmount(m->sb);
            kfree(m);
            r = -EBUSY;
            goto out;
        }
    }
    list_add_tail(&m->link, &mounts);
    if (is_root)
        root_mount = m;
    spin_unlock(&mount_lock);
    klog_info("mounted %s (%s) on %s", fstype, source, canon);
    r = 0;
out:
    if (point)
        inode_put(point);
    return r;
}

int vfs_umount(const char *target)
{
    char canon[VFS_PATH_MAX];
    int r = canonicalize_cwd(target, canon, sizeof canon);
    if (r < 0)
        return r;
    spin_lock(&mount_lock);
    struct list_head *pos;
    struct mount *m = NULL;
    list_for_each(pos, &mounts) {
        struct mount *o = list_entry(pos, struct mount, link);
        if (strcmp(o->path, canon) == 0) {
            m = o;
            break;
        }
    }
    if (!m) {
        spin_unlock(&mount_lock);
        return -EINVAL;
    }
    if (m == root_mount) {
        spin_unlock(&mount_lock);
        return -EBUSY;
    }
    /* Busy if any inode is still referenced, or another mount sits below. */
    list_for_each(pos, &mounts) {
        struct mount *o = list_entry(pos, struct mount, link);
        if (o->parent_sb == m->sb) {
            spin_unlock(&mount_lock);
            return -EBUSY;
        }
    }
    spin_lock(&m->sb->lock);
    bool busy = !list_empty(&m->sb->inodes);
    spin_unlock(&m->sb->lock);
    if (busy) {
        spin_unlock(&mount_lock);
        return -EBUSY;
    }
    list_del(&m->link);
    spin_unlock(&mount_lock);
    if (m->sb->ops->sync)
        m->sb->ops->sync(m->sb);
    if (m->sb->ops->unmount)
        m->sb->ops->unmount(m->sb);
    klog_info("unmounted %s", canon);
    kfree(m);
    return 0;
}

int vfs_umount_all(void)
{
    vfs_sync();
    int busy = 0;
    for (;;) {
        struct mount *m = NULL;
        spin_lock(&mount_lock);
        for (struct list_head *pos = mounts.prev; pos != &mounts; pos = pos->prev) {
            struct mount *o = list_entry(pos, struct mount, link);
            spin_lock(&o->sb->lock);
            bool inuse = !list_empty(&o->sb->inodes);
            spin_unlock(&o->sb->lock);
            if (!inuse) {
                m = o;
                break;
            }
        }
        if (m) {
            list_del(&m->link);
            if (m == root_mount)
                root_mount = NULL;
        }
        spin_unlock(&mount_lock);
        if (!m)
            break;
        if (m->sb->ops->sync)
            m->sb->ops->sync(m->sb);
        if (m->sb->ops->unmount)
            m->sb->ops->unmount(m->sb);
        klog_info("unmounted %s", m->path);
        kfree(m);
    }
    /* Filesystems without an unmount operation keep no state on disk, so
     * leaving them mounted (devfs holds the console of the caller) is
     * harmless. */
    spin_lock(&mount_lock);
    struct list_head *pos;
    list_for_each(pos, &mounts) {
        struct mount *o = list_entry(pos, struct mount, link);
        if (!o->sb->ops->unmount)
            continue;
        klog_warn("%s is busy, not unmounted", o->path);
        busy++;
    }
    spin_unlock(&mount_lock);
    return busy;
}

int vfs_sync(void)
{
    int r = 0;
    spin_lock(&mount_lock);
    struct list_head *pos;
    list_for_each(pos, &mounts) {
        struct mount *m = list_entry(pos, struct mount, link);
        spin_unlock(&mount_lock);
        if (m->sb->ops->sync) {
            int e = m->sb->ops->sync(m->sb);
            if (e < 0 && r == 0)
                r = e;
        }
        spin_lock(&mount_lock);
    }
    spin_unlock(&mount_lock);
    int e = bcache_sync(NULL);
    return r ? r : e;
}

/* ---- namespace operations ---- */

int vfs_open(const char *path, int flags, uint32_t mode, struct file **out)
{
    struct inode *ino = NULL;
    int r;
    if (flags & O_CREAT) {
        struct inode *dir;
        char name[NAME_MAX + 1];
        r = vfs_lookup_parent(path, &dir, name, sizeof name);
        if (r < 0)
            return r;
        size_t len = strlen(name);
        vfs_op_begin(dir->sb);
        mutex_lock(&dir->lock);
        r = dir->ops && dir->ops->lookup ? dir->ops->lookup(dir, name, len, &ino) : -ENOENT;
        if (r == 0 && (flags & O_EXCL)) {
            inode_put(ino);
            r = -EEXIST;
        } else if (r == -ENOENT) {
            if (!dir->ops || !dir->ops->create)
                r = -EROFS;
            else
                r = dir->ops->create(dir, name, len, S_IFREG | (mode & 0777), &ino);
        }
        mutex_unlock(&dir->lock);
        vfs_op_end(dir->sb);
        inode_put(dir);
        if (r < 0)
            return r;
        ino = cross_mount(ino);
        if (!ino)
            return -ENOMEM;
    } else {
        r = vfs_lookup(path, &ino);
        if (r < 0)
            return r;
    }

    int acc = flags & O_ACCMODE;
    if (S_ISDIR(ino->mode) && (acc != O_RDONLY || (flags & O_TRUNC))) {
        r = -EISDIR;
        goto fail;
    }
    if ((flags & O_DIRECTORY) && !S_ISDIR(ino->mode)) {
        r = -ENOTDIR;
        goto fail;
    }
    if ((flags & O_TRUNC) && S_ISREG(ino->mode)) {
        if (!ino->ops || !ino->ops->truncate) {
            r = -EROFS;
            goto fail;
        }
        vfs_op_begin(ino->sb);
        mutex_lock(&ino->lock);
        r = ino->ops->truncate(ino, 0);
        mutex_unlock(&ino->lock);
        vfs_op_end(ino->sb);
        if (r < 0)
            goto fail;
        if (ino->mapping)
            filemap_truncate(ino, 0);
    }
    struct file *f = file_alloc(ino, ino->fops, flags);
    if (!f) {
        r = -ENOMEM;
        goto fail;
    }
    if (f->ops && f->ops->open) {
        r = f->ops->open(ino, f);
        if (r < 0) {
            file_put(f);   /* drops the inode reference */
            return r;
        }
    }
    *out = f;
    return 0;
fail:
    inode_put(ino);
    return r;
}

/* Run a directory operation on the parent of path. */
enum dir_op_kind { DIR_OP_MKDIR, DIR_OP_UNLINK, DIR_OP_RMDIR };

static int dir_op(const char *path, enum dir_op_kind kind)
{
    struct inode *dir;
    char name[NAME_MAX + 1];
    int r = vfs_lookup_parent(path, &dir, name, sizeof name);
    if (r < 0)
        return r;
    int (*fn)(struct inode *, const char *, size_t) = NULL;
    if (dir->ops) {
        switch (kind) {
        case DIR_OP_MKDIR:  fn = dir->ops->mkdir; break;
        case DIR_OP_UNLINK: fn = dir->ops->unlink; break;
        case DIR_OP_RMDIR:  fn = dir->ops->rmdir; break;
        }
    }
    if (!fn) {
        inode_put(dir);
        return -EROFS;
    }
    vfs_op_begin(dir->sb);
    mutex_lock(&dir->lock);
    r = fn(dir, name, strlen(name));
    mutex_unlock(&dir->lock);
    vfs_op_end(dir->sb);
    inode_put(dir);
    return r;
}

int vfs_mkdir(const char *path)
{
    return dir_op(path, DIR_OP_MKDIR);
}

int vfs_unlink(const char *path)
{
    return dir_op(path, DIR_OP_UNLINK);
}

int vfs_rmdir(const char *path)
{
    return dir_op(path, DIR_OP_RMDIR);
}

int vfs_link(const char *oldpath, const char *newpath)
{
    struct inode *target;
    int r = vfs_lookup(oldpath, &target);
    if (r < 0)
        return r;
    struct inode *dir;
    char name[NAME_MAX + 1];
    r = vfs_lookup_parent(newpath, &dir, name, sizeof name);
    if (r < 0) {
        inode_put(target);
        return r;
    }
    if (dir->sb != target->sb)
        r = -EXDEV;
    else if (S_ISDIR(target->mode))
        r = -EPERM;
    else if (!dir->ops || !dir->ops->link)
        r = -EROFS;
    else {
        vfs_op_begin(dir->sb);
        mutex_lock(&dir->lock);
        r = dir->ops->link(dir, name, strlen(name), target);
        mutex_unlock(&dir->lock);
        vfs_op_end(dir->sb);
    }
    inode_put(dir);
    inode_put(target);
    return r;
}

int vfs_rename(const char *oldpath, const char *newpath)
{
    struct inode *olddir, *newdir;
    char oldname[NAME_MAX + 1], newname[NAME_MAX + 1];
    int r = vfs_lookup_parent(oldpath, &olddir, oldname, sizeof oldname);
    if (r < 0)
        return r;
    r = vfs_lookup_parent(newpath, &newdir, newname, sizeof newname);
    if (r < 0) {
        inode_put(olddir);
        return r;
    }
    if (olddir->sb != newdir->sb)
        r = -EXDEV;
    else if (!olddir->ops || !olddir->ops->rename)
        r = -EROFS;
    else {
        /* Lock in a fixed order (lower inode number first) so two renames
         * between the same directories cannot deadlock. */
        struct inode *first = olddir, *second = newdir;
        if (olddir != newdir && newdir->ino < olddir->ino) {
            first = newdir;
            second = olddir;
        }
        vfs_op_begin(olddir->sb);
        mutex_lock(&first->lock);
        if (first != second)
            mutex_lock(&second->lock);
        r = olddir->ops->rename(olddir, oldname, strlen(oldname), newdir, newname, strlen(newname));
        if (first != second)
            mutex_unlock(&second->lock);
        mutex_unlock(&first->lock);
        vfs_op_end(olddir->sb);
    }
    inode_put(newdir);
    inode_put(olddir);
    return r;
}

/* ---- helpers ---- */

void vfs_op_begin(struct superblock *sb)
{
    if (sb && sb->ops->op_begin)
        sb->ops->op_begin(sb);
}

void vfs_op_end(struct superblock *sb)
{
    if (sb && sb->ops->op_end)
        sb->ops->op_end(sb);
}

uint8_t vfs_mode_to_dtype(uint32_t mode)
{
    switch (mode & S_IFMT) {
    case S_IFDIR: return DT_DIR;
    case S_IFREG: return DT_REG;
    case S_IFCHR: return DT_CHR;
    case S_IFBLK: return DT_BLK;
    case S_IFIFO: return DT_FIFO;
    default: return DT_UNKNOWN;
    }
}

void inode_stat(struct inode *ino, struct stat *st)
{
    memset(st, 0, sizeof *st);
    st->st_dev = ino->sb ? ino->sb->dev : 0;
    st->st_ino = ino->ino;
    st->st_mode = ino->mode;
    st->st_nlink = ino->nlink;
    st->st_rdev = ino->rdev;
    st->st_size = (int64_t)ino->size;
    st->st_blksize = 4096;
    st->st_blocks = (int64_t)((ino->size + 511) / 512);
}

long vfs_generic_lseek(struct file *f, long off, int whence)
{
    struct inode *ino = f->inode;
    if (ino && !(S_ISREG(ino->mode) || S_ISDIR(ino->mode) || S_ISBLK(ino->mode)))
        return -ESPIPE;
    long base;
    switch (whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = (long)f->pos; break;
    case SEEK_END: base = ino ? (long)ino->size : 0; break;
    default: return -EINVAL;
    }
    if (base + off < 0)
        return -EINVAL;
    f->pos = (uint64_t)(base + off);
    return base + off;
}
