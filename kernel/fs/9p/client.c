/* The messages of 9P2000.L (V5 of docs/plan/release-0.6.0.md,
 * docs/design/9p.md). A message is a header of size[4] type[1] tag[2] and
 * the fields of its type in little endian order. A string is a length[2]
 * and the bytes without a NUL. A qid is type[1] version[4] path[8]. The
 * transport writes the tag. An error answer is Rlerror with a Linux errno
 * value, which the numbers of minios follow. */
#define KLOG_SUBSYS "9p"
#include "9p.h"
#include <drivers/virtio/virtio_9p.h>
#include <lib/string.h>
#include <mm/slab.h>
#include <klog.h>
#include <errno.h>

enum {
    P9_RLERROR = 7,
    P9_TSTATFS = 8,
    P9_TLOPEN = 12,
    P9_TLCREATE = 14,
    P9_TSYMLINK = 16,
    P9_TREADLINK = 22,
    P9_TGETATTR = 24,
    P9_TSETATTR = 26,
    P9_TREADDIR = 40,
    P9_TFSYNC = 50,
    P9_TLINK = 70,
    P9_TMKDIR = 72,
    P9_TRENAMEAT = 74,
    P9_TUNLINKAT = 76,
    P9_TVERSION = 100,
    P9_TATTACH = 104,
    P9_TWALK = 110,
    P9_TREAD = 116,
    P9_TWRITE = 118,
    P9_TCLUNK = 120,
};

#define P9_NOTAG 0xffff
#define P9_HEADER 7
#define P9_GETATTR_BASIC 0x7ffULL
#define QID_SIZE 13

/* A message being built or parsed. A field that does not fit sets bad. */
struct msg {
    uint8_t *data;
    size_t cap, len;
    bool bad;
};

static int msg_new(struct msg *m, size_t cap)
{
    m->data = kmalloc(cap);
    m->cap = cap;
    m->len = 0;
    m->bad = false;
    return m->data ? 0 : -ENOMEM;
}

static void msg_free(struct msg *m)
{
    kfree(m->data);
    m->data = NULL;
}

static void put(struct msg *m, uint64_t v, unsigned bytes)
{
    if (m->len + bytes > m->cap) {
        m->bad = true;
        return;
    }
    for (unsigned i = 0; i < bytes; i++)
        m->data[m->len++] = (uint8_t)(v >> (8 * i));
}

static void put_str(struct msg *m, const char *s, size_t len)
{
    if (len > 0xffff || m->len + 2 + len > m->cap) {
        m->bad = true;
        return;
    }
    put(m, len, 2);
    memcpy(m->data + m->len, s, len);
    m->len += len;
}

/* Starts a message of type. The size and the tag follow in rpc. */
static void begin(struct msg *m, uint8_t type)
{
    m->len = 0;
    put(m, 0, 4);
    put(m, type, 1);
    put(m, P9_NOTAG, 2);
}

static uint64_t get(struct msg *m, unsigned bytes)
{
    if (m->len + bytes > m->cap) {
        m->bad = true;
        return 0;
    }
    uint64_t v = 0;
    for (unsigned i = 0; i < bytes; i++)
        v |= (uint64_t)m->data[m->len++] << (8 * i);
    return v;
}

static void get_qid(struct msg *m, struct p9_qid *q)
{
    q->type = (uint8_t)get(m, 1);
    q->version = (uint32_t)get(m, 4);
    q->path = get(m, 8);
}

/* A Linux errno of Rlerror as a negative errno of minios. Numbers that
 * minios lacks become EIO, and ESTALE an absent file. */
static int map_error(uint32_t e)
{
    switch (e) {
    case EPERM: case ENOENT: case EIO: case ENXIO: case E2BIG: case EBADF: case EAGAIN: case ENOMEM:
    case EACCES: case EFAULT: case EBUSY: case EEXIST: case EXDEV: case ENODEV: case ENOTDIR: case EISDIR:
    case EINVAL: case ENFILE: case EMFILE: case ETXTBSY: case EFBIG: case ENOSPC: case ESPIPE: case EROFS:
    case EMLINK: case ERANGE: case ENAMETOOLONG: case ENOSYS: case ENOTEMPTY: case ELOOP: case EOVERFLOW:
    case EOPNOTSUPP:
        return -(int)e;
    case 116:                       /* ESTALE */
        return -ENOENT;
    case 122:                       /* EDQUOT */
        return -ENOSPC;
    default:
        return -EIO;
    }
}

