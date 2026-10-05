#pragma once
/* The 9P2000.L client (V5 of docs/plan/release-0.6.0.md, docs/design/9p.md).
 * client.c builds and parses the messages and sends them through the
 * virtio-9p transport. 9p.c connects the client to the VFS. */
#include <kernel.h>
#include <fs/vfs.h>
#include <sync/spinlock.h>

struct p9_channel;

#define P9_NOFID 0xffffffffu
#define P9_MSIZE (64 * 1024)
/* The longest data part of one Tread or Twrite: the message size without
 * the header of Twrite (23 bytes), rounded down to whole pages. */
#define P9_IO_MAX ((P9_MSIZE - 24) & ~(PAGE_SIZE - 1))
#define P9_MAX_FIDS 4096

/* The flags of Tunlinkat. */
#define P9_AT_REMOVEDIR 0x200
/* The fields of Tsetattr. */
#define P9_SETATTR_MODE      0x1
#define P9_SETATTR_UID       0x2
#define P9_SETATTR_GID       0x4
#define P9_SETATTR_SIZE      0x8
#define P9_SETATTR_MTIME     0x20
#define P9_SETATTR_MTIME_SET 0x100

struct p9_qid {
    uint8_t type;
    uint32_t version;
    uint64_t path;
};

/* The attributes of Rgetattr that the VFS uses. */
struct p9_attr {
    struct p9_qid qid;
    uint32_t mode, uid, gid;
    uint64_t nlink, rdev, size;
    int64_t mtime;                  /* nanoseconds since the epoch */
};

struct p9_setattr {
    uint32_t valid;                 /* P9_SETATTR_* */
    uint32_t mode, uid, gid;
    uint64_t size;
    int64_t mtime;                  /* nanoseconds since the epoch */
};

/* One connection to a share. fid_lock protects fids. */
struct p9_client {
    struct p9_channel *ch;
    uint32_t msize;
    struct spinlock fid_lock;
    uint64_t fids[P9_MAX_FIDS / 64];  /* bit per fid in use */
};

int p9_connect(struct p9_client *c, struct p9_channel *ch);
/* A free fid, or P9_NOFID when every fid is in use. */
uint32_t p9_fid_alloc(struct p9_client *c);
/* Return a fid that the server does not know, after a failed walk. */
void p9_fid_free(struct p9_client *c, uint32_t fid);

int p9_attach(struct p9_client *c, uint32_t fid, uint32_t uid, struct p9_qid *qid);
/* Walk from fid to the name (len bytes) as newfid, or clone fid when name
 * is NULL. The server knows newfid only after a successful walk. */
int p9_walk(struct p9_client *c, uint32_t fid, uint32_t newfid, const char *name, size_t len, struct p9_qid *qid);
int p9_getattr(struct p9_client *c, uint32_t fid, struct p9_attr *a);
int p9_setattr(struct p9_client *c, uint32_t fid, const struct p9_setattr *s);
int p9_lopen(struct p9_client *c, uint32_t fid, uint32_t flags);
/* Create the file name in the directory of fid. fid then refers to the
 * new file, opened with flags. */
int p9_lcreate(struct p9_client *c, uint32_t fid, const char *name, size_t len, uint32_t flags, uint32_t mode,
               uint32_t gid);
/* One Tread or Twrite of at most P9_IO_MAX bytes. Returns the count. */
long p9_read(struct p9_client *c, uint32_t fid, uint64_t offset, void *buf, size_t n);
long p9_write(struct p9_client *c, uint32_t fid, uint64_t offset, const void *buf, size_t n);
/* Tclunk, then the fid is free, also when the server reports an error. */
int p9_clunk(struct p9_client *c, uint32_t fid);
/* One Treaddir. Copies the entries (at most n bytes) to buf and returns
 * their length. */
long p9_readdir(struct p9_client *c, uint32_t fid, uint64_t offset, void *buf, size_t n);
int p9_mkdir(struct p9_client *c, uint32_t dfid, const char *name, size_t len, uint32_t mode, uint32_t gid);
int p9_unlinkat(struct p9_client *c, uint32_t dfid, const char *name, size_t len, uint32_t flags);
int p9_renameat(struct p9_client *c, uint32_t olddfid, const char *oldname, size_t oldlen, uint32_t newdfid,
                const char *newname, size_t newlen);
int p9_symlink(struct p9_client *c, uint32_t dfid, const char *name, size_t len, const char *target, size_t tlen,
               uint32_t gid);
int p9_link(struct p9_client *c, uint32_t dfid, uint32_t fid, const char *name, size_t len);
/* Copies the target of the link to buf, at most size bytes without a
 * NUL, and returns its length. */
int p9_readlink(struct p9_client *c, uint32_t fid, char *buf, size_t size);
int p9_statfs(struct p9_client *c, uint32_t fid, struct fs_space *space);
int p9_fsync(struct p9_client *c, uint32_t fid);

/* An entry of Rreaddir: the qid, the offset of the next entry, the type
 * (DT_*) and the name. Returns the bytes of the entry, or 0 when buf
 * (len bytes) contains no complete entry. */
size_t p9_dirent_parse(const uint8_t *buf, size_t len, struct p9_qid *qid, uint64_t *next, uint8_t *type,
                       const char **name, size_t *namelen);
