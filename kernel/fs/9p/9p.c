/* The filesystem 9p: folders of the host through virtio-9p (V5 of
 * docs/plan/release-0.6.0.md, docs/design/9p.md).
 *
 * The inode number is the path of the qid. The server gives each file of
 * the share a different path. Each cached inode has a fid that a walk
 * reached and that is not opened. Each open file has a fid of its own,
 * opened with Tlopen. The client stores no data: reads and writes go to the
 * host at once, and every lookup reads the attributes again. A file that
 * the host changes therefore shows its new attributes at the next lookup.
 *
 * The VFS reads an inode through read_inode with nothing but its number.
 * A lookup therefore adds the walked fid and the attributes to the list
 * pending before it calls inode_get. read_inode takes the entry of
 * its number. When the inode was cached already, the lookup clunks its
 * fid again and copies the new attributes into the cached inode. The root
 * fid belongs to the superblock, and read_inode uses it for the root.
 *
 * Locking. p9_sb.lock protects pending. open_lock protects the list of
 * open files, which sync walks to send Tfsync. The fid of an inode does
 * not change while the inode exists. */
#define KLOG_SUBSYS "9p"
#include "9p.h"
#include <fs/9p.h>
#include <drivers/virtio/virtio_9p.h>
#include <lib/string.h>
#include <mm/slab.h>
#include <sync/mutex.h>
#include <klog.h>
#include <errno.h>

/* The attributes of a walked fid for read_inode. */
struct p9_pending {
    struct p9_pending *next;
    uint32_t fid;
    struct p9_attr attr;
    bool taken;
};

struct p9_sb {
    struct p9_client client;
    uint32_t root_fid;
    struct spinlock lock;
    struct p9_pending *pending;
    struct mutex open_lock;
    struct list_head open_files;    /* struct p9_open, open_lock */
};

/* The private part of an inode. */
struct p9_node {
    uint32_t fid;
    bool root;                      /* the fid is the root fid of the superblock */
};

/* The private part of an open file. dir_offset and dir_index are protected
 * by the lock of the file, which getdents runs under. */
struct p9_open {
    uint32_t fid;
    uint64_t dir_offset;            /* the offset of Treaddir for the entry dir_index */
    uint64_t dir_index;             /* the index of the next entry that getdents returns */
    struct list_head link;
};

static const struct inode_ops p9_dir_ops, p9_file_ops, p9_link_ops;
static const struct file_ops p9_dir_fops, p9_file_fops;

static struct p9_sb *psb(struct superblock *sb) { return sb->priv; }
static struct p9_client *client_of(struct inode *ino) { return &psb(ino->sb)->client; }
static uint32_t fid_of(struct inode *ino) { return ((struct p9_node *)ino->priv)->fid; }

/* Copy the attributes of the host into an inode. */
static void publish(struct inode *ino, const struct p9_attr *a)
{
    ino->mode = (ino->mode & S_IFMT) | (a->mode & 07777);
    ino->nlink = (uint32_t)a->nlink;
    ino->uid = a->uid;
    ino->gid = a->gid;
    ino->size = a->size;
    ino->rdev = a->rdev;
    ino->mtime = a->mtime;
}

static void set_ops(struct inode *ino)
{
    if (S_ISDIR(ino->mode)) {
        ino->ops = &p9_dir_ops;
        ino->fops = &p9_dir_fops;
    } else if (S_ISLNK(ino->mode)) {
        ino->ops = &p9_link_ops;
    } else if (S_ISREG(ino->mode)) {
        ino->ops = &p9_file_ops;
        ino->fops = &p9_file_fops;
    }
}

static int p9fs_read_inode(struct superblock *sb, uint64_t num, struct inode *ino)
{
    struct p9_sb *s = psb(sb);
    struct p9_node *node = kzalloc(sizeof *node);
    if (!node)
        return -ENOMEM;
    struct p9_attr a;
    bool found = false;
    spin_lock(&s->lock);
    for (struct p9_pending *p = s->pending; p; p = p->next)
        if (p->attr.qid.path == num && !p->taken) {
            p->taken = true;
            node->fid = p->fid;
            a = p->attr;
            found = true;
            break;
        }
    spin_unlock(&s->lock);
    if (!found) {
        int e = num == sb->root_ino ? p9_getattr(&s->client, s->root_fid, &a) : -ENOENT;
        if (e < 0) {
            kfree(node);
            return e;
        }
        node->fid = s->root_fid;
        node->root = true;
    }
    ino->mode = a.mode & S_IFMT;
    publish(ino, &a);
    ino->priv = node;
    set_ops(ino);
    return 0;
}

