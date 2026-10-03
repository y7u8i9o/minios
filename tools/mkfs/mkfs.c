/* mkfs: build an mfs image from a directory tree, or inspect one.
 *
 *   mkfs [-p perms] <image> <size_mb> <dir>   create image with the tree of dir
 *   mkfs --dump <image>                       print superblock state and tree
 *   mkfs --cat <image> <path>                 print the contents of a file
 *
 * SIZE_MB 0 stands for the size of the existing image file or block device,
 * which is then written in place without truncation. A nonzero size on a
 * block device is an error. The tool is also built for minios, where it
 * installs as /usr/bin/mkfs.
 *
 * No buffer of the size of the image exists, which matters for disks of
 * several GiB on a small machine. Only the blocks the format touches are
 * read into memory and written out in block order. A new image is created with
 * its full size by ftruncate. On an existing target the superblock, both
 * bitmaps, the inode table and the journal (everything below the data area)
 * are written in full, with zeros where the tree does not use them, and
 * free data blocks are left as they are. The read modes fetch blocks on
 * demand.
 *
 * Exits non zero on any error. --dump reports "clean" or "unclean" and
 * the state of the journal. Symbolic links of the host tree are stored as
 * links (their target in one data block). A target longer than 255 bytes,
 * which the kernel would not resolve, is an error. Images are written in
 * format version 5, which places a journal between the inode table and the
 * data blocks, stores modification times in nanoseconds and owners.
 *
 * Files and directories keep the permission bits of the host tree and
 * belong to root. The manifest of -p changes modes and owners, one entry
 * per line in the form "path mode uid gid", where mode is octal or "-" to
 * keep the bits. A path ending in "/" names a directory and everything
 * below it, and lines starting with "#" are comments.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <errno.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fs/mfs_format.h>

#define S_IFMT_  0170000
#define S_IFDIR_ 0040000
#define S_IFREG_ 0100000
#define S_IFLNK_ 0120000
/* The longest target the kernel follows (VFS_SYMLINK_MAX). */
#define LINK_TARGET_MAX 255

/* Modification time of a host file in nanoseconds since the epoch. */
static uint64_t host_mtime_ns(const struct stat *st)
{
#if defined(__APPLE__) && defined(_POSIX_C_SOURCE) && !defined(_DARWIN_C_SOURCE)
    return (uint64_t)st->st_mtime * 1000000000ull + (uint64_t)st->st_mtimensec;
#elif defined(__APPLE__)
    return (uint64_t)st->st_mtimespec.tv_sec * 1000000000ull + (uint64_t)st->st_mtimespec.tv_nsec;
#else
    return (uint64_t)st->st_mtim.tv_sec * 1000000000ull + (uint64_t)st->st_mtim.tv_nsec;
#endif
}

/* The image is never read into memory as a whole. While an image is built,
 * only the blocks the format touches exist, each in a buffer that is created
 * zeroed on first access and found again through a hash table keyed by the
 * block number. They are written out in block order at the end. While an
 * image is read, blocks are fetched on demand into a small cache. A pointer
 * returned by block() remains valid until RING_SIZE other blocks have been
 * fetched. */
struct map_entry {
    uint64_t blk;
    uint8_t *data;                  /* NULL marks an unused slot */
};

#define RING_SIZE 16

static struct map_entry *map;       /* slots of the table, a power of two */
static size_t map_cap, map_count;

static FILE *img_file;              /* the open image in the read modes */
static struct {
    uint64_t blk;
    uint8_t *data;                  /* NULL until first used */
} ring[RING_SIZE];
static int ring_next;

static int building;                /* 1 while an image is created */
static uint64_t img_blocks;
static uint64_t next_data;          /* no data block below this one is free */
static struct mfs_superblock *sb;

static __attribute__((noreturn)) void die(const char *msg)
{
    fprintf(stderr, "mkfs: %s\n", msg);
    exit(1);
}

static size_t map_slot(uint64_t blk, size_t cap)
{
    return (size_t)((blk * 0x9e3779b97f4a7c15ull) >> 20) & (cap - 1);
}

