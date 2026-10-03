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
#include <drivers/rtc.h>
#include <drivers/timer.h>

/* Registered filesystem types. Protected by fs_types_lock. */
static LIST_HEAD(fs_types);
static DEFINE_SPINLOCK(fs_types_lock);

/* Mount table, root mount first. Protected by mount_lock. */
static LIST_HEAD(mounts);
static DEFINE_SPINLOCK(mount_lock);
static struct mount *root_mount;

void vfs_init(void)
{
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

int vfs_permission(struct inode *ino, int mask, const struct cred *c)
{
    uint32_t mode = __atomic_load_n(&ino->mode, __ATOMIC_RELAXED);
    if (cred_is_root(c)) {
        if ((mask & MAY_EXEC) && !S_ISDIR(mode) && !(mode & 0111))
            return -EACCES;
        return 0;
    }
    uint32_t bits;
    if (c->euid == __atomic_load_n(&ino->uid, __ATOMIC_RELAXED))
        bits = mode >> 6;
    else if (cred_in_group(c, __atomic_load_n(&ino->gid, __ATOMIC_RELAXED)))
        bits = mode >> 3;
    else
        bits = mode;
    return ((bits & 7) & (uint32_t)mask) == (uint32_t)mask ? 0 : -EACCES;
}

/* vfs_permission for the calling process. */
static int may_current(struct inode *ino, int mask)
{
    struct cred c;
    cred_get_current(&c);
    return vfs_permission(ino, mask, &c);
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

/* Read the target of the symbolic link link into buf, which holds
 * VFS_SYMLINK_MAX + 1 bytes, and terminate it. An empty target names
 * nothing. No other inode lock may be held. */
static int link_target(struct inode *link, char *buf)
{
    if (!link->ops || !link->ops->readlink)
        return -EINVAL;
    if (link->size > VFS_SYMLINK_MAX)
        return -ENAMETOOLONG;
    mutex_lock(&link->lock);
    int r = link->ops->readlink(link, buf, VFS_SYMLINK_MAX);
    mutex_unlock(&link->lock);
    if (r < 0)
        return r;
    if (r == 0)
        return -ENOENT;
    buf[r] = '\0';
    return r;
}

/* The state of one path walk, kmalloc'd because it is too large for the
 * kernel stack of a deep call chain. rest holds the components still to
 * be resolved: a symbolic link replaces its own component by its target,
 * so the remainder may grow beyond one path. path is the canonical name of
 * cur without symbolic links: a component is appended only when the walk
 * enters it, and it enters a link only as the last component of a lookup
 * with VFS_NOFOLLOW. */
#define WALK_REST_MAX (4 * VFS_PATH_MAX)
#define WALK_PARENT   2             /* stop before the last component; next to VFS_NOFOLLOW */

struct walk {
    struct inode *cur;              /* referenced */
    struct cred cred;               /* identity whose search permission counts */
    size_t plen;
    char path[VFS_PATH_MAX];
    char rest[WALK_REST_MAX];
    char target[VFS_SYMLINK_MAX + 1];
};

static int walk_as(struct walk *w, const char *path, unsigned flags, int *links, char *name, size_t namesize);

static int walk_to_root(struct walk *w)
{
    if (w->cur)
        inode_put(w->cur);
    w->cur = root_inode();
    w->path[0] = '/';
    w->path[1] = '\0';
    w->plen = 1;
    return w->cur ? 0 : -ENOMEM;
}

/* Move to the parent of w->cur. The parent is found again from the root
 * along w->path, which names directories only, so ".." after a symbolic
 * link leaves the directory the link led to, and ".." at the root of a
 * mounted filesystem reaches the parent of the directory it covers. */
static int walk_up(struct walk *w)
{
    if (w->plen == 1)
        return 0;
    while (w->plen > 1 && w->path[w->plen - 1] != '/')
        w->plen--;
    if (w->plen > 1)
        w->plen--;
    w->path[w->plen] = '\0';
    struct inode *dir = root_inode();
    if (!dir)
        return -ENOMEM;
    const char *p = w->path + 1;
    while (*p) {
        const char *start = p;
        while (*p && *p != '/')
            p++;
        struct inode *next;
        int r = lookup_child(dir, start, (size_t)(p - start), &next);
        inode_put(dir);
        if (r < 0)
            return r;
        dir = next;
        if (*p)
            p++;
    }
    inode_put(w->cur);
    w->cur = dir;
    return 0;
}

/* Resolve path from the root, or from the working directory when it is
 * relative, into w->cur and w->path. Symbolic links are followed in every
 * component, and in the last one unless flags has VFS_NOFOLLOW and the
 * component has no trailing slash; *links counts the links followed and
 * may not exceed SYMLOOP_MAX. With WALK_PARENT the walk stops at the
 * directory holding the last component and copies the component to name;
 * "." and ".." are refused there. On failure w->cur is NULL. */
static int walk(struct walk *w, const char *path, unsigned flags, int *links, char *name, size_t namesize)
{
    cred_get_current(&w->cred);
    return walk_as(w, path, flags, links, name, namesize);
}

/* walk with the identity already in w->cred. */
static int walk_as(struct walk *w, const char *path, unsigned flags, int *links, char *name, size_t namesize)
{
    w->cur = NULL;
    size_t n = strlen(path);
    if (n == 0)
        return -ENOENT;
    if (path[0] == '/') {
        if (n >= sizeof w->rest)
            return -ENAMETOOLONG;
        memcpy(w->rest, path, n + 1);
    } else {
        struct proc *p = thread_current()->proc;
        spin_lock(&p->lock);
        strlcpy(w->path, p->cwd, sizeof w->path);
        spin_unlock(&p->lock);
        int m = ksnprintf(w->rest, sizeof w->rest, "%s/%s", w->path, path);
        if (m < 0 || (size_t)m >= sizeof w->rest)
            return -ENAMETOOLONG;
    }
    int r = walk_to_root(w);
    if (r < 0)
        return r;
    size_t i = 0;
    for (;;) {
        while (w->rest[i] == '/')
            i++;
        if (!w->rest[i])
            break;
        size_t start = i;
        while (w->rest[i] && w->rest[i] != '/')
            i++;
        size_t len = i - start, end = i, j = i;
        while (w->rest[j] == '/')
            j++;
        bool last = w->rest[j] == '\0';
        bool trailing = last && j > end;
        const char *c = w->rest + start;
        bool dot = len == 1 && c[0] == '.';
        bool dotdot = len == 2 && c[0] == '.' && c[1] == '.';
        if (len > NAME_MAX) {
            r = -ENAMETOOLONG;
            goto fail;
        }
        if (!S_ISDIR(w->cur->mode)) {
            r = -ENOTDIR;
            goto fail;
        }
        if ((flags & WALK_PARENT) && last) {
            if (dot || dotdot) {
                r = -EINVAL;
                goto fail;
            }
            if (len >= namesize) {
                r = -ENAMETOOLONG;
                goto fail;
            }
            memcpy(name, c, len);
            name[len] = '\0';
            return 0;
        }
        if (dot)
            continue;
        if (dotdot) {
            r = walk_up(w);
            if (r < 0)
                goto fail;
            continue;
        }
        /* Entering a component needs search permission on the directory
         * holding it. */
        r = vfs_permission(w->cur, MAY_EXEC, &w->cred);
        if (r < 0)
            goto fail;
        struct inode *next;
        r = lookup_child(w->cur, c, len, &next);
        if (r < 0)
            goto fail;
        if (S_ISLNK(next->mode) && (!last || trailing || !(flags & VFS_NOFOLLOW))) {
            /* Replace the component by the target and continue from the
             * directory holding the link, or from the root. */
            int tlen = ++*links > SYMLOOP_MAX ? -ELOOP : link_target(next, w->target);
            inode_put(next);
            if (tlen < 0) {
                r = tlen;
                goto fail;
            }
            size_t tail = strlen(w->rest + end);
            if ((size_t)tlen + tail >= sizeof w->rest) {
                r = -ENAMETOOLONG;
                goto fail;
            }
            memmove(w->rest + tlen, w->rest + end, tail + 1);
            memcpy(w->rest, w->target, (size_t)tlen);
            i = 0;
            if (w->target[0] == '/') {
                r = walk_to_root(w);
                if (r < 0)
                    goto fail;
            }
            continue;
        }
        if (w->plen + 1 + len >= VFS_PATH_MAX) {
            inode_put(next);
            r = -ENAMETOOLONG;
            goto fail;
        }
        if (w->plen > 1)
            w->path[w->plen++] = '/';
        memcpy(w->path + w->plen, c, len);
        w->plen += len;
        w->path[w->plen] = '\0';
        inode_put(w->cur);
        w->cur = next;
    }
    if (!(flags & WALK_PARENT))
        return 0;
    r = -EINVAL;                    /* "/" has no parent and no name */
fail:
    if (w->cur)
        inode_put(w->cur);
    w->cur = NULL;
    return r;
}

int vfs_lookup_path(const char *path, unsigned flags, struct inode **out, char *phys, size_t physsize)
{
    struct walk *w = kmalloc(sizeof *w);
    if (!w)
        return -ENOMEM;
    int links = 0;
    int r = walk(w, path, flags & VFS_NOFOLLOW, &links, NULL, 0);
    if (r == 0 && phys) {
        if (w->plen >= physsize) {
            inode_put(w->cur);
            r = -ENAMETOOLONG;
        } else {
            memcpy(phys, w->path, w->plen + 1);
        }
    }
    if (r == 0)
        *out = w->cur;
    kfree(w);
    return r;
}

int vfs_lookup(const char *path, struct inode **out)
{
    return vfs_lookup_path(path, 0, out, NULL, 0);
}

/* vfs_lookup_parent with the link count of an outer lookup and, when phys
 * is not NULL, the canonical path of the directory in phys (VFS_PATH_MAX
 * bytes). */
static int lookup_parent(const char *path, int *links, struct inode **dir, char *name, size_t namesize,
                         char *phys)
{
    struct walk *w = kmalloc(sizeof *w);
    if (!w)
        return -ENOMEM;
    int r = walk(w, path, WALK_PARENT, links, name, namesize);
    if (r == 0 && !S_ISDIR(w->cur->mode)) {
        inode_put(w->cur);
        r = -ENOTDIR;
    }
    if (r == 0) {
        *dir = w->cur;
        if (phys)
            memcpy(phys, w->path, w->plen + 1);
    }
    kfree(w);
    return r;
}

int vfs_lookup_parent(const char *path, struct inode **dir, char *name, size_t namesize)
{
    int links = 0;
    name[0] = '\0';
    return lookup_parent(path, &links, dir, name, namesize, NULL);
}

/* ---- mounts ---- */

long vfs_format_mounts(char *buf, size_t size)
{
    size_t capacity = 0;
    struct list_head *pos;
    spin_lock(&mount_lock);
    list_for_each(pos, &mounts)
        capacity++;
    spin_unlock(&mount_lock);
    if (!capacity)
        return 0;
    struct mount **snapshot = kmalloc(capacity * sizeof *snapshot);
    if (!snapshot)
        return -ENOMEM;
    size_t count = 0;
    spin_lock(&mount_lock);
    list_for_each(pos, &mounts) {
        if (count == capacity)
            break;
        struct mount *m = list_entry(pos, struct mount, link);
        m->readers++;
        snapshot[count++] = m;
    }
    spin_unlock(&mount_lock);

    size_t used = 0;
    long error = 0;
    for (size_t i = 0; i < count; i++) {
        struct mount *m = snapshot[i];
        struct fs_space space = {0};
        if (m->sb->ops->statfs)
            m->sb->ops->statfs(m->sb, &space);
        if (!error) {
            int n = ksnprintf(buf + used, size - used, "%s %s %lu %lu %u\n",
                m->path, m->sb->type->name, space.blocks, space.free_blocks, space.block_size);
            if (n < 0 || (size_t)n >= size - used)
                error = -ENOSPC;
            else
                used += (size_t)n;
        }
        spin_lock(&mount_lock);
        m->readers--;
        spin_unlock(&mount_lock);
    }
    kfree(snapshot);
    return error ? error : (long)used;
}

int vfs_mount(const char *fstype, const char *source, const char *target, const char *options)
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
        /* The mount records the path without symbolic links, the name
         * that /dev/mounts shows and umount compares. */
        r = vfs_lookup_path(target, 0, &point, canon, sizeof canon);
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
    r = type->mount(type, source, options ? options : "", &m->sb);
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
    /* A target reached through symbolic links is compared by the path of
     * the directory it resolves to; a target that no longer resolves is
     * compared as written. */
    char canon[VFS_PATH_MAX];
    struct inode *ino;
    int r = vfs_lookup_path(target, 0, &ino, canon, sizeof canon);
    if (r == 0)
        inode_put(ino);
    else
        r = canonicalize_cwd(target, canon, sizeof canon);
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
    if (m == root_mount || m->readers) {
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
            bool inuse = !list_empty(&o->sb->inodes) || o->readers;
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

/* Open with O_CREAT: find or create the last component of path. A
 * symbolic link in the last component is followed like any other (O_EXCL
 * and O_NOFOLLOW refuse it), and a link whose target does not exist
 * creates the target, as POSIX specifies for open. phys (VFS_PATH_MAX
 * bytes) receives the canonical path of the result. */
static int open_create(const char *path, int flags, uint32_t mode, struct inode **out, char *phys, bool *created)
{
    *created = false;
    struct open_create_bufs {
        char name[NAME_MAX + 1];
        char target[VFS_SYMLINK_MAX + 1];
        char next[2 * VFS_PATH_MAX];
    } *b = kmalloc(sizeof *b);
    if (!b)
        return -ENOMEM;
    int links = 0, r;
    struct inode *ino = NULL;
    for (;;) {
        struct inode *dir;
        r = lookup_parent(path, &links, &dir, b->name, sizeof b->name, phys);
        if (r < 0)
            break;
        size_t len = strlen(b->name);
        /* Looking the name up needs search permission on the directory,
         * creating it also write permission. */
        r = may_current(dir, MAY_EXEC);
        if (r < 0) {
            inode_put(dir);
            break;
        }
        vfs_op_begin(dir->sb);
        mutex_lock(&dir->lock);
        r = dir->ops && dir->ops->lookup ? dir->ops->lookup(dir, b->name, len, &ino) : -ENOENT;
        if (r == 0 && (flags & O_EXCL)) {
            inode_put(ino);
            r = -EEXIST;
        } else if (r == -ENOENT) {
            if (!dir->ops || !dir->ops->create) {
                r = -EROFS;
            } else {
                r = may_current(dir, MAY_WRITE);
                if (r == 0)
                    r = dir->ops->create(dir, b->name, len, S_IFREG | (mode & 07777 & ~vfs_umask()), &ino);
                *created = r == 0;
            }
        }
        mutex_unlock(&dir->lock);
        vfs_op_end(dir->sb);
        inode_put(dir);
        if (r < 0)
            break;
        if (!S_ISLNK(ino->mode)) {
            size_t plen = strlen(phys);
            if (plen + 1 + len >= VFS_PATH_MAX) {
                inode_put(ino);
                r = -ENAMETOOLONG;
                break;
            }
            ksnprintf(phys + plen, VFS_PATH_MAX - plen, "%s%s", plen > 1 ? "/" : "", b->name);
            ino = cross_mount(ino);
            r = ino ? 0 : -ENOMEM;
            break;
        }
        /* Continue with the target, relative to the link's directory. */
        r = flags & O_NOFOLLOW ? -ELOOP : ++links > SYMLOOP_MAX ? -ELOOP : link_target(ino, b->target);
        inode_put(ino);
        ino = NULL;
        if (r < 0)
            break;
        if (b->target[0] == '/')
            strlcpy(b->next, b->target, sizeof b->next);
        else
            ksnprintf(b->next, sizeof b->next, "%s/%s", phys, b->target);
        path = b->next;
    }
    kfree(b);
    if (r == 0)
        *out = ino;
    return r;
}

/* The body of vfs_open and vfs_open_exec. With exec the file must be a
 * regular file with execute permission instead of satisfying the access
 * mode. */
static int open_common(const char *path, int flags, uint32_t mode, bool exec, struct file **out)
{
    struct inode *ino = NULL;
    char phys[VFS_PATH_MAX];
    bool created = false;
    int r;
    if (flags & O_CREAT) {
        r = open_create(path, flags, mode, &ino, phys, &created);
        if (r < 0)
            return r;
    } else {
        r = vfs_lookup_path(path, flags & O_NOFOLLOW ? VFS_NOFOLLOW : 0, &ino, phys, sizeof phys);
        if (r < 0)
            return r;
        if (S_ISLNK(ino->mode)) {
            inode_put(ino);
            return -ELOOP;
        }
    }

    int acc = flags & O_ACCMODE;
    if (S_ISDIR(ino->mode) && (acc != O_RDONLY || (flags & O_TRUNC))) {
        r = -EISDIR;
        goto fail;
    }
    if (exec) {
        r = S_ISREG(ino->mode) ? may_current(ino, MAY_EXEC) : -EACCES;
    } else if (!created) {
        int mask = acc == O_WRONLY ? MAY_WRITE : acc == O_RDWR ? MAY_READ | MAY_WRITE : MAY_READ;
        if (flags & O_TRUNC)
            mask |= MAY_WRITE;
        r = may_current(ino, mask);
    }
    if (r < 0)
        goto fail;
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
    if (S_ISDIR(ino->mode) || S_ISREG(ino->mode)) {
        size_t len = strlen(phys) + 1;
        f->path = kmalloc(len);
        if (f->path)
            memcpy(f->path, phys, len);
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

int vfs_open(const char *path, int flags, uint32_t mode, struct file **out)
{
    return open_common(path, flags, mode, false, out);
}

int vfs_open_exec(const char *path, struct file **out)
{
    return open_common(path, O_RDONLY, 0, true, out);
}

int vfs_access(const char *path, int mask, unsigned flags)
{
    struct walk *w = kmalloc(sizeof *w);
    if (!w)
        return -ENOMEM;
    cred_get_current(&w->cred);
    if (!(flags & VFS_EACCESS)) {
        w->cred.euid = w->cred.ruid;
        w->cred.egid = w->cred.rgid;
    }
    int links = 0;
    int r = walk_as(w, path, flags & VFS_NOFOLLOW, &links, NULL, 0);
    if (r == 0) {
        if (mask)
            r = vfs_permission(w->cur, mask, &w->cred);
        inode_put(w->cur);
    }
    kfree(w);
    return r;
}

/* Removing or replacing an entry of a sticky directory is reserved to the
 * owners of the entry and of the directory, and to root. */
static int sticky_check(struct inode *dir, const char *name)
{
    if (!(dir->mode & S_ISVTX))
        return 0;
    struct cred c;
    cred_get_current(&c);
    if (cred_is_root(&c) || c.euid == dir->uid)
        return 0;
    struct inode *child;
    int r = lookup_child(dir, name, strlen(name), &child);
    if (r < 0)
        return r == -ENOENT ? 0 : r;
    r = child->uid == c.euid ? 0 : -EPERM;
    inode_put(child);
    return r;
}

/* Run a directory operation on the parent of path. */
enum dir_op_kind { DIR_OP_UNLINK, DIR_OP_RMDIR };

static int dir_op(const char *path, enum dir_op_kind kind)
{
    struct inode *dir;
    char name[NAME_MAX + 1];
    int r = vfs_lookup_parent(path, &dir, name, sizeof name);
    if (r < 0)
        return r;
    int (*fn)(struct inode *, const char *, size_t) = NULL;
    if (dir->ops)
        fn = kind == DIR_OP_UNLINK ? dir->ops->unlink : dir->ops->rmdir;
    if (!fn) {
        inode_put(dir);
        return -EROFS;
    }
    r = may_current(dir, MAY_WRITE | MAY_EXEC);
    if (r == 0)
        r = sticky_check(dir, name);
    if (r < 0) {
        inode_put(dir);
        return r;
    }
    vfs_op_begin(dir->sb);
    mutex_lock(&dir->lock);
    r = fn(dir, name, strlen(name));
    mutex_unlock(&dir->lock);
    vfs_op_end(dir->sb);
    inode_put(dir);
    return r;
}

/* A directory made in a directory with the set group id bit inherits the
 * bit, which keeps the group of a shared tree. */
int vfs_mkdir(const char *path, uint32_t mode)
{
    struct inode *dir;
    char name[NAME_MAX + 1];
    int r = vfs_lookup_parent(path, &dir, name, sizeof name);
    if (r < 0)
        return r;
    if (!dir->ops || !dir->ops->mkdir) {
        inode_put(dir);
        return -EROFS;
    }
    r = may_current(dir, MAY_WRITE | MAY_EXEC);
    if (r < 0) {
        inode_put(dir);
        return r;
    }
    mode &= 07777 & ~vfs_umask();
    if (dir->mode & S_ISGID)
        mode |= S_ISGID;
    vfs_op_begin(dir->sb);
    mutex_lock(&dir->lock);
    r = dir->ops->mkdir(dir, name, strlen(name), mode);
    mutex_unlock(&dir->lock);
    vfs_op_end(dir->sb);
    inode_put(dir);
    return r;
}

int vfs_option_uint(const char *opt, size_t len, const char *name, unsigned base, uint32_t *out)
{
    size_t n = strlen(name);
    if (len <= n || strncmp(opt, name, n) != 0 || opt[n] != '=')
        return 0;
    uint64_t v = 0;
    for (size_t i = n + 1; i < len; i++) {
        unsigned d = (unsigned)(opt[i] - '0');
        if (d >= base)
            return -EINVAL;
        v = v * base + d;
        if (v > 0xfffffffeu)
            return -EINVAL;
    }
    *out = (uint32_t)v;
    return 1;
}

uint32_t vfs_umask(void)
{
    struct proc *p = thread_current()->proc;
    return __atomic_load_n(&p->cred.umask, __ATOMIC_RELAXED);
}

void vfs_new_owner(struct inode *dir, uint32_t *uid, uint32_t *gid)
{
    struct cred c;
    cred_get_current(&c);
    *uid = c.euid;
    *gid = dir && (dir->mode & S_ISGID) ? dir->gid : c.egid;
}

static int setattr_locked(struct inode *ino, uint32_t mode, uint32_t uid, uint32_t gid)
{
    if (!ino->ops || !ino->ops->setattr)
        return -EROFS;
    vfs_op_begin(ino->sb);
    mutex_lock(&ino->lock);
    int r = ino->ops->setattr(ino, mode & 07777, uid, gid);
    mutex_unlock(&ino->lock);
    vfs_op_end(ino->sb);
    return r;
}

int vfs_chmod_inode(struct inode *ino, uint32_t mode)
{
    struct cred c;
    cred_get_current(&c);
    if (!cred_is_root(&c)) {
        if (c.euid != ino->uid)
            return -EPERM;
        /* Only a member of the file's group may set its set group id bit. */
        if ((mode & S_ISGID) && !cred_in_group(&c, ino->gid))
            mode &= ~(uint32_t)S_ISGID;
    }
    return setattr_locked(ino, mode, ino->uid, ino->gid);
}

int vfs_chown_inode(struct inode *ino, uint32_t uid, uint32_t gid)
{
    struct cred c;
    cred_get_current(&c);
    uint32_t new_uid = uid == VFS_CHOWN_KEEP ? ino->uid : uid;
    uint32_t new_gid = gid == VFS_CHOWN_KEEP ? ino->gid : gid;
    uint32_t mode = ino->mode & 07777;
    if (!cred_is_root(&c)) {
        if (new_uid != ino->uid || c.euid != ino->uid)
            return -EPERM;
        if (new_gid != ino->gid && !cred_in_group(&c, new_gid))
            return -EPERM;
        /* A change by anyone but root drops the set id bits of a file, which
         * would otherwise run with privileges its new owner never granted. */
        if (!S_ISDIR(ino->mode))
            mode &= ~(uint32_t)(S_ISUID | S_ISGID);
    }
    return setattr_locked(ino, mode, new_uid, new_gid);
}

int vfs_chmod(const char *path, uint32_t mode, unsigned flags)
{
    struct inode *ino;
    int r = vfs_lookup_path(path, flags & VFS_NOFOLLOW, &ino, NULL, 0);
    if (r < 0)
        return r;
    r = vfs_chmod_inode(ino, mode);
    inode_put(ino);
    return r;
}

int vfs_chown(const char *path, uint32_t uid, uint32_t gid, unsigned flags)
{
    struct inode *ino;
    int r = vfs_lookup_path(path, flags & VFS_NOFOLLOW, &ino, NULL, 0);
    if (r < 0)
        return r;
    r = vfs_chown_inode(ino, uid, gid);
    inode_put(ino);
    return r;
}

int vfs_utimens(const char *path, int64_t mtime, unsigned flags)
{
    struct inode *ino;
    int r = vfs_lookup_path(path, flags & VFS_NOFOLLOW, &ino, NULL, 0);
    if (r < 0)
        return r;
    if (!ino->ops || !ino->ops->setmtime) {
        inode_put(ino);
        return -EROFS;
    }
    struct cred c;
    cred_get_current(&c);
    if (!cred_is_root(&c) && c.euid != ino->uid) {
        r = flags & VFS_UTIME_NOW ? vfs_permission(ino, MAY_WRITE, &c) : -EPERM;
        if (r < 0) {
            inode_put(ino);
            return r;
        }
    }
    vfs_op_begin(ino->sb);
    mutex_lock(&ino->lock);
    r = ino->ops->setmtime(ino, mtime);
    mutex_unlock(&ino->lock);
    vfs_op_end(ino->sb);
    inode_put(ino);
    return r;
}

int vfs_unlink(const char *path)
{
    return dir_op(path, DIR_OP_UNLINK);
}

int vfs_rmdir(const char *path)
{
    return dir_op(path, DIR_OP_RMDIR);
}

/* A symbolic link named by oldpath is linked itself, not its target
 * (POSIX leaves the choice to the implementation). */
int vfs_link(const char *oldpath, const char *newpath)
{
    struct inode *target;
    int r = vfs_lookup_path(oldpath, VFS_NOFOLLOW, &target, NULL, 0);
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
    else if ((r = may_current(dir, MAY_WRITE | MAY_EXEC)) == 0) {
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

int vfs_symlink(const char *target, const char *path)
{
    size_t tlen = strlen(target);
    if (tlen == 0)
        return -ENOENT;
    if (tlen > VFS_SYMLINK_MAX)
        return -ENAMETOOLONG;
    struct inode *dir;
    char name[NAME_MAX + 1];
    int r = vfs_lookup_parent(path, &dir, name, sizeof name);
    if (r < 0)
        return r;
    if (!dir->ops || !dir->ops->symlink) {
        r = -EROFS;
    } else if ((r = may_current(dir, MAY_WRITE | MAY_EXEC)) == 0) {
        vfs_op_begin(dir->sb);
        mutex_lock(&dir->lock);
        r = dir->ops->symlink(dir, name, strlen(name), target, tlen);
        mutex_unlock(&dir->lock);
        vfs_op_end(dir->sb);
    }
    inode_put(dir);
    return r;
}

int vfs_readlink(const char *path, char *buf, size_t size)
{
    struct inode *ino;
    int r = vfs_lookup_path(path, VFS_NOFOLLOW, &ino, NULL, 0);
    if (r < 0)
        return r;
    if (!S_ISLNK(ino->mode) || !ino->ops || !ino->ops->readlink) {
        r = -EINVAL;
    } else {
        mutex_lock(&ino->lock);
        r = ino->ops->readlink(ino, buf, size);
        mutex_unlock(&ino->lock);
    }
    inode_put(ino);
    return r;
}

/* Renaming needs write and search permission on both directories, obeys
 * the sticky bit for the moved entry and for an entry it replaces, and a
 * directory that moves to another parent must be writable itself, since
 * its ".." entry changes. */
static int rename_allowed(struct inode *olddir, const char *oldname, struct inode *newdir, const char *newname)
{
    int r = may_current(olddir, MAY_WRITE | MAY_EXEC);
    if (r == 0 && newdir != olddir)
        r = may_current(newdir, MAY_WRITE | MAY_EXEC);
    if (r == 0)
        r = sticky_check(olddir, oldname);
    if (r == 0)
        r = sticky_check(newdir, newname);
    if (r == 0 && newdir != olddir) {
        struct inode *child;
        if (lookup_child(olddir, oldname, strlen(oldname), &child) == 0) {
            if (S_ISDIR(child->mode))
                r = may_current(child, MAY_WRITE);
            inode_put(child);
        }
    }
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
    else if ((r = rename_allowed(olddir, oldname, newdir, newname)) == 0) {
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
    case S_IFLNK: return DT_LNK;
    default: return DT_UNKNOWN;
    }
}

int64_t vfs_now(void)
{
    return (int64_t)(rtc_epoch_offset_ns() + timer_ns());
}

void inode_stat(struct inode *ino, struct stat *st)
{
    memset(st, 0, sizeof *st);
    st->st_dev = ino->sb ? ino->sb->dev : 0;
    st->st_ino = ino->ino;
    st->st_mode = ino->mode;
    st->st_nlink = ino->nlink;
    st->st_uid = ino->uid;
    st->st_gid = ino->gid;
    st->st_rdev = ino->rdev;
    st->st_size = (int64_t)ino->size;
    st->st_mtim.tv_sec = ino->mtime / 1000000000;
    st->st_mtim.tv_nsec = ino->mtime % 1000000000;
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