/* The inode goes away. Its fid is no longer needed. */
static void p9fs_put_inode(struct inode *ino)
{
    struct p9_node *node = ino->priv;
    if (!node->root)
        p9_clunk(client_of(ino), node->fid);
    kfree(node);
}

static void p9fs_free_inode(struct inode *ino)
{
    p9fs_put_inode(ino);
}

/* The inode of a walked fid with the attributes a. The fid belongs to the
 * inode afterwards, or it is clunked. dir is locked by the caller. */
static int inode_of_fid(struct inode *dir, uint32_t fid, const struct p9_attr *a, struct inode **out)
{
    struct p9_sb *s = psb(dir->sb);
    struct p9_pending p = { .fid = fid, .attr = *a };
    spin_lock(&s->lock);
    p.next = s->pending;
    s->pending = &p;
    spin_unlock(&s->lock);
    struct inode *ino = inode_get(dir->sb, a->qid.path);
    spin_lock(&s->lock);
    struct p9_pending **pp = &s->pending;
    while (*pp && *pp != &p)
        pp = &(*pp)->next;
    if (*pp)
        *pp = p.next;
    bool taken = p.taken;
    spin_unlock(&s->lock);
    if (!taken) {
        p9_clunk(&s->client, fid);
        if (ino && ino != dir) {
            mutex_lock(&ino->lock);
            publish(ino, a);
            mutex_unlock(&ino->lock);
        }
    }
    if (!ino)
        return -ENOMEM;
    *out = ino;
    return 0;
}

/* Walk from dir to name and read the attributes. The new fid belongs to
 * the caller. */
static int walk_name(struct inode *dir, const char *name, size_t len, uint32_t *fid, struct p9_attr *a)
{
    struct p9_client *c = client_of(dir);
    if (len > NAME_MAX)
        return -ENAMETOOLONG;
    *fid = p9_fid_alloc(c);
    if (*fid == P9_NOFID)
        return -ENFILE;
    int e = p9_walk(c, fid_of(dir), *fid, name, len, NULL);
    if (e < 0) {
        p9_fid_free(c, *fid);
        return e;
    }
    e = p9_getattr(c, *fid, a);
    if (e < 0)
        p9_clunk(c, *fid);
    return e;
}

static int p9fs_lookup(struct inode *dir, const char *name, size_t len, struct inode **out)
{
    uint32_t fid;
    struct p9_attr a;
    int e = walk_name(dir, name, len, &fid, &a);
    return e < 0 ? e : inode_of_fid(dir, fid, &a, out);
}

/* Read the attributes of an inode again after a change. A change of a
 * directory changes its time stamp and link count on the host. The caller
 * has locked ino. */
static void refresh(struct inode *ino)
{
    struct p9_attr a;
    if (p9_getattr(client_of(ino), fid_of(ino), &a) == 0)
        publish(ino, &a);
}

static int p9fs_create(struct inode *dir, const char *name, size_t len, uint32_t mode, struct inode **out)
{
    struct p9_client *c = client_of(dir);
    if (len > NAME_MAX)
        return -ENAMETOOLONG;
    uint32_t uid, gid;
    vfs_new_owner(dir, &uid, &gid);
    /* Tlcreate turns a fid of the directory into the new open file. */
    uint32_t fid = p9_fid_alloc(c);
    if (fid == P9_NOFID)
        return -ENFILE;
    int e = p9_walk(c, fid_of(dir), fid, NULL, 0, NULL);
    if (e < 0) {
        p9_fid_free(c, fid);
        return e;
    }
    e = p9_lcreate(c, fid, name, len, O_RDWR | O_CREAT | O_EXCL, mode & 07777, gid);
    p9_clunk(c, fid);
    if (e < 0)
        return e;
    refresh(dir);
    return p9fs_lookup(dir, name, len, out);
}

static int p9fs_mkdir(struct inode *dir, const char *name, size_t len, uint32_t mode)
{
    if (len > NAME_MAX)
        return -ENAMETOOLONG;
    uint32_t uid, gid;
    vfs_new_owner(dir, &uid, &gid);
    int e = p9_mkdir(client_of(dir), fid_of(dir), name, len, mode & 07777, gid);
    if (e == 0)
        refresh(dir);
    return e;
}