/* The entry of blk, or NULL when the block was never touched. */
static struct map_entry *map_find(uint64_t blk)
{
    if (!map_cap)
        return NULL;
    for (size_t i = map_slot(blk, map_cap); map[i].data; i = (i + 1) & (map_cap - 1))
        if (map[i].blk == blk)
            return &map[i];
    return NULL;
}

static void map_grow(void)
{
    size_t cap = map_cap ? map_cap * 2 : 1024;
    struct map_entry *nm = calloc(cap, sizeof *nm);
    if (!nm)
        die("out of memory");
    for (size_t k = 0; k < map_cap; k++) {
        if (!map[k].data)
            continue;
        size_t i = map_slot(map[k].blk, cap);
        while (nm[i].data)
            i = (i + 1) & (cap - 1);
        nm[i] = map[k];
    }
    free(map);
    map = nm;
    map_cap = cap;
}

static uint8_t *map_get(uint64_t blk)
{
    struct map_entry *e = map_find(blk);
    if (e)
        return e->data;
    if ((map_count + 1) * 2 > map_cap)
        map_grow();
    size_t i = map_slot(blk, map_cap);
    while (map[i].data)
        i = (i + 1) & (map_cap - 1);
    map[i].blk = blk;
    map[i].data = calloc(1, MFS_BLOCK_SIZE);
    if (!map[i].data)
        die("out of memory");
    map_count++;
    return map[i].data;
}

static uint8_t *block(uint64_t n)
{
    if (n >= img_blocks)
        die("block out of range");
    if (building)
        return map_get(n);
    for (int i = 0; i < RING_SIZE; i++)
        if (ring[i].data && ring[i].blk == n)
            return ring[i].data;
    int slot = ring_next;
    ring_next = (ring_next + 1) % RING_SIZE;
    if (!ring[slot].data) {
        ring[slot].data = malloc(MFS_BLOCK_SIZE);
        if (!ring[slot].data)
            die("out of memory");
    }
    ring[slot].data[0] = 0;
    if (fseeko(img_file, (off_t)(n * MFS_BLOCK_SIZE), SEEK_SET) != 0 ||
        fread(ring[slot].data, 1, MFS_BLOCK_SIZE, img_file) != MFS_BLOCK_SIZE)
        die("cannot read image");
    ring[slot].blk = n;
    return ring[slot].data;
}

static struct mfs_dinode *dinode(uint32_t ino)
{
    if (ino == 0 || ino >= sb->ninodes)
        die("inode out of range");
    return (struct mfs_dinode *)(block(sb->inode_table_start + ino / MFS_INODES_PER_BLOCK)
                                 + (ino % MFS_INODES_PER_BLOCK) * MFS_INODE_SIZE);
}

static int bitmap_test(uint32_t start, uint64_t bit)
{
    return block(start + bit / (MFS_BLOCK_SIZE * 8))[(bit / 8) % MFS_BLOCK_SIZE] >> (bit % 8) & 1;
}

static void bitmap_set(uint32_t start, uint64_t bit)
{
    block(start + bit / (MFS_BLOCK_SIZE * 8))[(bit / 8) % MFS_BLOCK_SIZE] |= (uint8_t)(1 << (bit % 8));
}

/* Blocks are never freed while an image is built, thus the first free one
 * is found by advancing next_data. A new block reads as zeros because the
 * map creates it zeroed. */
static uint32_t alloc_block(void)
{
    for (uint64_t b = next_data; b < sb->nblocks; b++) {
        if (!bitmap_test(sb->block_bitmap_start, b)) {
            bitmap_set(sb->block_bitmap_start, b);
            sb->free_blocks--;
            next_data = b + 1;
            return (uint32_t)b;
        }
    }
    die("out of blocks");
}

