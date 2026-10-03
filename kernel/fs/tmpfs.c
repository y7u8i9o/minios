/* tmpfs: a filesystem in memory for /tmp and /run (docs/design/vfs.md,
 * P4 of docs/plan/packaging.md). Each file, directory and symbolic link is
 * a node that the filesystem retains for as long as it has a name. The VFS
 * caches inodes only while they are referenced, which makes an inode a
 * view of its node: read_inode fills it from the node, and every operation changes
 * the node and the inode together. The data are the pages of a file, the
 * entries of a directory in the order of their creation, or the target of
 * a link. The last reference to a node without links frees it.
 *
 * Locking. The fields of a node are protected by the lock of its cached
 * inode. read_inode reads a node only when no inode of it is cached, and
 * nothing changes a node then, since every change goes through an inode.
 * tmpfs_sb.lock protects the hash table of the nodes, the next inode
 * number and the page count. It is a leaf lock taken under inode locks
 * (docs/design/locking.md). */
#define KLOG_SUBSYS "tmpfs"
#include <fs/tmpfs.h>
#include <fs/vfs.h>
#include <mm/memlayout.h>
#include <mm/pmm.h>
#include <mm/slab.h>
#include <lib/string.h>
#include <klog.h>
#include <errno.h>

#define TMPFS_HASH 256
#define TMPFS_ROOT_INO 1

struct tmpfs_dirent {
    struct tmpfs_dirent *next;
    struct tmpfs_node *node;
    size_t len;
    char name[];
};

struct tmpfs_node {
    uint64_t ino;
    uint32_t mode, nlink, uid, gid;
    uint64_t size;
    int64_t mtime;
    uint8_t **pages;                /* regular file: one pointer per page, NULL for a hole */
    size_t npages;                  /* length of pages */
    struct tmpfs_dirent *entries;   /* directory */
    struct tmpfs_node *parent;      /* directory: the target of ".." */
    char *target;                   /* symbolic link, size bytes */
    struct tmpfs_node *hash_next;   /* tmpfs_sb.hash, tmpfs_sb.lock */
};

/* One mounted tmpfs. lock protects hash, next_ino and pages. */
struct tmpfs_sb {
    struct spinlock lock;
    struct tmpfs_node *hash[TMPFS_HASH];
    uint64_t next_ino;
    uint64_t pages, max_pages;
};

static const struct inode_ops tmpfs_dir_ops, tmpfs_file_ops, tmpfs_link_ops;
static const struct file_ops tmpfs_dir_fops, tmpfs_file_fops;

static struct tmpfs_sb *tsb_of(struct superblock *sb) { return sb->priv; }

/* The data of a file live in whole pages of the page allocator, reached
 * through the direct map. */
static uint8_t *page_new(void)
{
    struct page *pg = pmm_alloc_page();
    if (!pg)
        return NULL;
    uint8_t *data = P2V(page_to_phys(pg));
    memset(data, 0, PAGE_SIZE);
    return data;
}

static void page_release(uint8_t *data)
{
    pmm_free_page(phys_to_page(V2P(data)));
}
static struct tmpfs_node *node_of(struct inode *ino) { return ino->priv; }

static struct tmpfs_node *node_find(struct tmpfs_sb *t, uint64_t ino)
{
    spin_lock(&t->lock);
    struct tmpfs_node *n = t->hash[ino % TMPFS_HASH];
    while (n && n->ino != ino)
        n = n->hash_next;
    spin_unlock(&t->lock);
    return n;
}

static void node_unhash(struct tmpfs_sb *t, struct tmpfs_node *n)
{
    spin_lock(&t->lock);
    struct tmpfs_node **pp = &t->hash[n->ino % TMPFS_HASH];
    while (*pp && *pp != n)
        pp = &(*pp)->hash_next;
    if (*pp)
        *pp = n->hash_next;
    spin_unlock(&t->lock);
}

/* Reserve count pages of the size limit, or release them with a negative
 * count. */
static int pages_charge(struct tmpfs_sb *t, long count)
{
    int r = 0;
    spin_lock(&t->lock);
    if (count > 0 && t->pages + (uint64_t)count > t->max_pages)
        r = -ENOSPC;
    else
        t->pages += (uint64_t)count;
    spin_unlock(&t->lock);
    return r;
}