/* Sends t and receives the answer into r. On success r is positioned
 * after the header of the answer. */
static int rpc(struct p9_client *c, struct msg *t, struct msg *r)
{
    if (t->bad)
        return -ENAMETOOLONG;
    uint8_t type = t->data[4];
    uint32_t size = (uint32_t)t->len;
    for (unsigned i = 0; i < 4; i++)
        t->data[i] = (uint8_t)(size >> (8 * i));
    long n = virtio_9p_request(c->ch, t->data, t->len, r->data, r->cap);
    if (n < 0)
        return (int)n;
    r->len = 0;
    uint32_t rsize = (uint32_t)get(r, 4);
    uint8_t rtype = (uint8_t)get(r, 1);
    get(r, 2);
    if (r->bad || rsize < P9_HEADER || rsize > (size_t)n || rsize > r->cap) {
        klog_error("short answer to message %u", type);
        return -EIO;
    }
    r->cap = rsize;                 /* parsing stops at the end of the answer */
    if (rtype == P9_RLERROR) {
        uint32_t e = (uint32_t)get(r, 4);
        return r->bad ? -EIO : map_error(e);
    }
    if (rtype != type + 1) {
        klog_error("answer %u to message %u", rtype, type);
        return -EIO;
    }
    return 0;
}

/* The buffer size of a message without data. */
#define SMALL 512

int p9_connect(struct p9_client *c, struct p9_channel *ch)
{
    c->ch = ch;
    spinlock_init(&c->fid_lock, "p9_fids");
    struct msg t, r;
    if (msg_new(&t, SMALL) < 0)
        return -ENOMEM;
    if (msg_new(&r, SMALL) < 0) {
        msg_free(&t);
        return -ENOMEM;
    }
    begin(&t, P9_TVERSION);
    put(&t, P9_MSIZE, 4);
    put_str(&t, "9P2000.L", 8);
    int e = rpc(c, &t, &r);
    if (e == 0) {
        uint32_t msize = (uint32_t)get(&r, 4);
        uint16_t vlen = (uint16_t)get(&r, 2);
        bool ok = !r.bad && vlen == 8 && r.len + 8 <= r.cap && memcmp(r.data + r.len, "9P2000.L", 8) == 0;
        if (!ok) {
            klog_error("the server does not speak 9P2000.L");
            e = -EPROTONOSUPPORT;
        } else if (msize < 4096) {
            klog_error("message size %u is too small", msize);
            e = -EIO;
        } else {
            c->msize = msize < P9_MSIZE ? msize : P9_MSIZE;
        }
    }
    msg_free(&t);
    msg_free(&r);
    return e;
}

uint32_t p9_fid_alloc(struct p9_client *c)
{
    spin_lock(&c->fid_lock);
    for (uint32_t w = 0; w < P9_MAX_FIDS / 64; w++) {
        if (c->fids[w] == ~0ULL)
            continue;
        uint32_t bit = (uint32_t)__builtin_ctzll(~c->fids[w]);
        c->fids[w] |= 1ULL << bit;
        spin_unlock(&c->fid_lock);
        return w * 64 + bit;
    }
    spin_unlock(&c->fid_lock);
    return P9_NOFID;
}

void p9_fid_free(struct p9_client *c, uint32_t fid)
{
    if (fid >= P9_MAX_FIDS)
        return;
    spin_lock(&c->fid_lock);
    c->fids[fid / 64] &= ~(1ULL << (fid % 64));
    spin_unlock(&c->fid_lock);
}

/* A request and the buffer of its answer. */
struct pair {
    struct msg t, r;
};

static int pair_new(struct pair *p, size_t tcap, size_t rcap)
{
    if (msg_new(&p->t, tcap) < 0)
        return -ENOMEM;
    if (msg_new(&p->r, rcap) < 0) {
        msg_free(&p->t);
        return -ENOMEM;
    }
    return 0;
}

static void pair_free(struct pair *p)
{
    msg_free(&p->t);
    msg_free(&p->r);
}