static uint32_t alloc_inode(void)
{
    for (uint32_t i = 1; i < sb->ninodes; i++) {
        if (!bitmap_test(sb->inode_bitmap_start, i)) {
            bitmap_set(sb->inode_bitmap_start, i);
            sb->free_inodes--;
            memset(dinode(i), 0, MFS_INODE_SIZE);
            return i;
        }
    }
    die("out of inodes");
}

/* Map logical block index to a physical block, allocating when needed. */
static uint32_t bmap(struct mfs_dinode *di, uint64_t idx, int alloc)
{
    if (idx < MFS_NDIRECT) {
        if (!di->direct[idx] && alloc)
            di->direct[idx] = alloc_block();
        return di->direct[idx];
    }
    idx -= MFS_NDIRECT;
    if (idx < MFS_PTRS_PER_BLOCK) {
        if (!di->indirect) {
            if (!alloc) return 0;
            di->indirect = alloc_block();
        }
        uint32_t *tbl = (uint32_t *)block(di->indirect);
        if (!tbl[idx] && alloc)
            tbl[idx] = alloc_block();
        return tbl[idx];
    }
    idx -= MFS_PTRS_PER_BLOCK;
    if (idx >= (uint64_t)MFS_PTRS_PER_BLOCK * MFS_PTRS_PER_BLOCK)
        die("file too large");
    if (!di->dindirect) {
        if (!alloc) return 0;
        di->dindirect = alloc_block();
    }
    uint32_t *l1 = (uint32_t *)block(di->dindirect);
    uint32_t i1 = (uint32_t)(idx / MFS_PTRS_PER_BLOCK);
    if (!l1[i1]) {
        if (!alloc) return 0;
        l1[i1] = alloc_block();
    }
    uint32_t *l2 = (uint32_t *)block(l1[i1]);
    uint32_t i2 = (uint32_t)(idx % MFS_PTRS_PER_BLOCK);
    if (!l2[i2] && alloc)
        l2[i2] = alloc_block();
    return l2[i2];
}

/* Store len bytes into the data blocks of ino, taken from data or, when
 * data is NULL, read block by block from the open file f. */
static void write_data(uint32_t ino, const void *data, uint64_t len, FILE *f)
{
    struct mfs_dinode *di = dinode(ino);
    const uint8_t *p = data;
    for (uint64_t off = 0; off < len; off += MFS_BLOCK_SIZE) {
        uint32_t b = bmap(di, off / MFS_BLOCK_SIZE, 1);
        uint64_t n = len - off < MFS_BLOCK_SIZE ? len - off : MFS_BLOCK_SIZE;
        if (p)
            memcpy(block(b), p + off, n);
        else if (fread(block(b), 1, n, f) != n)
            die("short read");
    }
    di->size = len;
}

static void add_dirent(uint32_t dir, const char *name, uint32_t ino)
{
    if (strlen(name) > MFS_NAME_MAX)
        die("name too long");
    struct mfs_dinode *di = dinode(dir);
    uint64_t slot = di->size / MFS_DIRENT_SIZE;
    uint32_t b = bmap(di, slot * MFS_DIRENT_SIZE / MFS_BLOCK_SIZE, 1);
    struct mfs_dirent *e = (struct mfs_dirent *)(block(b) + (slot * MFS_DIRENT_SIZE) % MFS_BLOCK_SIZE);
    e->ino = ino;
    strncpy(e->name, name, MFS_NAME_MAX);
    e->name[MFS_NAME_MAX] = '\0';
    di->size += MFS_DIRENT_SIZE;
}

static uint32_t make_dir(uint32_t parent, uint32_t perm)
{
    uint32_t ino = alloc_inode();
    struct mfs_dinode *di = dinode(ino);
    di->mode = S_IFDIR_ | perm;
    di->nlink = 2;
    add_dirent(ino, ".", ino);
    add_dirent(ino, "..", parent ? parent : ino);
    if (parent)
        dinode(parent)->nlink++;
    return ino;
}