static void node_free(struct tmpfs_sb *t, struct tmpfs_node *n)
{
    long freed = 0;
    for (size_t i = 0; i < n->npages; i++)
        if (n->pages[i]) {
            page_release(n->pages[i]);
            freed++;
        }
    kfree(n->pages);
    while (n->entries) {
        struct tmpfs_dirent *e = n->entries;
        n->entries = e->next;
        kfree(e);
    }
    kfree(n->target);
    pages_charge(t, -freed);
    kfree(n);
}

/* Copy the attributes of a node into its inode. */
static void publish(struct inode *ino, const struct tmpfs_node *n)
{
    ino->mode = n->mode;
    ino->nlink = n->nlink;
    ino->uid = n->uid;
    ino->gid = n->gid;
    ino->size = n->size;
    ino->mtime = n->mtime;
}

static int tmpfs_read_inode(struct superblock *sb, uint64_t num, struct inode *ino)
{
    struct tmpfs_node *n = node_find(tsb_of(sb), num);
    if (!n)
        return -ENOENT;
    publish(ino, n);
    ino->priv = n;
    if (S_ISDIR(n->mode)) {
        ino->ops = &tmpfs_dir_ops;
        ino->fops = &tmpfs_dir_fops;
    } else if (S_ISLNK(n->mode)) {
        ino->ops = &tmpfs_link_ops;
    } else {
        ino->ops = &tmpfs_file_ops;
        ino->fops = &tmpfs_file_fops;
    }
    return 0;
}

/* The last reference to an inode went away. A node without links has no
 * name that leads to it any more and is freed with its data. */
static void tmpfs_put_inode(struct inode *ino)
{
    struct tmpfs_node *n = node_of(ino);
    if (n->nlink == 0) {
        node_unhash(tsb_of(ino->sb), n);
        node_free(tsb_of(ino->sb), n);
    }
}

/* A new node with one or, for a directory, two links, owned as
 * vfs_new_owner decides, and its inode. NULL when memory is short. */
static struct inode *node_new(struct inode *dir, uint32_t mode)
{
    struct tmpfs_sb *t = tsb_of(dir->sb);
    struct tmpfs_node *n = kzalloc(sizeof *n);
    if (!n)
        return NULL;
    n->mode = mode;
    n->nlink = S_ISDIR(mode) ? 2 : 1;
    vfs_new_owner(dir, &n->uid, &n->gid);
    n->mtime = vfs_now();
    n->parent = node_of(dir);
    spin_lock(&t->lock);
    n->ino = t->next_ino++;
    n->hash_next = t->hash[n->ino % TMPFS_HASH];
    t->hash[n->ino % TMPFS_HASH] = n;
    spin_unlock(&t->lock);
    struct inode *ino = inode_get(dir->sb, n->ino);
    if (!ino) {
        node_unhash(t, n);
        node_free(t, n);
    }
    return ino;
}

static struct tmpfs_dirent **entry_find(struct inode *dir, const char *name, size_t len)
{
    struct tmpfs_dirent **pp = &node_of(dir)->entries;
    while (*pp && ((*pp)->len != len || memcmp((*pp)->name, name, len) != 0))
        pp = &(*pp)->next;
    return pp;
}

static int entry_add(struct inode *dir, const char *name, size_t len, struct tmpfs_node *n)
{
    if (len > NAME_MAX)
        return -ENAMETOOLONG;
    struct tmpfs_dirent *e = kmalloc(sizeof *e + len);
    if (!e)
        return -ENOMEM;
    e->next = NULL;
    e->node = n;
    e->len = len;
    memcpy(e->name, name, len);
    struct tmpfs_dirent **pp = &node_of(dir)->entries;
    while (*pp)
        pp = &(*pp)->next;
    *pp = e;
    node_of(dir)->mtime = dir->mtime = vfs_now();
    return 0;
}

static void entry_remove(struct inode *dir, struct tmpfs_dirent **pp)
{
    struct tmpfs_dirent *e = *pp;
    *pp = e->next;
    kfree(e);
    node_of(dir)->mtime = dir->mtime = vfs_now();
}

/* Change the link count of the node of a directory entry through its
 * inode, which the caller has not locked. */