int p9_attach(struct p9_client *c, uint32_t fid, uint32_t uid, struct p9_qid *qid)
{
    struct pair p;
    if (pair_new(&p, SMALL, SMALL) < 0)
        return -ENOMEM;
    begin(&p.t, P9_TATTACH);
    put(&p.t, fid, 4);
    put(&p.t, P9_NOFID, 4);
    put_str(&p.t, "", 0);
    put_str(&p.t, "", 0);
    put(&p.t, uid, 4);
    int e = rpc(c, &p.t, &p.r);
    if (e == 0) {
        get_qid(&p.r, qid);
        e = p.r.bad ? -EIO : 0;
    }
    pair_free(&p);
    return e;
}

int p9_walk(struct p9_client *c, uint32_t fid, uint32_t newfid, const char *name, size_t len, struct p9_qid *qid)
{
    struct pair p;
    if (pair_new(&p, SMALL, SMALL) < 0)
        return -ENOMEM;
    begin(&p.t, P9_TWALK);
    put(&p.t, fid, 4);
    put(&p.t, newfid, 4);
    put(&p.t, name ? 1 : 0, 2);
    if (name)
        put_str(&p.t, name, len);
    int e = rpc(c, &p.t, &p.r);
    if (e == 0) {
        uint16_t n = (uint16_t)get(&p.r, 2);
        /* A walk of one name that returns no qid did not reach it, and
         * the server did not create newfid. */
        if (name && n != 1)
            e = -ENOENT;
        else if (name && qid)
            get_qid(&p.r, qid);
        if (e == 0 && p.r.bad)
            e = -EIO;
    }
    pair_free(&p);
    return e;
}

int p9_getattr(struct p9_client *c, uint32_t fid, struct p9_attr *a)
{
    struct pair p;
    if (pair_new(&p, SMALL, SMALL) < 0)
        return -ENOMEM;
    begin(&p.t, P9_TGETATTR);
    put(&p.t, fid, 4);
    put(&p.t, P9_GETATTR_BASIC, 8);
    int e = rpc(c, &p.t, &p.r);
    if (e == 0) {
        get(&p.r, 8);               /* valid */
        get_qid(&p.r, &a->qid);
        a->mode = (uint32_t)get(&p.r, 4);
        a->uid = (uint32_t)get(&p.r, 4);
        a->gid = (uint32_t)get(&p.r, 4);
        a->nlink = get(&p.r, 8);
        a->rdev = get(&p.r, 8);
        a->size = get(&p.r, 8);
        get(&p.r, 8);               /* blksize */
        get(&p.r, 8);               /* blocks */
        get(&p.r, 8);               /* atime */
        get(&p.r, 8);
        uint64_t sec = get(&p.r, 8), nsec = get(&p.r, 8);
        a->mtime = (int64_t)sec * 1000000000 + (int64_t)nsec;
        e = p.r.bad ? -EIO : 0;
    }
    pair_free(&p);
    return e;
}

int p9_setattr(struct p9_client *c, uint32_t fid, const struct p9_setattr *s)
{
    struct pair p;
    if (pair_new(&p, SMALL, SMALL) < 0)
        return -ENOMEM;
    begin(&p.t, P9_TSETATTR);
    put(&p.t, fid, 4);
    put(&p.t, s->valid, 4);
    put(&p.t, s->mode, 4);
    put(&p.t, s->uid, 4);
    put(&p.t, s->gid, 4);
    put(&p.t, s->size, 8);
    put(&p.t, 0, 8);                /* atime */
    put(&p.t, 0, 8);
    put(&p.t, (uint64_t)(s->mtime / 1000000000), 8);
    put(&p.t, (uint64_t)(s->mtime % 1000000000), 8);
    int e = rpc(c, &p.t, &p.r);
    pair_free(&p);
    return e;
}

int p9_lopen(struct p9_client *c, uint32_t fid, uint32_t flags)
{
    struct pair p;
    if (pair_new(&p, SMALL, SMALL) < 0)
        return -ENOMEM;
    begin(&p.t, P9_TLOPEN);
    put(&p.t, fid, 4);
    put(&p.t, flags, 4);
    int e = rpc(c, &p.t, &p.r);
    pair_free(&p);
    return e;
}