static void add_tree(uint32_t dir, const char *path)
{
    DIR *d = opendir(path);
    if (!d)
        die(path);
    struct dirent *e;
    while ((e = readdir(d))) {
        /* Dot files are part of the image (.shrc, .config); only the
         * directory entries and the macOS Finder file are skipped. */
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0 || strcmp(e->d_name, ".DS_Store") == 0)
            continue;
        char full[1024];
        snprintf(full, sizeof full, "%s/%s", path, e->d_name);
        struct stat st;
        if (lstat(full, &st) < 0)
            die(full);
        if (S_ISLNK(st.st_mode)) {
            char target[LINK_TARGET_MAX + 2];
            ssize_t n = readlink(full, target, sizeof target);
            if (n < 0)
                die(full);
            if (n == 0 || n > LINK_TARGET_MAX) {
                fprintf(stderr, "mkfs: %s: symbolic link target empty or longer than %d bytes\n", full, LINK_TARGET_MAX);
                exit(1);
            }
            uint32_t ino = alloc_inode();
            struct mfs_dinode *di = dinode(ino);
            di->mode = S_IFLNK_ | 0777;
            di->nlink = 1;
            di->mtime = host_mtime_ns(&st);
            write_data(ino, target, (uint64_t)n, NULL);
            add_dirent(dir, e->d_name, ino);
        } else if (S_ISDIR(st.st_mode)) {
            uint32_t sub = make_dir(dir, st.st_mode & 07777);
            dinode(sub)->mtime = host_mtime_ns(&st);
            add_dirent(dir, e->d_name, sub);
            add_tree(sub, full);
        } else if (S_ISREG(st.st_mode)) {
            FILE *f = fopen(full, "rb");
            if (!f)
                die(full);
            uint32_t ino = alloc_inode();
            struct mfs_dinode *di = dinode(ino);
            di->mode = S_IFREG_ | (st.st_mode & 07777);
            di->nlink = 1;
            di->mtime = host_mtime_ns(&st);
            write_data(ino, NULL, (uint64_t)st.st_size, f);
            fclose(f);
            add_dirent(dir, e->d_name, ino);
        }
    }
    closedir(d);
}

static void format(uint64_t nblocks)
{
    img_blocks = nblocks;
    building = 1;
    sb = (struct mfs_superblock *)block(0);
    sb->magic = MFS_MAGIC;
    sb->version = MFS_VERSION;
    sb->block_size = MFS_BLOCK_SIZE;
    sb->flags = MFS_FLAG_CLEAN;
    sb->nblocks = nblocks;
    uint32_t ninodes = (uint32_t)(nblocks / 4);
    if (ninodes < 64)
        ninodes = 64;
    ninodes = (ninodes + MFS_INODES_PER_BLOCK - 1) / MFS_INODES_PER_BLOCK * MFS_INODES_PER_BLOCK;
    sb->ninodes = ninodes;
    sb->inode_bitmap_start = 1;
    sb->inode_bitmap_blocks = (ninodes + MFS_BLOCK_SIZE * 8 - 1) / (MFS_BLOCK_SIZE * 8);
    sb->block_bitmap_start = sb->inode_bitmap_start + sb->inode_bitmap_blocks;
    sb->block_bitmap_blocks = (uint32_t)((nblocks + MFS_BLOCK_SIZE * 8 - 1) / (MFS_BLOCK_SIZE * 8));
    sb->inode_table_start = sb->block_bitmap_start + sb->block_bitmap_blocks;
    sb->inode_table_blocks = ninodes / MFS_INODES_PER_BLOCK;
    sb->journal_start = sb->inode_table_start + sb->inode_table_blocks;
    sb->journal_blocks = MFS_JOURNAL_BLOCKS;
    sb->data_start = sb->journal_start + sb->journal_blocks;
    if (sb->data_start >= nblocks)
        die("image too small");
    for (uint32_t b = 0; b < sb->data_start; b++)
        bitmap_set(sb->block_bitmap_start, b);
    struct mfs_journal_header *jh = (struct mfs_journal_header *)block(sb->journal_start);
    jh->magic = MFS_JOURNAL_MAGIC;
    jh->count = 0;
    jh->sequence = 1;
    next_data = sb->data_start;
    sb->free_blocks = nblocks - sb->data_start;
    bitmap_set(sb->inode_bitmap_start, 0);
    sb->free_inodes = ninodes - 1;
    uint32_t root = make_dir(0, 0755);
    dinode(root)->mtime = (uint64_t)time(NULL) * 1000000000ull;
    if (root != MFS_ROOT_INO)
        die("root inode is not 1");
}