static void links_add(struct superblock *sb, struct tmpfs_node *n, int delta)
{
    struct inode *ino = inode_get(sb, n->ino);
    if (!ino)
        return;
    mutex_lock(&ino->lock);
    n->nlink = delta < 0 && (uint32_t)-delta > n->nlink ? 0 : n->nlink + (uint32_t)delta;
    ino->nlink = n->nlink;
    mutex_unlock(&ino->lock);
    inode_put(ino);
}

static void dir_links_add(struct inode *dir, int delta)
{
    node_of(dir)->nlink += (uint32_t)delta;
    dir->nlink = node_of(dir)->nlink;
}

static int tmpfs_lookup(struct inode *dir, const char *name, size_t len, struct inode **out)
{
    struct tmpfs_node *n = NULL;
    if (len == 1 && name[0] == '.') {
        n = node_of(dir);
    } else if (len == 2 && name[0] == '.' && name[1] == '.') {
        n = node_of(dir)->parent;
    } else {
        struct tmpfs_dirent *e = *entry_find(dir, name, len);
        n = e ? e->node : NULL;
    }
    if (!n)
        return -ENOENT;
    *out = inode_get(dir->sb, n->ino);
    return *out ? 0 : -ENOMEM;
}

static int tmpfs_create(struct inode *dir, const char *name, size_t len, uint32_t mode, struct inode **out)
{
    if (*entry_find(dir, name, len))
        return -EEXIST;
    struct inode *ino = node_new(dir, S_IFREG | (mode & 07777));
    if (!ino)
        return -ENOMEM;
    int r = entry_add(dir, name, len, node_of(ino));
    if (r < 0) {
        node_of(ino)->nlink = ino->nlink = 0;
        inode_put(ino);
        return r;
    }
    *out = ino;
    return 0;
}

static int tmpfs_mkdir(struct inode *dir, const char *name, size_t len, uint32_t mode)
{
    if (*entry_find(dir, name, len))
        return -EEXIST;
    struct inode *sub = node_new(dir, S_IFDIR | (mode & 07777));
    if (!sub)
        return -ENOMEM;
    int r = entry_add(dir, name, len, node_of(sub));
    if (r < 0)
        node_of(sub)->nlink = sub->nlink = 0;
    else
        dir_links_add(dir, 1);
    inode_put(sub);
    return r;
}

static int tmpfs_symlink(struct inode *dir, const char *name, size_t len, const char *target, size_t tlen)
{
    if (*entry_find(dir, name, len))
        return -EEXIST;
    if (tlen == 0 || tlen > VFS_SYMLINK_MAX)
        return -ENAMETOOLONG;
    struct inode *ino = node_new(dir, S_IFLNK | 0777);
    if (!ino)
        return -ENOMEM;
    struct tmpfs_node *n = node_of(ino);
    int r = (n->target = kmalloc(tlen)) ? 0 : -ENOMEM;
    if (r == 0) {
        memcpy(n->target, target, tlen);
        n->size = ino->size = tlen;
        r = entry_add(dir, name, len, n);
    }
    if (r < 0)
        n->nlink = ino->nlink = 0;
    inode_put(ino);
    return r;
}

static int tmpfs_readlink(struct inode *ino, char *buf, size_t size)
{
    struct tmpfs_node *n = node_of(ino);
    size_t len = MIN(size, (size_t)n->size);
    memcpy(buf, n->target, len);
    return (int)len;
}

static int tmpfs_link(struct inode *dir, const char *name, size_t len, struct inode *target)
{
    if (*entry_find(dir, name, len))
        return -EEXIST;
    int r = entry_add(dir, name, len, node_of(target));
    if (r == 0) {
        mutex_lock(&target->lock);
        target->nlink = ++node_of(target)->nlink;
        mutex_unlock(&target->lock);
    }
    return r;
}

static int tmpfs_unlink(struct inode *dir, const char *name, size_t len)
{
    struct tmpfs_dirent **pp = entry_find(dir, name, len);
    if (!*pp)
        return -ENOENT;
    struct tmpfs_node *n = (*pp)->node;
    if (S_ISDIR(n->mode))
        return -EISDIR;
    entry_remove(dir, pp);
    links_add(dir->sb, n, -1);
    return 0;
}

