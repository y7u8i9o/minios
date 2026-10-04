#pragma once
#include <kernel.h>
#include <lib/list.h>
#include <sync/spinlock.h>
#include <sync/mutex.h>
#include <sync/atomic.h>
#include <sync/rcu.h>
#include <minios/abi.h>

struct inode;
struct file;
struct superblock;
struct blockdev;
struct vmspace;
struct mapping;
struct poll_source;

#define VFS_PATH_MAX 256
/* The longest symbolic link target: like every path the kernel accepts, a
 * target contains at most VFS_PATH_MAX - 1 bytes. */
#define VFS_SYMLINK_MAX (VFS_PATH_MAX - 1)

/* Operations on directory and file inodes. name is not NUL terminated,
 * len is its length. The caller has acquired dir->lock for directory operations.
 * Lookups return a referenced inode. */
struct inode_ops {
    int (*lookup)(struct inode *dir, const char *name, size_t len, struct inode **out);
    /* create, mkdir and symlink give the new inode the owner chosen by
     * vfs_new_owner. mode contains the permission bits after the umask. */
    int (*create)(struct inode *dir, const char *name, size_t len, uint32_t mode, struct inode **out);
    int (*mkdir)(struct inode *dir, const char *name, size_t len, uint32_t mode);
    int (*unlink)(struct inode *dir, const char *name, size_t len);
    int (*rmdir)(struct inode *dir, const char *name, size_t len);
    int (*link)(struct inode *dir, const char *name, size_t len, struct inode *target);
    /* Create name as a symbolic link containing target (tlen bytes, 1 to
     * VFS_SYMLINK_MAX, not NUL terminated). A filesystem that cannot store
     * links returns -EPERM; without the operation the VFS reports EROFS. */
    int (*symlink)(struct inode *dir, const char *name, size_t len, const char *target, size_t tlen);
    /* Copy the target of the symbolic link ino into buf, at most size
     * bytes and without a NUL; returns the length. ino->lock is locked. */
    int (*readlink)(struct inode *ino, char *buf, size_t size);
    /* Both directory locks are locked, olddir first when they differ. */
    int (*rename)(struct inode *olddir, const char *oldname, size_t oldlen,
                  struct inode *newdir, const char *newname, size_t newlen);
    int (*truncate)(struct inode *ino, uint64_t size);
    /* Set the modification time and write the inode back. ino->lock is locked
     * and the call is bracketed by op_begin and op_end. Optional: a
     * filesystem without it refuses utimensat with EROFS. */
    int (*setmtime)(struct inode *ino, int64_t mtime);
    /* Set the permission bits (07777) and the owner and write the inode
     * back (U1). ino->lock is locked and the call is bracketed by op_begin
     * and op_end. A filesystem that cannot store a change returns -EPERM,
     * one without the operation makes chmod and chown fail with EROFS. */
    int (*setattr)(struct inode *ino, uint32_t mode, uint32_t uid, uint32_t gid);
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
    struct poll_source *(*poll_source)(struct file *f);
    int (*truncate)(struct file *f, uint64_t size);   /* M23: memfd */
    /* FOPS_STREAM: the object has no position, so file_read and
     * file_write call read and write without file.lock and a reader
     * blocked in read does not exclude a writer on the same open file
     * description (N01, sockets). pos is a scratch word for such calls. */
    unsigned flags;
};
#define FOPS_STREAM 1

/* An in memory inode. refcount and link are protected by sb->lock. size,
 * nlink, the permission bits of mode, uid, gid and the contents are
 * protected by lock, a mutex because filesystem operations may sleep on a
 * block device. The file type bits of mode never change and may be read
 * without the lock, and a permission check may read the other fields
 * without it, since each is one word. The remaining fields are set when
 * the inode is read and do not change. */
struct inode {
    struct superblock *sb;
    uint64_t ino;
    uint32_t mode;
    uint32_t nlink;
    uint32_t uid, gid;              /* owner (U1) */
    uint64_t size;
    uint64_t rdev;
    int64_t mtime;                  /* nanoseconds since the epoch; lock */
    const struct inode_ops *ops;
    const struct file_ops *fops;
    void *priv;                     /* filesystem private */
    int refcount;
    struct mutex lock;
    struct list_head link;          /* sb->inodes */
    struct mapping *mapping;        /* mapped pages of the file, filemap_lock (M37) */
};