/* The size in bytes of an existing block device or regular file, which
 * SIZE_MB 0 selects. The target is then written in place without creating
 * or truncating it. */
static uint64_t existing_size(const char *path)
{
    struct stat st;
    if (stat(path, &st) < 0)
        die(path);
    if (!S_ISBLK(st.st_mode) && !S_ISREG(st.st_mode))
        die("not a block device or regular file");
    return (uint64_t)st.st_size;
}

/* Whether path is a block device, whose size cannot be changed. */
static int is_block_device(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISBLK(st.st_mode);
}

static void load(const char *path)
{
    img_file = fopen(path, "rb");
    if (!img_file)
        die(path);
    if (fseeko(img_file, 0, SEEK_END) != 0)
        die("cannot read image");
    off_t size = ftello(img_file);
    if (size < MFS_BLOCK_SIZE)
        die("cannot read image");
    img_blocks = (uint64_t)size / MFS_BLOCK_SIZE;
    sb = malloc(MFS_BLOCK_SIZE);
    if (!sb || fseeko(img_file, 0, SEEK_SET) != 0 || fread(sb, 1, MFS_BLOCK_SIZE, img_file) != MFS_BLOCK_SIZE)
        die("cannot read image");
    if (sb->magic != MFS_MAGIC)
        die("not an mfs image");
    if (sb->version < MFS_VERSION_MIN || sb->version > MFS_VERSION)
        die("unsupported format version (rebuild the image)");
    if (sb->nblocks > img_blocks)
        die("image truncated");
}

static int map_cmp(const void *a, const void *b)
{
    uint64_t x = ((const struct map_entry *)a)->blk, y = ((const struct map_entry *)b)->blk;
    return x < y ? -1 : x > y;
}

/* Write the blocks of the map to f in block order. Everything below the data
 * area has a required content, thus on an existing target the blocks there
 * that were never touched are written as zeros (the free data blocks are
 * left as they are). A new target is made as large as the image first and
 * reads as zeros wherever nothing is written. */
static void write_image(FILE *f, int existing)
{
    static const uint8_t zero[MFS_BLOCK_SIZE];
    uint64_t at = UINT64_MAX;       /* the block the file position is at */
    if (!existing) {
        if (fflush(f) != 0 || ftruncate(fileno(f), (long)(img_blocks * MFS_BLOCK_SIZE)) != 0)
            die("cannot set the image size");
    }
    for (uint64_t b = 0; b < sb->data_start; b++) {
        struct map_entry *e = map_find(b);
        if (!e && !existing)
            continue;
        if (at != b && fseeko(f, (off_t)(b * MFS_BLOCK_SIZE), SEEK_SET) != 0)
            die("seek failed");
        if (fwrite(e ? e->data : zero, 1, MFS_BLOCK_SIZE, f) != MFS_BLOCK_SIZE)
            die("short write");
        at = b + 1;
    }
    struct map_entry *list = malloc(map_count * sizeof *list);
    if (!list)
        die("out of memory");
    size_t n = 0;
    for (size_t i = 0; i < map_cap; i++)
        if (map[i].data && map[i].blk >= sb->data_start)
            list[n++] = map[i];
    qsort(list, n, sizeof *list, map_cmp);
    for (size_t i = 0; i < n; i++) {
        if (at != list[i].blk && fseeko(f, (off_t)(list[i].blk * MFS_BLOCK_SIZE), SEEK_SET) != 0)
            die("seek failed");
        if (fwrite(list[i].data, 1, MFS_BLOCK_SIZE, f) != MFS_BLOCK_SIZE)
            die("short write");
        at = list[i].blk + 1;
    }
    free(list);
}