static int tmpfs_rmdir(struct inode *dir, const char *name, size_t len)
{
    if ((len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.'))
        return -EINVAL;
    struct tmpfs_dirent **pp = entry_find(dir, name, len);
    if (!*pp)
        return -ENOENT;
    struct tmpfs_node *n = (*pp)->node;
    if (!S_ISDIR(n->mode))
        return -ENOTDIR;
    struct inode *sub = inode_get(dir->sb, n->ino);
    if (!sub)
        return -ENOMEM;
    int r = 0;
    mutex_lock(&sub->lock);
    if (n->entries) {
        r = -ENOTEMPTY;
    } else {
        entry_remove(dir, pp);
        n->nlink = sub->nlink = 0;
        dir_links_add(dir, -1);
    }
    mutex_unlock(&sub->lock);
    inode_put(sub);
    return r;
}

static int tmpfs_rename(struct inode *olddir, const char *oldname, size_t oldlen,
                        struct inode *newdir, const char *newname, size_t newlen)
{
    struct tmpfs_dirent **oldp = entry_find(olddir, oldname, oldlen);
    if (!*oldp)
        return -ENOENT;
    struct tmpfs_node *src = (*oldp)->node;
    bool src_dir = S_ISDIR(src->mode);
    struct tmpfs_dirent **newp = entry_find(newdir, newname, newlen);
    if (*newp && (*newp)->node == src)
        return 0;
    if (*newp) {
        struct tmpfs_node *dst = (*newp)->node;
        bool dst_dir = S_ISDIR(dst->mode);
        if (dst_dir && !src_dir)
            return -EISDIR;
        if (!dst_dir && src_dir)
            return -ENOTDIR;
        if (dst_dir && dst->entries)
            return -ENOTEMPTY;
        /* The entry now names the source, and the replaced node loses a
         * link, all of them for a directory. */
        (*newp)->node = src;
        links_add(newdir->sb, dst, dst_dir ? -(int)dst->nlink : -1);
        if (dst_dir)
            dir_links_add(newdir, -1);
        node_of(newdir)->mtime = newdir->mtime = vfs_now();
    } else {
        int r = entry_add(newdir, newname, newlen, src);
        if (r < 0)
            return r;
    }
    /* The new entry may have been appended to the same list. */
    oldp = entry_find(olddir, oldname, oldlen);
    entry_remove(olddir, oldp);
    if (src_dir && olddir != newdir) {
        src->parent = node_of(newdir);
        dir_links_add(olddir, -1);
        dir_links_add(newdir, 1);
    }
    return 0;
}

/* Free the pages from index first on, and set the size. */
static int truncate_locked(struct inode *ino, uint64_t size)
{
    struct tmpfs_node *n = node_of(ino);
    size_t remain = (size_t)((size + PAGE_SIZE - 1) / PAGE_SIZE);
    long freed = 0;
    for (size_t i = remain; i < n->npages; i++)
        if (n->pages[i]) {
            page_release(n->pages[i]);
            n->pages[i] = NULL;
            freed++;
        }
    /* The rest of the last page reads as zeros if the file grows again. */
    if (size % PAGE_SIZE && remain - 1 < n->npages && n->pages[remain - 1])
        memset(n->pages[remain - 1] + size % PAGE_SIZE, 0, PAGE_SIZE - size % PAGE_SIZE);
    pages_charge(tsb_of(ino->sb), -freed);
    n->size = ino->size = size;
    n->mtime = ino->mtime = vfs_now();
    return 0;
}

static int tmpfs_truncate(struct inode *ino, uint64_t size)
{
    return truncate_locked(ino, size);
}

static int tmpfs_setmtime(struct inode *ino, int64_t mtime)
{
    node_of(ino)->mtime = ino->mtime = mtime;
    return 0;
}

static int tmpfs_setattr(struct inode *ino, uint32_t mode, uint32_t uid, uint32_t gid)
{
    struct tmpfs_node *n = node_of(ino);
    n->mode = (n->mode & S_IFMT) | mode;
    n->uid = uid;
    n->gid = gid;
    publish(ino, n);
    return 0;
}

static long tmpfs_file_read(struct file *f, char *buf, size_t count, uint64_t *pos)
{
    struct inode *ino = f->inode;
    mutex_lock(&ino->lock);
    struct tmpfs_node *n = node_of(ino);
    size_t done = 0;
    while (done < count && *pos + done < n->size) {
        uint64_t at = *pos + done;
        size_t idx = (size_t)(at / PAGE_SIZE), off = (size_t)(at % PAGE_SIZE);
        size_t chunk = MIN(MIN(count - done, PAGE_SIZE - off), (size_t)(n->size - at));
        if (idx < n->npages && n->pages[idx])
            memcpy(buf + done, n->pages[idx] + off, chunk);
        else
            memset(buf + done, 0, chunk);
        done += chunk;
    }
    mutex_unlock(&ino->lock);
    *pos += done;
    return (long)done;
}

/* Grow the page array to a capacity of count pages. */
static int pages_reserve(struct tmpfs_node *n, size_t count)
{
    if (count <= n->npages)
        return 0;
    size_t cap = n->npages ? n->npages : 4;
    while (cap < count)
        cap *= 2;
    uint8_t **grown = kzalloc(cap * sizeof *grown);
    if (!grown)
        return -ENOMEM;
    if (n->pages)
        memcpy(grown, n->pages, n->npages * sizeof *grown);
    kfree(n->pages);
    n->pages = grown;
    n->npages = cap;
    return 0;
}

static long tmpfs_file_write(struct file *f, const char *buf, size_t count, uint64_t *pos)
{
    struct inode *ino = f->inode;
    struct tmpfs_sb *t = tsb_of(ino->sb);
    mutex_lock(&ino->lock);
    struct tmpfs_node *n = node_of(ino);
    size_t done = 0;
    int r = pages_reserve(n, (size_t)((*pos + count + PAGE_SIZE - 1) / PAGE_SIZE));
    while (r == 0 && done < count) {
        uint64_t at = *pos + done;
        size_t idx = (size_t)(at / PAGE_SIZE), off = (size_t)(at % PAGE_SIZE);
        size_t chunk = MIN(count - done, PAGE_SIZE - off);
        if (!n->pages[idx]) {
            if ((r = pages_charge(t, 1)) < 0)
                break;
            if (!(n->pages[idx] = page_new())) {
                pages_charge(t, -1);
                r = -ENOMEM;
                break;
            }
        }
        memcpy(n->pages[idx] + off, buf + done, chunk);
        done += chunk;
    }
    if (done) {
        if (*pos + done > n->size)
            n->size = ino->size = *pos + done;
        n->mtime = ino->mtime = vfs_now();
    }
    mutex_unlock(&ino->lock);
    *pos += done;
    return done ? (long)done : r;
}

/* "." and "..", then the entries in the order of their creation. */
static long tmpfs_getdents(struct file *f, struct dirent *buf, size_t count)
{
    struct inode *dir = f->inode;
    size_t max = count / sizeof(struct dirent), filled = 0;
    mutex_lock(&dir->lock);
    struct tmpfs_node *n = node_of(dir);
    while (filled < max) {
        struct tmpfs_node *child;
        const char *name;
        size_t len;
        if (f->pos == 0) {
            child = n, name = ".", len = 1;
        } else if (f->pos == 1) {
            child = n->parent, name = "..", len = 2;
        } else {
            struct tmpfs_dirent *e = n->entries;
            for (uint64_t i = 2; e && i < f->pos; i++)
                e = e->next;
            if (!e)
                break;
            child = e->node, name = e->name, len = e->len;
        }
        buf[filled].d_ino = child->ino;
        buf[filled].d_type = vfs_mode_to_dtype(child->mode);
        memcpy(buf[filled].d_name, name, len);
        buf[filled].d_name[len] = '\0';
        filled++;
        f->pos++;
    }
    mutex_unlock(&dir->lock);
    return (long)(filled * sizeof(struct dirent));
}

static int tmpfs_statfs(struct superblock *sb, struct fs_space *space)
{
    struct tmpfs_sb *t = tsb_of(sb);
    spin_lock(&t->lock);
    space->blocks = t->max_pages;
    space->free_blocks = t->max_pages - t->pages;
    spin_unlock(&t->lock);
    space->block_size = PAGE_SIZE;
    return 0;
}

/* The VFS unmounts a tmpfs only when none of its inodes is referenced.
 * The nodes are freed then, every one reached from the hash table, and
 * the superblock with them, as every filesystem frees its own. */
static void tmpfs_unmount(struct superblock *sb)
{
    struct tmpfs_sb *t = tsb_of(sb);
    for (size_t i = 0; i < TMPFS_HASH; i++)
        while (t->hash[i]) {
            struct tmpfs_node *n = t->hash[i];
            t->hash[i] = n->hash_next;
            node_free(t, n);
        }
    kfree(t);
    kfree(sb);
}

static const struct inode_ops tmpfs_dir_ops = {
    .lookup = tmpfs_lookup,
    .create = tmpfs_create,
    .mkdir = tmpfs_mkdir,
    .unlink = tmpfs_unlink,
    .rmdir = tmpfs_rmdir,
    .link = tmpfs_link,
    .symlink = tmpfs_symlink,
    .rename = tmpfs_rename,
    .setmtime = tmpfs_setmtime,
    .setattr = tmpfs_setattr,
};

static const struct inode_ops tmpfs_file_ops = {
    .truncate = tmpfs_truncate,
    .setmtime = tmpfs_setmtime,
    .setattr = tmpfs_setattr,
};

static const struct inode_ops tmpfs_link_ops = {
    .readlink = tmpfs_readlink,
    .setmtime = tmpfs_setmtime,
    .setattr = tmpfs_setattr,
};

static const struct file_ops tmpfs_dir_fops = { .getdents = tmpfs_getdents };
static const struct file_ops tmpfs_file_fops = { .read = tmpfs_file_read, .write = tmpfs_file_write };

static const struct sb_ops tmpfs_sb_ops = {
    .read_inode = tmpfs_read_inode,
    .put_inode = tmpfs_put_inode,
    .unmount = tmpfs_unmount,
    .statfs = tmpfs_statfs,
};

/* The options size=MIB, mode=OOOO, uid=N and gid=N, separated by commas.
 * The size defaults to a quarter of the memory and the root directory to
 * mode 1777, owned by root. */
static int parse_options(const char *options, uint64_t *max_pages, uint32_t *mode, uint32_t *uid, uint32_t *gid)
{
    for (const char *p = options; *p;) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        uint32_t mib = 0;
        int r = vfs_option_uint(p, len, "size", 10, &mib);
        if (r == 1)
            *max_pages = (uint64_t)mib * (1024 * 1024 / PAGE_SIZE);
        if (r == 0)
            r = vfs_option_uint(p, len, "mode", 8, mode);
        if (r == 0)
            r = vfs_option_uint(p, len, "uid", 10, uid);
        if (r == 0)
            r = vfs_option_uint(p, len, "gid", 10, gid);
        if (r <= 0 || *mode > 07777 || *max_pages == 0)
            return -EINVAL;
        p += len + (comma ? 1 : 0);
    }
    return 0;
}

static int tmpfs_mount(const struct fs_type *type, const char *source, const char *options,
                       struct superblock **out)
{
    static uint64_t instances;
    struct pmm_stats st;
    pmm_get_stats(&st);
    uint64_t max_pages = st.total_pages / 4;
    uint32_t mode = 01777, uid = 0, gid = 0;
    if (parse_options(options, &max_pages, &mode, &uid, &gid) < 0)
        return -EINVAL;
    struct tmpfs_sb *t = kzalloc(sizeof *t);
    struct tmpfs_node *root = kzalloc(sizeof *root);
    struct superblock *sb = t && root ? sb_alloc(type, &tmpfs_sb_ops) : NULL;
    if (!sb) {
        kfree(t);
        kfree(root);
        return -ENOMEM;
    }
    spinlock_init(&t->lock, "tmpfs");
    t->max_pages = max_pages;
    t->next_ino = TMPFS_ROOT_INO + 1;
    root->ino = TMPFS_ROOT_INO;
    root->mode = S_IFDIR | mode;
    root->nlink = 2;
    root->uid = uid;
    root->gid = gid;
    root->mtime = vfs_now();
    root->parent = root;
    t->hash[TMPFS_ROOT_INO % TMPFS_HASH] = root;
    sb->priv = t;
    sb->root_ino = TMPFS_ROOT_INO;
    sb->dev = 0x746d7000 + __atomic_add_fetch(&instances, 1, __ATOMIC_RELAXED);
    klog_info("%s: %lu KiB at most", source, max_pages * (PAGE_SIZE / 1024));
    *out = sb;
    return 0;
}

static struct fs_type tmpfs_type = {
    .name = "tmpfs",
    .mount = tmpfs_mount,
};

void tmpfs_init(void)
{
    vfs_register_fs(&tmpfs_type);
}