int p9_lcreate(struct p9_client *c, uint32_t fid, const char *name, size_t len, uint32_t flags, uint32_t mode,
               uint32_t gid)
{
    struct pair p;
    if (pair_new(&p, SMALL, SMALL) < 0)
        return -ENOMEM;
    begin(&p.t, P9_TLCREATE);
    put(&p.t, fid, 4);
    put_str(&p.t, name, len);
    put(&p.t, flags, 4);
    put(&p.t, mode, 4);
    put(&p.t, gid, 4);
    int e = rpc(c, &p.t, &p.r);
    pair_free(&p);
    return e;
}

long p9_read(struct p9_client *c, uint32_t fid, uint64_t offset, void *buf, size_t n)
{
    if (n > P9_IO_MAX)
        n = P9_IO_MAX;
    struct pair p;
    if (pair_new(&p, SMALL, P9_HEADER + 4 + n) < 0)
        return -ENOMEM;
    begin(&p.t, P9_TREAD);
    put(&p.t, fid, 4);
    put(&p.t, offset, 8);
    put(&p.t, n, 4);
    long r = rpc(c, &p.t, &p.r);
    if (r == 0) {
        uint32_t count = (uint32_t)get(&p.r, 4);
        if (p.r.bad || count > n || p.r.len + count > p.r.cap) {
            r = -EIO;
        } else {
            memcpy(buf, p.r.data + p.r.len, count);
            r = count;
        }
    }
    pair_free(&p);
    return r;
}

long p9_write(struct p9_client *c, uint32_t fid, uint64_t offset, const void *buf, size_t n)
{
    if (n > P9_IO_MAX)
        n = P9_IO_MAX;
    struct pair p;
    if (pair_new(&p, P9_HEADER + 16 + n, SMALL) < 0)
        return -ENOMEM;
    begin(&p.t, P9_TWRITE);
    put(&p.t, fid, 4);
    put(&p.t, offset, 8);
    put(&p.t, n, 4);
    memcpy(p.t.data + p.t.len, buf, n);
    p.t.len += n;
    long r = rpc(c, &p.t, &p.r);
    if (r == 0) {
        uint32_t count = (uint32_t)get(&p.r, 4);
        r = p.r.bad || count > n ? -EIO : (long)count;
    }
    pair_free(&p);
    return r;
}

int p9_clunk(struct p9_client *c, uint32_t fid)
{
    struct pair p;
    int e = pair_new(&p, SMALL, SMALL);
    if (e == 0) {
        begin(&p.t, P9_TCLUNK);
        put(&p.t, fid, 4);
        e = rpc(c, &p.t, &p.r);
        pair_free(&p);
    }
    /* The server forgets the fid also after an error of Tclunk. */
    p9_fid_free(c, fid);
    return e;
}

long p9_readdir(struct p9_client *c, uint32_t fid, uint64_t offset, void *buf, size_t n)
{
    struct pair p;
    if (pair_new(&p, SMALL, P9_HEADER + 4 + n) < 0)
        return -ENOMEM;
    begin(&p.t, P9_TREADDIR);
    put(&p.t, fid, 4);
    put(&p.t, offset, 8);
    put(&p.t, n, 4);
    long r = rpc(c, &p.t, &p.r);
    if (r == 0) {
        uint32_t count = (uint32_t)get(&p.r, 4);
        if (p.r.bad || count > n || p.r.len + count > p.r.cap) {
            r = -EIO;
        } else {
            memcpy(buf, p.r.data + p.r.len, count);
            r = count;
        }
    }
    pair_free(&p);
    return r;
}

size_t p9_dirent_parse(const uint8_t *buf, size_t len, struct p9_qid *qid, uint64_t *next, uint8_t *type,
                       const char **name, size_t *namelen)
{
    struct msg m = { (uint8_t *)buf, len, 0, false };
    get_qid(&m, qid);
    *next = get(&m, 8);
    *type = (uint8_t)get(&m, 1);
    size_t n = (size_t)get(&m, 2);
    if (m.bad || m.len + n > len)
        return 0;
    *name = (const char *)buf + m.len;
    *namelen = n;
    return m.len + n;
}

int p9_mkdir(struct p9_client *c, uint32_t dfid, const char *name, size_t len, uint32_t mode, uint32_t gid)
{
    struct pair p;
    if (pair_new(&p, SMALL, SMALL) < 0)
        return -ENOMEM;
    begin(&p.t, P9_TMKDIR);
    put(&p.t, dfid, 4);
    put_str(&p.t, name, len);
    put(&p.t, mode, 4);
    put(&p.t, gid, 4);
    int e = rpc(c, &p.t, &p.r);
    pair_free(&p);
    return e;
}