static void dump_tree(uint32_t ino, const char *prefix, int depth)
{
    /* A copy, because the block cache may reuse the buffer of the inode. */
    struct mfs_dinode copy = *dinode(ino);
    struct mfs_dinode *di = &copy;
    for (uint64_t off = 0; off < di->size; off += MFS_DIRENT_SIZE) {
        uint32_t b = bmap(di, off / MFS_BLOCK_SIZE, 0);
        /* Copies, because a hit does not renew a cache entry and the next fetch
         * may reuse the buffer of the directory block or of the inode block. */
        struct mfs_dirent ent = *(struct mfs_dirent *)(block(b) + off % MFS_BLOCK_SIZE);
        struct mfs_dirent *e = &ent;
        if (!e->ino || strcmp(e->name, ".") == 0 || strcmp(e->name, "..") == 0)
            continue;
        struct mfs_dinode cin = *dinode(e->ino);
        struct mfs_dinode *c = &cin;
        int isdir = (c->mode & S_IFMT_) == S_IFDIR_;
        printf("%s/%s%s ino %u size %llu nlink %u mode %o uid %u gid %u", prefix, e->name, isdir ? "/" : "",
               e->ino, (unsigned long long)c->size, c->nlink, c->mode & 07777, c->uid, c->gid);
        if ((c->mode & S_IFMT_) == S_IFLNK_ && c->direct[0] && c->size < MFS_BLOCK_SIZE)
            printf(" -> %.*s", (int)c->size, (const char *)block(c->direct[0]));
        printf("\n");
        if (isdir && depth < 16) {
            char sub[1024];
            snprintf(sub, sizeof sub, "%s/%s", prefix, e->name);
            dump_tree(e->ino, sub, depth + 1);
        }
    }
}

static uint32_t lookup(const char *path)
{
    uint32_t ino = MFS_ROOT_INO;
    char buf[1024];
    strncpy(buf, path, sizeof buf - 1);
    buf[sizeof buf - 1] = '\0';
    char *save = NULL;
    for (char *comp = strtok_r(buf, "/", &save); comp; comp = strtok_r(NULL, "/", &save)) {
        struct mfs_dinode copy = *dinode(ino);  /* the cache may reuse the inode buffer */
        struct mfs_dinode *di = &copy;
        uint32_t found = 0;
        for (uint64_t off = 0; off < di->size && !found; off += MFS_DIRENT_SIZE) {
            uint32_t b = bmap(di, off / MFS_BLOCK_SIZE, 0);
            struct mfs_dirent *e = (struct mfs_dirent *)(block(b) + off % MFS_BLOCK_SIZE);
            if (e->ino && strcmp(e->name, comp) == 0)
                found = e->ino;
        }
        if (!found)
            die("path not found");
        ino = found;
    }
    return ino;
}

/* Set the mode (unless keep) and owner of ino, and of everything below it
 * when recursive. */
static void set_owner(uint32_t ino, int keep, uint32_t mode, uint32_t uid, uint32_t gid, int recursive)
{
    struct mfs_dinode *di = dinode(ino);
    if (!keep)
        di->mode = (di->mode & S_IFMT_) | mode;
    di->uid = uid;
    di->gid = gid;
    if (!recursive || (di->mode & S_IFMT_) != S_IFDIR_)
        return;
    for (uint64_t off = 0; off < di->size; off += MFS_DIRENT_SIZE) {
        uint32_t b = bmap(di, off / MFS_BLOCK_SIZE, 0);
        struct mfs_dirent *e = (struct mfs_dirent *)(block(b) + off % MFS_BLOCK_SIZE);
        if (e->ino && strcmp(e->name, ".") != 0 && strcmp(e->name, "..") != 0)
            set_owner(e->ino, keep, mode, uid, gid, 1);
    }
}