/* Superblock operations. read_inode fills a freshly allocated inode for
 * ino. put_inode is called under no lock when the last reference goes away
 * and may free filesystem resources of an unlinked inode. free_inode
 * releases what read_inode attached to an inode that is discarded without
 * ever being published, because another CPU read the same inode first;
 * optional. op_begin and op_end (optional, M36) bracket every modifying
 * operation the VFS issues (create, write, truncate, mkdir, unlink, rmdir,
 * link, rename); they are called with no inode or file lock acquired, so a
 * journaling filesystem may block in op_begin for log space. */
struct fs_space {
    uint64_t blocks, free_blocks;
    uint32_t block_size;
};

struct sb_ops {
    int (*read_inode)(struct superblock *sb, uint64_t ino, struct inode *ino_out);
    void (*put_inode)(struct inode *ino);
    void (*free_inode)(struct inode *ino);
    int (*sync)(struct superblock *sb);
    void (*unmount)(struct superblock *sb);
    void (*op_begin)(struct superblock *sb);
    void (*op_end)(struct superblock *sb);
    int (*statfs)(struct superblock *sb, struct fs_space *space);
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

/* A filesystem type. mount builds a superblock from a source string and
 * a comma separated option string, which is empty when none was given. A
 * filesystem rejects options it does not know with -EINVAL. */
struct fs_type {
    const char *name;
    int (*mount)(const struct fs_type *type, const char *source, const char *options,
                 struct superblock **out);
    struct list_head link;          /* fs_types, fs_types_lock */
};

/* A mount. The covered directory is identified by (sb, ino) of the
 * mountpoint so it can be recognized without retaining the inode.
 * Protected by mount_lock. */
struct mount {
    struct superblock *sb;
    struct superblock *parent_sb;   /* NULL for the root mount */
    uint64_t parent_ino;
    char path[VFS_PATH_MAX];
    char source[64];              /* the source string of the mount, such as vda3 */
    unsigned readers;             /* statfs snapshots pin this mount */
    struct list_head link;
};

/* An open file description. refcount is atomic.  The storage remains alive
 * for an RCU grace period after the final reference; pos is protected by
 * lock. */
struct file {
    struct inode *inode;
    const struct file_ops *ops;
    uint64_t pos;
    int flags;                      /* O_* */
    refcount_t refcount;
    struct mutex lock;
    void *priv;
    char *path;                     /* canonical path of a directory or regular file
                                       opened by name, for the *at system calls and
                                       /dev/maps; NULL otherwise. Set at open, freed
                                       with the file. */
    struct rcu_head rcu;
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
int vfs_mount(const char *fstype, const char *source, const char *target, const char *options);
int vfs_umount(const char *target);
int vfs_sync(void);
/* Text records: path type total-blocks free-blocks block-size. */
long vfs_format_mounts(char *buf, size_t size);
/* One mounted filesystem as vfs_for_each_mount reports it. The strings are
 * valid during the call. */
struct mount_info {
    const char *path, *source, *type;
    struct fs_space space;
};
/* Call fn for every mount, in mount order, with its space from statfs.
 * The mounts are pinned for the walk and fn runs with no lock acquired.
 * Returns 0, or -ENOMEM when the snapshot cannot be allocated. */
int vfs_for_each_mount(void (*fn)(const struct mount_info *m, void *arg), void *arg);
/* Sync everything and unmount every filesystem that is not busy, most
 * recent first. Used by shutdown. Returns the number of busy mounts. */
int vfs_umount_all(void);

/* Build the canonical absolute form of path relative to cwd into out:
 * "." and ".." are folded, duplicate slashes removed, no trailing slash. */
int vfs_canonicalize(const char *cwd, const char *path, char *out, size_t size);
/* Resolve a path (relative paths use the current process cwd) to a
 * referenced inode. Mount points are crossed downwards, symbolic links are
 * followed in every component (SYMLOOP_MAX in total) and ".." leaves the
 * directory that was actually reached, not the name that led there. */
int vfs_lookup(const char *path, struct inode **out);
/* vfs_lookup with flags: VFS_NOFOLLOW returns a symbolic link in the last
 * component itself. phys, when not NULL, receives the canonical path of
 * the result without symbolic links (physsize bytes, VFS_PATH_MAX is
 * enough). */
#define VFS_NOFOLLOW 1
/* vfs_access checks with the effective instead of the real ids. */
#define VFS_EACCESS  4
/* vfs_utimens sets the current time, which write permission allows. */
#define VFS_UTIME_NOW 8
int vfs_lookup_path(const char *path, unsigned flags, struct inode **out, char *phys, size_t physsize);
/* Resolve everything but the last component, which is copied to name and
 * is never followed. Fails with -EINVAL for "/" and for "." or ".." as
 * last component. */
int vfs_lookup_parent(const char *path, struct inode **dir, char *name, size_t namesize);

/* Permission checks (U2). mask combines MAY_READ, MAY_WRITE and MAY_EXEC.
 * The owner class of the mode applies to the owner, the group class to
 * members of the file's group and the other class to everyone else. Root
 * passes every check, except that executing a file that is not a
 * directory needs one execute bit. Returns 0 or -EACCES. */
#define MAY_EXEC  1
#define MAY_WRITE 2
#define MAY_READ  4
struct cred;
int vfs_permission(struct inode *ino, int mask, const struct cred *c);
/* access(2) with the real ids, or the effective ones with VFS_EACCESS. */
int vfs_access(const char *path, int mask, unsigned flags);

/* File level API used by the system calls. Opening checks read and write
 * permission against the access mode, except for a file that the call
 * itself created. */
int vfs_open(const char *path, int flags, uint32_t mode, struct file **out);
/* Set the size of a regular file, inside a filesystem operation and under
 * inode.lock, and drop the cached pages past the new end. Growing leaves
 * a hole that reads as zeros. Used by O_TRUNC and ftruncate (U5). */
int vfs_truncate(struct inode *ino, uint64_t size);
/* Open a program for exec: a regular file with execute permission, which
 * need not be readable. */
int vfs_open_exec(const char *path, struct file **out);
struct file *file_alloc(struct inode *ino, const struct file_ops *ops, int flags);
void file_ref(struct file *f);
void file_put(struct file *f);
long file_read(struct file *f, char *buf, size_t n);
long file_write(struct file *f, const char *buf, size_t n);
long file_lseek(struct file *f, long off, int whence);
long file_getdents(struct file *f, struct dirent *buf, size_t count);
/* Seconds since the Unix epoch from the real time clock, for time stamps. */
/* The current time in nanoseconds since the epoch, the unit of inode mtime. */
int64_t vfs_now(void);
void inode_stat(struct inode *ino, struct stat *st);

/* mode is masked by the umask of the calling process. */
int vfs_mkdir(const char *path, uint32_t mode);
int vfs_unlink(const char *path);
/* Set the modification time of the file at path to mtime (nanoseconds).
 * flags combines VFS_NOFOLLOW and VFS_UTIME_NOW. The owner and root may
 * set any time, and anyone with write permission the current one. */
int vfs_utimens(const char *path, int64_t mtime, unsigned flags);
int vfs_rmdir(const char *path);
int vfs_rename(const char *oldpath, const char *newpath);
int vfs_link(const char *oldpath, const char *newpath);
/* Create path as a symbolic link containing target. */
int vfs_symlink(const char *target, const char *path);
/* Change the permission bits (mode, 07777) or the owner of an inode or of
 * the file at path (U1). VFS_CHOWN_UNCHANGED leaves the uid or gid as it is.
 * Only the owner and root may change the mode, only root the owner, and
 * the owner may change the group to one of its own groups. flags is 0 or
 * VFS_NOFOLLOW. */
#define VFS_CHOWN_UNCHANGED 0xffffffffu
int vfs_chmod_inode(struct inode *ino, uint32_t mode);
int vfs_chown_inode(struct inode *ino, uint32_t uid, uint32_t gid);
int vfs_chmod(const char *path, uint32_t mode, unsigned flags);
int vfs_chown(const char *path, uint32_t uid, uint32_t gid, unsigned flags);
/* The owner of an inode created in dir by the calling process: its
 * effective uid, and its effective gid unless dir has the setgid bit, in
 * which case dir's group. */
void vfs_new_owner(struct inode *dir, uint32_t *uid, uint32_t *gid);
/* The umask of the calling process. */
uint32_t vfs_umask(void);
/* Match the mount option token opt (len bytes, no comma) against
 * "name=value" with value a number in base 8 or 10. Returns 1 and stores
 * the value on a match, 0 when the name differs, -EINVAL for a bad value. */
int vfs_option_uint(const char *opt, size_t len, const char *name, unsigned base, uint32_t *out);
/* Copy the target of the symbolic link at path into the kernel buffer buf
 * (size bytes, no NUL added); returns the length, -EINVAL when path is
 * not a link. */
int vfs_readlink(const char *path, char *buf, size_t size);

/* Generic helpers for filesystems. */
long vfs_generic_lseek(struct file *f, long off, int whence);
/* Call the superblock's op_begin / op_end hooks when it has them. */
void vfs_op_begin(struct superblock *sb);
void vfs_op_end(struct superblock *sb);
uint8_t vfs_mode_to_dtype(uint32_t mode);