int p9_unlinkat(struct p9_client *c, uint32_t dfid, const char *name, size_t len, uint32_t flags)
{
    struct pair p;
    if (pair_new(&p, SMALL, SMALL) < 0)
        return -ENOMEM;
    begin(&p.t, P9_TUNLINKAT);
    put(&p.t, dfid, 4);
    put_str(&p.t, name, len);
    put(&p.t, flags, 4);
    int e = rpc(c, &p.t, &p.r);
    pair_free(&p);
    return e;
}

int p9_renameat(struct p9_client *c, uint32_t olddfid, const char *oldname, size_t oldlen, uint32_t newdfid,
                const char *newname, size_t newlen)
{
    struct pair p;
    if (pair_new(&p, 2 * SMALL + 32, SMALL) < 0)
        return -ENOMEM;
    begin(&p.t, P9_TRENAMEAT);
    put(&p.t, olddfid, 4);
    put_str(&p.t, oldname, oldlen);
    put(&p.t, newdfid, 4);
    put_str(&p.t, newname, newlen);
    int e = rpc(c, &p.t, &p.r);
    pair_free(&p);
    return e;
}

int p9_symlink(struct p9_client *c, uint32_t dfid, const char *name, size_t len, const char *target, size_t tlen,
               uint32_t gid)
{
    struct pair p;
    if (pair_new(&p, SMALL + VFS_PATH_MAX + 32, SMALL) < 0)
        return -ENOMEM;
    begin(&p.t, P9_TSYMLINK);
    put(&p.t, dfid, 4);
    put_str(&p.t, name, len);
    put_str(&p.t, target, tlen);
    put(&p.t, gid, 4);
    int e = rpc(c, &p.t, &p.r);
    pair_free(&p);
    return e;
}

int p9_link(struct p9_client *c, uint32_t dfid, uint32_t fid, const char *name, size_t len)
{
    struct pair p;
    if (pair_new(&p, SMALL, SMALL) < 0)
        return -ENOMEM;
    begin(&p.t, P9_TLINK);
    put(&p.t, dfid, 4);
    put(&p.t, fid, 4);
    put_str(&p.t, name, len);
    int e = rpc(c, &p.t, &p.r);
    pair_free(&p);
    return e;
}

int p9_readlink(struct p9_client *c, uint32_t fid, char *buf, size_t size)
{
    struct pair p;
    if (pair_new(&p, SMALL, P9_HEADER + 2 + 4096) < 0)
        return -ENOMEM;
    begin(&p.t, P9_TREADLINK);
    put(&p.t, fid, 4);
    int e = rpc(c, &p.t, &p.r);
    if (e == 0) {
        size_t len = (size_t)get(&p.r, 2);
        if (p.r.bad || p.r.len + len > p.r.cap) {
            e = -EIO;
        } else {
            len = MIN(len, size);
            memcpy(buf, p.r.data + p.r.len, len);
            e = (int)len;
        }
    }
    pair_free(&p);
    return e;
}

int p9_statfs(struct p9_client *c, uint32_t fid, struct fs_space *space)
{
    struct pair p;
    if (pair_new(&p, SMALL, SMALL) < 0)
        return -ENOMEM;
    begin(&p.t, P9_TSTATFS);
    put(&p.t, fid, 4);
    int e = rpc(c, &p.t, &p.r);
    if (e == 0) {
        get(&p.r, 4);               /* type */
        uint32_t bsize = (uint32_t)get(&p.r, 4);
        uint64_t blocks = get(&p.r, 8), bfree = get(&p.r, 8);
        if (p.r.bad || bsize == 0) {
            e = -EIO;
        } else {
            space->block_size = bsize;
            space->blocks = blocks;
            space->free_blocks = bfree;
        }
    }
    pair_free(&p);
    return e;
}

int p9_fsync(struct p9_client *c, uint32_t fid)
{
    struct pair p;
    if (pair_new(&p, SMALL, SMALL) < 0)
        return -ENOMEM;
    begin(&p.t, P9_TFSYNC);
    put(&p.t, fid, 4);
    put(&p.t, 0, 4);                /* data and metadata */
    int e = rpc(c, &p.t, &p.r);
    pair_free(&p);
    return e;
}