static void apply_manifest(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        die(path);
    char line[1024];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        n++;
        char file[900], mode[16];
        unsigned uid, gid;
        if (line[0] == '#' || strspn(line, " \t\n") == strlen(line))
            continue;
        if (sscanf(line, "%899s %15s %u %u", file, mode, &uid, &gid) != 4) {
            fprintf(stderr, "mkfs: %s:%d: expected \"path mode uid gid\"\n", path, n);
            exit(1);
        }
        size_t len = strlen(file);
        int recursive = len > 1 && file[len - 1] == '/';
        int keep = strcmp(mode, "-") == 0;
        char *end;
        unsigned long bits = keep ? 0 : strtoul(mode, &end, 8);
        if (!keep && (*end || bits > 07777)) {
            fprintf(stderr, "mkfs: %s:%d: bad mode %s\n", path, n, mode);
            exit(1);
        }
        set_owner(lookup(file), keep, (uint32_t)bits, uid, gid, recursive);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    const char *manifest = NULL;
    if (argc > 2 && strcmp(argv[1], "-p") == 0) {
        manifest = argv[2];
        argc -= 2;
        argv += 2;
    }
    if (argc == 3 && strcmp(argv[1], "--dump") == 0) {
        load(argv[2]);
        printf("mfs: %llu blocks, %u inodes, %llu free blocks, %u free inodes, mounts %u, %s\n",
               (unsigned long long)sb->nblocks, sb->ninodes, (unsigned long long)sb->free_blocks,
               sb->free_inodes, sb->mount_count, sb->flags & MFS_FLAG_CLEAN ? "clean" : "unclean");
        struct mfs_journal_header *jh = (struct mfs_journal_header *)block(sb->journal_start);
        printf("journal: blocks %u..%u, sequence %llu, %u pending block%s\n", sb->journal_start,
               sb->journal_start + sb->journal_blocks - 1, (unsigned long long)jh->sequence,
               jh->count, jh->count == 1 ? "" : "s");
        dump_tree(MFS_ROOT_INO, "", 0);
        return 0;
    }
    if (argc == 4 && strcmp(argv[1], "--cat") == 0) {
        load(argv[2]);
        uint32_t ino = lookup(argv[3]);
        struct mfs_dinode copy = *dinode(ino);  /* the cache may reuse the inode buffer */
        struct mfs_dinode *di = &copy;
        for (uint64_t off = 0; off < di->size; off += MFS_BLOCK_SIZE) {
            uint32_t b = bmap(di, off / MFS_BLOCK_SIZE, 0);
            uint64_t n = di->size - off < MFS_BLOCK_SIZE ? di->size - off : MFS_BLOCK_SIZE;
            static uint8_t zero[MFS_BLOCK_SIZE];
            fwrite(b ? block(b) : zero, 1, n, stdout);
        }
        return 0;
    }
    if (argc != 4) {
        fprintf(stderr, "usage: mkfs [-p perms] <image> <size_mb|0> <dir> | --dump <image> | --cat <image> <path>\n");
        return 2;
    }
    uint64_t mb = strtoull(argv[2], NULL, 10);
    int existing = 0;
    uint64_t bytes;
    if (mb == 0 && strcmp(argv[2], "0") == 0) {
        bytes = existing_size(argv[1]);
        existing = 1;
    } else {
        if (is_block_device(argv[1]))
            die("the size of a device cannot be changed, use 0");
        bytes = mb * 1024 * 1024;
    }
    if (bytes < 1024 * 1024)
        die("size must be at least 1 MiB");
    format(bytes / MFS_BLOCK_SIZE);
    add_tree(MFS_ROOT_INO, argv[3]);
    if (manifest)
        apply_manifest(manifest);
    FILE *f = fopen(argv[1], existing ? "r+b" : "wb");
    if (!f)
        die(argv[1]);
    write_image(f, existing);
    if (fclose(f) != 0)
        die("short write");
    printf("mkfs: %s: %llu blocks, %u inodes, %llu free blocks\n", argv[1],
           (unsigned long long)sb->nblocks, sb->ninodes, (unsigned long long)sb->free_blocks);
    return 0;
}