/* The link count of a cached inode after the host removed a name of it. */
static void name_removed(struct superblock *sb, uint64_t ino_num, bool dir)
{
    struct inode *ino = inode_get(sb, ino_num);
    if (!ino)
        return;
    mutex_lock(&ino->lock);
    if (dir)
        ino->nlink = 0;
    else if (ino->nlink)
        ino->nlink--;
    mutex_unlock(&ino->lock);
    inode_put(ino);
}

static int p9fs_unlink(struct inode *dir, const char *name, size_t len)
{
    struct p9_client *c = client_of(dir);
    uint32_t fid;
    struct p9_attr a;
    int e = walk_name(dir, name, len, &fid, &a);
    if (e < 0)
        return e;
    p9_clunk(c, fid);
    if (S_ISDIR(a.mode))
        return -EISDIR;
    e = p9_unlinkat(c, fid_of(dir), name, len, 0);
    if (e == 0) {
        refresh(dir);
        name_removed(dir->sb, a.qid.path, false);
    }
    return e;
}

static int p9fs_rmdir(struct inode *dir, const char *name, size_t len)
{
    if ((len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.'))
        return -EINVAL;
    struct p9_client *c = client_of(dir);
    uint32_t fid;
    struct p9_attr a;
    int e = walk_name(dir, name, len, &fid, &a);
    if (e < 0)
        return e;
    p9_clunk(c, fid);
    if (!S_ISDIR(a.mode))
        return -ENOTDIR;
    e = p9_unlinkat(c, fid_of(dir), name, len, P9_AT_REMOVEDIR);
    if (e == 0) {
        refresh(dir);
        name_removed(dir->sb, a.qid.path, true);
    }
    return e;
}

static int p9fs_link(struct inode *dir, const char *name, size_t len, struct inode *target)
{
    if (len > NAME_MAX)
        return -ENAMETOOLONG;
    int e = p9_link(client_of(dir), fid_of(dir), fid_of(target), name, len);
    if (e == 0) {
        refresh(dir);
        mutex_lock(&target->lock);
        refresh(target);
        mutex_unlock(&target->lock);
    }
    return e;
}

static int p9fs_symlink(struct inode *dir, const char *name, size_t len, const char *target, size_t tlen)
{
    if (len > NAME_MAX)
        return -ENAMETOOLONG;
    uint32_t uid, gid;
    vfs_new_owner(dir, &uid, &gid);
    int e = p9_symlink(client_of(dir), fid_of(dir), name, len, target, tlen, gid);
    if (e == 0)
        refresh(dir);
    return e;
}

static int p9fs_readlink(struct inode *ino, char *buf, size_t size)
{
    return p9_readlink(client_of(ino), fid_of(ino), buf, size);
}

static int p9fs_rename(struct inode *olddir, const char *oldname, size_t oldlen, struct inode *newdir,
                     const char *newname, size_t newlen)
{
    if (oldlen > NAME_MAX || newlen > NAME_MAX)
        return -ENAMETOOLONG;
    int e = p9_renameat(client_of(olddir), fid_of(olddir), oldname, oldlen, fid_of(newdir), newname, newlen);
    if (e == 0) {
        refresh(olddir);
        if (newdir != olddir)
            refresh(newdir);
    }
    return e;
}

static int p9fs_truncate(struct inode *ino, uint64_t size)
{
    struct p9_setattr s = { .valid = P9_SETATTR_SIZE, .size = size };
    int e = p9_setattr(client_of(ino), fid_of(ino), &s);
    if (e == 0)
        refresh(ino);
    return e;
}

static int p9fs_setmtime(struct inode *ino, int64_t mtime)
{
    struct p9_setattr s = { .valid = P9_SETATTR_MTIME | P9_SETATTR_MTIME_SET, .mtime = mtime };
    int e = p9_setattr(client_of(ino), fid_of(ino), &s);
    if (e == 0)
        refresh(ino);
    return e;
}

/* The host decides whether it can store an owner. A refusal of the host
 * reaches the caller, as EPERM for a filesystem that cannot store the
 * change. */
static int p9fs_setattr(struct inode *ino, uint32_t mode, uint32_t uid, uint32_t gid)
{
    struct p9_setattr s = { .valid = P9_SETATTR_MODE, .mode = mode & 07777 };
    if (uid != ino->uid)
        s.valid |= P9_SETATTR_UID, s.uid = uid;
    if (gid != ino->gid)
        s.valid |= P9_SETATTR_GID, s.gid = gid;
    int e = p9_setattr(client_of(ino), fid_of(ino), &s);
    refresh(ino);
    return e;
}

/* ---- open files ---- */

static int p9fs_open(struct inode *ino, struct file *f)
{
    struct p9_sb *s = psb(ino->sb);
    struct p9_client *c = &s->client;
    struct p9_open *o = kzalloc(sizeof *o);
    if (!o)
        return -ENOMEM;
    o->fid = p9_fid_alloc(c);
    if (o->fid == P9_NOFID) {
        kfree(o);
        return -ENFILE;
    }
    int e = p9_walk(c, fid_of(ino), o->fid, NULL, 0, NULL);
    if (e < 0) {
        p9_fid_free(c, o->fid);
        kfree(o);
        return e;
    }
    uint32_t flags = S_ISDIR(ino->mode) ? O_RDONLY : (uint32_t)(f->flags & O_ACCMODE);
    e = p9_lopen(c, o->fid, flags);
    if (e < 0) {
        p9_clunk(c, o->fid);
        kfree(o);
        return e;
    }
    /* The size of the host matters for O_APPEND and SEEK_END. */
    mutex_lock(&ino->lock);
    refresh(ino);
    mutex_unlock(&ino->lock);
    mutex_lock(&s->open_lock);
    list_add_tail(&o->link, &s->open_files);
    mutex_unlock(&s->open_lock);
    f->priv = o;
    return 0;
}

static void p9fs_release(struct file *f)
{
    struct p9_open *o = f->priv;
    if (!o)
        return;
    struct p9_sb *s = psb(f->inode->sb);
    mutex_lock(&s->open_lock);
    list_del(&o->link);
    mutex_unlock(&s->open_lock);
    p9_clunk(&s->client, o->fid);
    kfree(o);
}

static long p9fs_file_read(struct file *f, char *buf, size_t n, uint64_t *pos)
{
    struct p9_open *o = f->priv;
    struct p9_client *c = client_of(f->inode);
    size_t done = 0;
    while (done < n) {
        size_t want = MIN(n - done, (size_t)P9_IO_MAX);
        long r = p9_read(c, o->fid, *pos + done, buf + done, want);
        if (r < 0) {
            if (done)
                break;
            return r;
        }
        done += (size_t)r;
        if ((size_t)r < want)
            break;              /* the end of the file */
    }
    *pos += done;
    return (long)done;
}

static long p9fs_file_write(struct file *f, const char *buf, size_t n, uint64_t *pos)
{
    struct p9_open *o = f->priv;
    struct inode *ino = f->inode;
    struct p9_client *c = client_of(ino);
    size_t done = 0;
    long r = 0;
    while (done < n) {
        r = p9_write(c, o->fid, *pos + done, buf + done, n - done);
        if (r <= 0)
            break;
        done += (size_t)r;
    }
    if (done) {
        mutex_lock(&ino->lock);
        if (*pos + done > ino->size)
            ino->size = *pos + done;
        ino->mtime = vfs_now();
        mutex_unlock(&ino->lock);
    }
    *pos += done;
    return done ? (long)done : (r < 0 ? r : -EIO);
}

/* The entries of Treaddir, from the entry with the index f->pos on. A
 * position other than the one of the last call starts at the first entry
 * and skips to it. */
static long p9fs_getdents(struct file *f, struct dirent *buf, size_t count)
{
    struct p9_open *o = f->priv;
    struct p9_client *c = client_of(f->inode);
    size_t max = count / sizeof(struct dirent), filled = 0;
    if (f->pos != o->dir_index) {
        o->dir_offset = 0;
        o->dir_index = 0;
    }
    size_t cap = 8192;
    uint8_t *data = kmalloc(cap);
    if (!data)
        return -ENOMEM;
    long r = 0;
    while (filled < max) {
        r = p9_readdir(c, o->fid, o->dir_offset, data, cap);
        if (r <= 0)
            break;
        size_t at = 0;
        while (filled < max && at < (size_t)r) {
            struct p9_qid qid;
            uint64_t next;
            uint8_t type;
            const char *name;
            size_t len;
            size_t used = p9_dirent_parse(data + at, (size_t)r - at, &qid, &next, &type, &name, &len);
            if (used == 0 || len > NAME_MAX) {
                r = -EIO;
                break;
            }
            at += used;
            o->dir_offset = next;
            if (o->dir_index++ < f->pos)
                continue;
            buf[filled].d_ino = qid.path;
            buf[filled].d_type = type;
            memcpy(buf[filled].d_name, name, len);
            buf[filled].d_name[len] = '\0';
            filled++;
            f->pos++;
        }
        if (r < 0)
            break;
    }
    kfree(data);
    if (r < 0 && filled == 0)
        return r;
    return (long)(filled * sizeof(struct dirent));
}

/* ---- the superblock ---- */

static int p9fs_statfs_sb(struct superblock *sb, struct fs_space *space)
{
    struct p9_sb *s = psb(sb);
    return p9_statfs(&s->client, s->root_fid, space);
}

/* sync asks the host to write the open files to its disk. */
static int p9fs_sync(struct superblock *sb)
{
    struct p9_sb *s = psb(sb);
    int r = 0;
    mutex_lock(&s->open_lock);
    struct list_head *pos;
    list_for_each(pos, &s->open_files) {
        struct p9_open *o = list_entry(pos, struct p9_open, link);
        int e = p9_fsync(&s->client, o->fid);
        if (e < 0 && r == 0)
            r = e;
    }
    mutex_unlock(&s->open_lock);
    return r;
}

static void p9fs_unmount(struct superblock *sb)
{
    struct p9_sb *s = psb(sb);
    p9_clunk(&s->client, s->root_fid);
    kfree(s);
    kfree(sb);
}

static const struct inode_ops p9_dir_ops = {
    .lookup = p9fs_lookup,
    .create = p9fs_create,
    .mkdir = p9fs_mkdir,
    .unlink = p9fs_unlink,
    .rmdir = p9fs_rmdir,
    .link = p9fs_link,
    .symlink = p9fs_symlink,
    .rename = p9fs_rename,
    .setmtime = p9fs_setmtime,
    .setattr = p9fs_setattr,
};

static const struct inode_ops p9_file_ops = {
    .truncate = p9fs_truncate,
    .setmtime = p9fs_setmtime,
    .setattr = p9fs_setattr,
};

static const struct inode_ops p9_link_ops = {
    .readlink = p9fs_readlink,
    .setmtime = p9fs_setmtime,
    .setattr = p9fs_setattr,
};

static const struct file_ops p9_dir_fops = {
    .open = p9fs_open,
    .release = p9fs_release,
    .getdents = p9fs_getdents,
};

static const struct file_ops p9_file_fops = {
    .open = p9fs_open,
    .release = p9fs_release,
    .read = p9fs_file_read,
    .write = p9fs_file_write,
};

static const struct sb_ops p9_sb_ops = {
    .read_inode = p9fs_read_inode,
    .put_inode = p9fs_put_inode,
    .free_inode = p9fs_free_inode,
    .sync = p9fs_sync,
    .unmount = p9fs_unmount,
    .statfs = p9fs_statfs_sb,
};

/* The source is the mount tag of a virtio-9p device. The filesystem has no
 * options. */
static int p9fs_mount(const struct fs_type *type, const char *source, const char *options, struct superblock **out)
{
    static uint64_t instances;
    if (options[0])
        return -EINVAL;
    struct p9_channel *ch = virtio_9p_find(source);
    if (!ch)
        return -ENODEV;
    struct p9_sb *s = kzalloc(sizeof *s);
    struct superblock *sb = s ? sb_alloc(type, &p9_sb_ops) : NULL;
    if (!sb) {
        kfree(s);
        return -ENOMEM;
    }
    spinlock_init(&s->lock, "p9_sb");
    mutex_init(&s->open_lock, "p9_open");
    list_init(&s->open_files);
    int e = p9_connect(&s->client, ch);
    struct p9_qid root;
    if (e == 0) {
        s->root_fid = p9_fid_alloc(&s->client);
        e = p9_attach(&s->client, s->root_fid, 0, &root);
        if (e < 0)
            p9_fid_free(&s->client, s->root_fid);
    }
    if (e < 0) {
        klog_error("%s: %d", source, e);
        kfree(s);
        kfree(sb);
        return e;
    }
    sb->priv = s;
    sb->root_ino = root.path;
    sb->dev = 0x39700000 + __atomic_add_fetch(&instances, 1, __ATOMIC_RELAXED);
    klog_info("%s: 9P2000.L, message size %u", source, s->client.msize);
    *out = sb;
    return 0;
}

static struct fs_type p9_type = {
    .name = "9p",
    .mount = p9fs_mount,
};

void p9fs_init(void)
{
    vfs_register_fs(&p9_type);
}
