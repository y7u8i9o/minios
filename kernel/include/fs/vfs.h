#pragma once
#include <kernel.h>
#include <lib/list.h>
#include <sync/spinlock.h>
#include <sync/mutex.h>
#include <minios/abi.h>

struct inode;
struct file;
struct superblock;
struct blockdev;
struct vmspace;

#define VFS_PATH_MAX 256

/* Operations on directory and file inodes. name is not NUL terminated,
 * len is its length. The caller holds dir->lock for directory operations.
 * Lookups return a referenced inode. */
struct inode_ops {
    int (*lookup)(struct inode *dir, const char *name, size_t len, struct inode **out);
    int (*create)(struct inode *dir, const char *name, size_t len, uint32_t mode, struct inode **out);
    int (*mkdir)(struct inode *dir, const char *name, size_t len);
    int (*unlink)(struct inode *dir, const char *name, size_t len);
    int (*rmdir)(struct inode *dir, const char *name, size_t len);
    int (*link)(struct inode *dir, const char *name, size_t len, struct inode *target);
    /* Both directory locks are held, olddir first when they differ. */
    int (*rename)(struct inode *olddir, const char *oldname, size_t oldlen,
                  struct inode *newdir, const char *newname, size_t newlen);
    int (*truncate)(struct inode *ino, uint64_t size);
};

/* Operations on open files. read and write receive the position to use and
 * update it. getdents fills whole records starting at the entry index in
 * f->pos and advances it. */
struct file_ops {
    int (*open)(struct inode *ino, struct file *f);
    void (*release)(struct file *f);
    long (*read)(struct file *f, char *buf, size_t n, uint64_t *pos);
    long (*write)(struct file *f, const char *buf, size_t n, uint64_t *pos);
    long (*getdents)(struct file *f, struct dirent *buf, size_t count);
    /* NULL means the generic lseek on inode size. */
    long (*lseek)(struct file *f, long off, int whence);
    /* Device control; NULL means ENOTTY. arg is a checked user pointer or
     * a plain value depending on the request. */
    long (*ioctl)(struct file *f, unsigned long req, uintptr_t arg);
    /* Map the file into vm; returns the address or -errno. NULL means
     * ENODEV. flags are VM_READ/VM_WRITE from prot. */
    long (*mmap)(struct file *f, struct vmspace *vm, uintptr_t hint, size_t len, unsigned flags,
                 uint64_t off);
    /* Readiness bits (POLLIN, POLLOUT). NULL means always ready. */
    int (*poll)(struct file *f);
    int (*truncate)(struct file *f, uint64_t size);   /* M23: memfd */
};

/* An in memory inode. refcount and link are protected by sb->lock. size,
 * nlink and the contents are protected by lock, a mutex because filesystem
 * operations may sleep on a block device. The remaining fields are set when
 * the inode is read and do not change. */
struct inode {
    struct superblock *sb;
    uint64_t ino;
    uint32_t mode;
    uint32_t nlink;
    uint64_t size;
    uint64_t rdev;
    const struct inode_ops *ops;
    const struct file_ops *fops;
    void *priv;                     /* filesystem private */
    int refcount;
    struct mutex lock;
    struct list_head link;          /* sb->inodes */
};

/* Superblock operations. read_inode fills a freshly allocated inode for
 * ino. put_inode is called under no lock when the last reference goes away
 * and may free filesystem resources of an unlinked inode. free_inode
 * releases what read_inode attached to an inode that is discarded without
 * ever being published, because another CPU read the same inode first;
 * optional. */
struct sb_ops {
    int (*read_inode)(struct superblock *sb, uint64_t ino, struct inode *ino_out);
    void (*put_inode)(struct inode *ino);
    void (*free_inode)(struct inode *ino);
    int (*sync)(struct superblock *sb);
    void (*unmount)(struct superblock *sb);
};

/* One mounted filesystem instance. lock protects inodes and the refcounts
 * of the inodes on it. */
struct superblock {
    const struct fs_type *type;
    const struct sb_ops *ops;
    uint64_t root_ino;
    uint64_t dev;
    void *priv;
    struct spinlock lock;
    struct list_head inodes;        /* cached inodes, all referenced */
};

/* A filesystem type. mount builds a superblock from a source string. */
struct fs_type {
    const char *name;
    int (*mount)(const struct fs_type *type, const char *source, struct superblock **out);
    struct list_head link;          /* fs_types, fs_types_lock */
};

/* A mount. The covered directory is identified by (sb, ino) of the
 * mountpoint so it can be recognized without keeping the inode alive.
 * Protected by mount_lock. */
struct mount {
    struct superblock *sb;
    struct superblock *parent_sb;   /* NULL for the root mount */
    uint64_t parent_ino;
    char path[VFS_PATH_MAX];
    struct list_head link;
};

/* An open file description. refcount is protected by files_lock. pos is
 * protected by lock. */
struct file {
    struct inode *inode;
    const struct file_ops *ops;
    uint64_t pos;
    int flags;                      /* O_* */
    int refcount;
    struct mutex lock;
    void *priv;
};

void vfs_init(void);
int vfs_register_fs(struct fs_type *type);

/* Inode cache. inode_get returns a referenced inode for ino on sb, reading
 * it through sb->ops->read_inode when it is not cached. */
struct inode *inode_get(struct superblock *sb, uint64_t ino);
void inode_ref(struct inode *ino);
void inode_put(struct inode *ino);
/* Allocate a superblock with initialized lists and lock. */
struct superblock *sb_alloc(const struct fs_type *type, const struct sb_ops *ops);

/* Mount table. */
int vfs_mount(const char *fstype, const char *source, const char *target);
int vfs_umount(const char *target);
int vfs_sync(void);
/* Sync everything and unmount every filesystem that is not busy, most
 * recent first. Used by shutdown. Returns the number of busy mounts. */
int vfs_umount_all(void);

/* Build the canonical absolute form of path relative to cwd into out:
 * "." and ".." are folded, duplicate slashes removed, no trailing slash. */
int vfs_canonicalize(const char *cwd, const char *path, char *out, size_t size);
/* Resolve a path (relative paths use the current process cwd) to a
 * referenced inode. Mount points are crossed downwards. */
int vfs_lookup(const char *path, struct inode **out);
/* Resolve everything but the last component, which is copied to name.
 * Fails with -EINVAL for "/" and for "." or ".." as last component. */
int vfs_lookup_parent(const char *path, struct inode **dir, char *name, size_t namesize);

/* File level API used by the system calls. */
int vfs_open(const char *path, int flags, uint32_t mode, struct file **out);
struct file *file_alloc(struct inode *ino, const struct file_ops *ops, int flags);
void file_ref(struct file *f);
void file_put(struct file *f);
long file_read(struct file *f, char *buf, size_t n);
long file_write(struct file *f, const char *buf, size_t n);
long file_lseek(struct file *f, long off, int whence);
long file_getdents(struct file *f, struct dirent *buf, size_t count);
void inode_stat(struct inode *ino, struct stat *st);

int vfs_mkdir(const char *path);
int vfs_unlink(const char *path);
int vfs_rmdir(const char *path);
int vfs_rename(const char *oldpath, const char *newpath);
int vfs_link(const char *oldpath, const char *newpath);

/* Generic helpers for filesystems. */
long vfs_generic_lseek(struct file *f, long off, int whence);
uint8_t vfs_mode_to_dtype(uint32_t mode);
