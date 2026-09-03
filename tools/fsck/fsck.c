/* fsck: check and repair an mfs image on the host.
 *
 *   fsck [-n | -y] [-v] <image>
 *
 * The journal is replayed first (a committed transaction whose checksum
 * matches is copied to the home blocks, anything else is discarded), then
 * five passes check the inodes, the directory tree, connectivity, the
 * bitmaps and the superblock counters. Without -y problems are reported
 * only; -y repairs them; -n never writes the image, so the replay happens
 * in memory only. Exit status: 0 no problems, 1 problems repaired, 4
 * problems left, 8 usage or I/O error.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <fs/mfs_format.h>

#define S_IFMT_  0170000
#define S_IFDIR_ 0040000
#define S_IFREG_ 0100000

static uint8_t *img;
static uint64_t img_blocks;
static struct mfs_superblock *sb;
static int fix, readonly, verbose;
static int problems, repaired, unrepaired, dirty;

/* Per block owners (0 free, inode number, or UINT32_MAX for more than
 * one), per inode counted names, parents and subdirectories. */
static uint32_t *block_owner;
static uint32_t *names;
static uint32_t *parent;
static uint32_t *subdirs;
static uint8_t *checked;

static __attribute__((noreturn)) void fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "fsck: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(8);
}

/* Report a problem; returns 1 when the caller should repair it. */
static int problem(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    problems++;
    if (fix) {
        printf(" (fixed)\n");
        repaired++;
        dirty = 1;
        return 1;
    }
    printf("\n");
    unrepaired++;
    return 0;
}

static uint32_t crc32_update(uint32_t crc, const void *data, size_t n)
{
    static uint32_t table[256];
    if (!table[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    }
    const uint8_t *p = data;
    crc = ~crc;
    for (size_t i = 0; i < n; i++)
        crc = table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
    return ~crc;
}

static uint8_t *block(uint64_t n)
{
    if (n >= img_blocks)
        fatal("block %llu out of range", (unsigned long long)n);
    return img + n * MFS_BLOCK_SIZE;
}

static struct mfs_dinode *dinode(uint32_t ino)
{
    return (struct mfs_dinode *)(block(sb->inode_table_start + ino / MFS_INODES_PER_BLOCK)
                                 + (ino % MFS_INODES_PER_BLOCK) * MFS_INODE_SIZE);
}

static int bit_test(uint32_t start, uint64_t bit)
{
    return block(start + bit / (MFS_BLOCK_SIZE * 8))[(bit / 8) % MFS_BLOCK_SIZE] >> (bit % 8) & 1;
}

static void bit_write(uint32_t start, uint64_t bit, int value)
{
    uint8_t *byte = &block(start + bit / (MFS_BLOCK_SIZE * 8))[(bit / 8) % MFS_BLOCK_SIZE];
    if (value)
        *byte |= (uint8_t)(1 << (bit % 8));
    else
        *byte &= (uint8_t)~(1 << (bit % 8));
}

static int inode_allocated(uint32_t ino)
{
    return ino > 0 && ino < sb->ninodes && bit_test(sb->inode_bitmap_start, ino);
}

static int is_dir(const struct mfs_dinode *d)
{
    return (d->mode & S_IFMT_) == S_IFDIR_;
}

/* ---- loading and the superblock ---- */

static void load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        fatal("%s: cannot open", path);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < MFS_BLOCK_SIZE)
        fatal("%s: too small", path);
    img_blocks = (uint64_t)size / MFS_BLOCK_SIZE;
    img = malloc((size_t)img_blocks * MFS_BLOCK_SIZE);
    if (!img || fread(img, MFS_BLOCK_SIZE, img_blocks, f) != img_blocks)
        fatal("%s: cannot read", path);
    fclose(f);
    sb = (struct mfs_superblock *)img;
}

static void save(const char *path)
{
    FILE *f = fopen(path, "r+b");
    if (!f)
        fatal("%s: cannot open for writing", path);
    if (fwrite(img, MFS_BLOCK_SIZE, img_blocks, f) != img_blocks)
        fatal("%s: short write", path);
    fclose(f);
}

static void check_superblock(void)
{
    if (sb->magic != MFS_MAGIC)
        fatal("not an mfs image");
    if (sb->version != MFS_VERSION)
        fatal("format version %u, expected %u", sb->version, MFS_VERSION);
    if (sb->block_size != MFS_BLOCK_SIZE)
        fatal("block size %u", sb->block_size);
    if (sb->nblocks > img_blocks)
        fatal("filesystem of %llu blocks in an image of %llu", (unsigned long long)sb->nblocks,
              (unsigned long long)img_blocks);
    uint32_t ib = (sb->ninodes + MFS_BLOCK_SIZE * 8 - 1) / (MFS_BLOCK_SIZE * 8);
    uint32_t bb = (uint32_t)((sb->nblocks + MFS_BLOCK_SIZE * 8 - 1) / (MFS_BLOCK_SIZE * 8));
    if (sb->inode_bitmap_start != 1 || sb->inode_bitmap_blocks != ib ||
        sb->block_bitmap_start != 1 + ib || sb->block_bitmap_blocks != bb ||
        sb->inode_table_start != 1 + ib + bb ||
        sb->inode_table_blocks != sb->ninodes / MFS_INODES_PER_BLOCK ||
        sb->journal_start != sb->inode_table_start + sb->inode_table_blocks ||
        sb->journal_blocks != MFS_JOURNAL_BLOCKS ||
        sb->data_start != sb->journal_start + sb->journal_blocks ||
        sb->data_start >= sb->nblocks)
        fatal("inconsistent region layout in the superblock");
    if (verbose)
        printf("superblock: %llu blocks, %u inodes, data from block %u, %s\n",
               (unsigned long long)sb->nblocks, sb->ninodes, sb->data_start,
               sb->flags & MFS_FLAG_CLEAN ? "clean" : "unclean");
}

/* ---- journal ---- */

static void replay_journal(void)
{
    struct mfs_journal_header *jh = (struct mfs_journal_header *)block(sb->journal_start);
    if (jh->magic != MFS_JOURNAL_MAGIC) {
        if (problem("journal header has no magic"))
            memset(jh, 0, sizeof *jh), jh->magic = MFS_JOURNAL_MAGIC, jh->sequence = 1;
        return;
    }
    if (jh->count == 0) {
        if (verbose)
            printf("journal: empty, sequence %llu\n", (unsigned long long)jh->sequence);
        return;
    }
    int valid = jh->count <= MFS_JOURNAL_SLOTS;
    for (uint32_t i = 0; valid && i < jh->count; i++)
        if (jh->block[i] >= sb->nblocks ||
            (jh->block[i] >= sb->journal_start && jh->block[i] < sb->data_start))
            valid = 0;
    if (valid) {
        struct mfs_journal_header copy = *jh;
        copy.checksum = 0;
        uint32_t crc = crc32_update(0, &copy, sizeof copy);
        for (uint32_t i = 0; i < jh->count; i++)
            crc = crc32_update(crc, block(sb->journal_start + 1 + i), MFS_BLOCK_SIZE);
        valid = crc == jh->checksum;
    }
    if (!valid) {
        printf("journal: discarding an incomplete transaction of %u blocks\n", jh->count);
    } else {
        printf("journal: replaying transaction %llu, %u blocks\n",
               (unsigned long long)jh->sequence, jh->count);
        for (uint32_t i = 0; i < jh->count; i++)
            memcpy(block(jh->block[i]), block(sb->journal_start + 1 + i), MFS_BLOCK_SIZE);
    }
    jh->count = 0;
    jh->checksum = 0;
    dirty = 1;
}

/* ---- pass 1: inodes and block pointers ---- */

static void claim_block(uint32_t ino, uint32_t *ptr, const char *what)
{
    uint32_t b = *ptr;
    if (!b)
        return;
    if (b < sb->data_start || b >= sb->nblocks) {
        if (problem("inode %u: %s block %u out of range", ino, what, b))
            *ptr = 0;
        return;
    }
    if (block_owner[b] == 0) {
        block_owner[b] = ino;
    } else {
        if (problem("inode %u: %s block %u already used by inode %u", ino, what, b,
                    block_owner[b] == UINT32_MAX ? 0 : block_owner[b]))
            *ptr = 0;
        else
            block_owner[b] = UINT32_MAX;
    }
}

static uint64_t count_blocks(uint32_t ino, struct mfs_dinode *d)
{
    uint64_t n = 0;
    for (int i = 0; i < MFS_NDIRECT; i++) {
        claim_block(ino, &d->direct[i], "direct");
        n += d->direct[i] != 0;
    }
    claim_block(ino, &d->indirect, "indirect");
    if (d->indirect) {
        uint32_t *tbl = (uint32_t *)block(d->indirect);
        for (uint32_t i = 0; i < MFS_PTRS_PER_BLOCK; i++) {
            claim_block(ino, &tbl[i], "indirect data");
            n += tbl[i] != 0;
        }
    }
    claim_block(ino, &d->dindirect, "double indirect");
    if (d->dindirect) {
        uint32_t *l1 = (uint32_t *)block(d->dindirect);
        for (uint32_t i = 0; i < MFS_PTRS_PER_BLOCK; i++) {
            claim_block(ino, &l1[i], "double indirect table");
            if (!l1[i])
                continue;
            uint32_t *l2 = (uint32_t *)block(l1[i]);
            for (uint32_t k = 0; k < MFS_PTRS_PER_BLOCK; k++) {
                claim_block(ino, &l2[k], "double indirect data");
                n += l2[k] != 0;
            }
        }
    }
    return n;
}

static void pass1_inodes(void)
{
    for (uint32_t ino = 1; ino < sb->ninodes; ino++) {
        if (!inode_allocated(ino))
            continue;
        struct mfs_dinode *d = dinode(ino);
        uint32_t type = d->mode & S_IFMT_;
        if (type != S_IFDIR_ && type != S_IFREG_) {
            if (problem("inode %u: invalid mode 0%o", ino, d->mode)) {
                memset(d, 0, sizeof *d);
                bit_write(sb->inode_bitmap_start, ino, 0);
            }
            continue;
        }
        if (d->nlink == 0) {
            if (problem("inode %u: allocated with no links", ino)) {
                memset(d, 0, sizeof *d);
                bit_write(sb->inode_bitmap_start, ino, 0);
            }
            continue;
        }
        uint64_t used = count_blocks(ino, d);
        uint64_t max = (d->size + MFS_BLOCK_SIZE - 1) / MFS_BLOCK_SIZE;
        if (used > max && problem("inode %u: %llu data blocks beyond size %llu", ino,
                                  (unsigned long long)(used - max), (unsigned long long)d->size))
            d->size = used * MFS_BLOCK_SIZE;
        if (is_dir(d) && d->size % MFS_DIRENT_SIZE &&
            problem("inode %u: directory size %llu is not a multiple of %d", ino,
                    (unsigned long long)d->size, MFS_DIRENT_SIZE))
            d->size -= d->size % MFS_DIRENT_SIZE;
        checked[ino] = 1;
    }
}

/* ---- pass 2: directories ---- */

static uint32_t bmap(const struct mfs_dinode *d, uint64_t idx)
{
    if (idx < MFS_NDIRECT)
        return d->direct[idx];
    idx -= MFS_NDIRECT;
    if (idx < MFS_PTRS_PER_BLOCK)
        return d->indirect ? ((uint32_t *)block(d->indirect))[idx] : 0;
    idx -= MFS_PTRS_PER_BLOCK;
    if (!d->dindirect)
        return 0;
    uint32_t l1 = ((uint32_t *)block(d->dindirect))[idx / MFS_PTRS_PER_BLOCK];
    return l1 ? ((uint32_t *)block(l1))[idx % MFS_PTRS_PER_BLOCK] : 0;
}

static struct mfs_dirent *entry(const struct mfs_dinode *d, uint64_t slot)
{
    static struct mfs_dirent hole;
    uint32_t b = bmap(d, slot * MFS_DIRENT_SIZE / MFS_BLOCK_SIZE);
    if (!b) {
        memset(&hole, 0, sizeof hole);
        return &hole;
    }
    return (struct mfs_dirent *)(block(b) + (slot * MFS_DIRENT_SIZE) % MFS_BLOCK_SIZE);
}

static void check_dir(uint32_t ino, uint32_t expected_parent, int depth)
{
    struct mfs_dinode *d = dinode(ino);
    if (depth > 64) {
        problem("directory %u: nesting deeper than 64", ino);
        return;
    }
    int seen_dot = 0, seen_dotdot = 0;
    for (uint64_t s = 0; s < d->size / MFS_DIRENT_SIZE; s++) {
        struct mfs_dirent *e = entry(d, s);
        if (!e->ino)
            continue;
        e->name[MFS_NAME_MAX] = '\0';
        if (strcmp(e->name, ".") == 0) {
            seen_dot = 1;
            if (e->ino != ino && problem("directory %u: \".\" points to %u", ino, e->ino))
                e->ino = ino;
            continue;
        }
        if (strcmp(e->name, "..") == 0) {
            seen_dotdot = 1;
            if (e->ino != expected_parent &&
                problem("directory %u: \"..\" points to %u, parent is %u", ino, e->ino, expected_parent))
                e->ino = expected_parent;
            continue;
        }
        if (!inode_allocated(e->ino) || !checked[e->ino]) {
            if (problem("directory %u: entry \"%s\" references unusable inode %u", ino, e->name, e->ino))
                e->ino = 0;
            continue;
        }
        if (e->name[0] == '\0' || strchr(e->name, '/')) {
            if (problem("directory %u: entry for inode %u has an invalid name", ino, e->ino))
                e->ino = 0;
            continue;
        }
        struct mfs_dinode *c = dinode(e->ino);
        if (is_dir(c)) {
            if (parent[e->ino]) {
                if (problem("directory %u: entry \"%s\" links directory %u a second time (also in %u)",
                            ino, e->name, e->ino, parent[e->ino]))
                    e->ino = 0;
                continue;
            }
            parent[e->ino] = ino;
            names[e->ino]++;
            subdirs[ino]++;
            check_dir(e->ino, ino, depth + 1);
        } else {
            names[e->ino]++;
        }
    }
    if (!seen_dot)
        problem("directory %u: no \".\" entry", ino);
    if (!seen_dotdot)
        problem("directory %u: no \"..\" entry", ino);
}

static void pass2_tree(void)
{
    if (!inode_allocated(MFS_ROOT_INO) || !checked[MFS_ROOT_INO] || !is_dir(dinode(MFS_ROOT_INO)))
        fatal("root inode is not a directory");
    parent[MFS_ROOT_INO] = MFS_ROOT_INO;
    names[MFS_ROOT_INO] = 1;
    check_dir(MFS_ROOT_INO, MFS_ROOT_INO, 0);
}

/* ---- pass 3: connectivity and link counts ---- */

static uint32_t alloc_block_for_fsck(void)
{
    for (uint64_t b = sb->data_start; b < sb->nblocks; b++) {
        if (block_owner[b] == 0) {
            block_owner[b] = UINT32_MAX - 1;
            memset(block(b), 0, MFS_BLOCK_SIZE);
            return (uint32_t)b;
        }
    }
    return 0;
}

static int add_entry(uint32_t dir, const char *name, uint32_t ino)
{
    struct mfs_dinode *d = dinode(dir);
    uint64_t nslots = d->size / MFS_DIRENT_SIZE;
    uint64_t s;
    for (s = 0; s < nslots; s++)
        if (!entry(d, s)->ino)
            break;
    uint64_t idx = s * MFS_DIRENT_SIZE / MFS_BLOCK_SIZE;
    if (idx >= MFS_NDIRECT)
        return 0;               /* keep the repair simple: direct blocks only */
    if (!d->direct[idx]) {
        d->direct[idx] = alloc_block_for_fsck();
        if (!d->direct[idx])
            return 0;
    }
    struct mfs_dirent *e = entry(d, s);
    memset(e, 0, sizeof *e);
    e->ino = ino;
    strncpy(e->name, name, MFS_NAME_MAX);
    if (s >= nslots)
        d->size = (s + 1) * MFS_DIRENT_SIZE;
    return 1;
}

static uint32_t lost_found;

static uint32_t find_lost_found(void)
{
    struct mfs_dinode *root = dinode(MFS_ROOT_INO);
    for (uint64_t s = 0; s < root->size / MFS_DIRENT_SIZE; s++) {
        struct mfs_dirent *e = entry(root, s);
        if (e->ino && strcmp(e->name, "lost+found") == 0 && inode_allocated(e->ino) &&
            is_dir(dinode(e->ino)))
            return e->ino;
    }
    /* Create it. */
    uint32_t ino = 0;
    for (uint32_t i = 1; i < sb->ninodes && !ino; i++)
        if (!inode_allocated(i))
            ino = i;
    if (!ino)
        return 0;
    struct mfs_dinode *d = dinode(ino);
    memset(d, 0, sizeof *d);
    d->mode = S_IFDIR_ | 0755;
    d->nlink = 2;
    bit_write(sb->inode_bitmap_start, ino, 1);
    checked[ino] = 1;
    if (!add_entry(ino, ".", ino) || !add_entry(ino, "..", MFS_ROOT_INO) ||
        !add_entry(MFS_ROOT_INO, "lost+found", ino))
        return 0;
    parent[ino] = MFS_ROOT_INO;
    names[ino] = 1;
    subdirs[MFS_ROOT_INO]++;
    printf("created /lost+found as inode %u\n", ino);
    return ino;
}

static void pass3_links(void)
{
    /* Reconnect or release unreferenced inodes first: creating lost+found
     * changes the root's expected link count. */
    for (uint32_t ino = 1; ino < sb->ninodes; ino++) {
        if (!inode_allocated(ino) || !checked[ino] || names[ino])
            continue;
        struct mfs_dinode *d = dinode(ino);
        if (!problem("inode %u (%s, %llu bytes) is not referenced by any directory", ino,
                     is_dir(d) ? "directory" : "file", (unsigned long long)d->size))
            continue;
        if (!lost_found)
            lost_found = find_lost_found();
        char name[MFS_NAME_MAX + 1];
        snprintf(name, sizeof name, "#%u", ino);
        if (lost_found && add_entry(lost_found, name, ino)) {
            names[ino] = 1;
            if (is_dir(d)) {
                parent[ino] = lost_found;
                subdirs[lost_found]++;
                /* Point ".." at the new parent. */
                for (uint64_t s = 0; s < d->size / MFS_DIRENT_SIZE; s++) {
                    struct mfs_dirent *e = entry(d, s);
                    if (e->ino && strcmp(e->name, "..") == 0)
                        e->ino = lost_found;
                }
            }
            printf("  reconnected as /lost+found/%s\n", name);
        } else {
            printf("  cannot reconnect, releasing the inode\n");
            memset(d, 0, sizeof *d);
            bit_write(sb->inode_bitmap_start, ino, 0);
            checked[ino] = 0;
        }
    }
    /* Blocks of inodes released above are no longer owned. */
    for (uint64_t b = sb->data_start; b < sb->nblocks; b++)
        if (block_owner[b] && block_owner[b] < sb->ninodes && !checked[block_owner[b]])
            block_owner[b] = 0;
    for (uint32_t ino = 1; ino < sb->ninodes; ino++) {
        if (!inode_allocated(ino) || !checked[ino] || !names[ino])
            continue;
        struct mfs_dinode *d = dinode(ino);
        uint32_t expected = is_dir(d) ? 2 + subdirs[ino] : names[ino];
        if (d->nlink != expected &&
            problem("inode %u: link count %u, should be %u", ino, d->nlink, expected))
            d->nlink = expected;
    }
}

/* ---- pass 4 and 5: bitmaps and counters ---- */

static void pass4_bitmaps(void)
{
    uint64_t marked_free = 0, marked_used = 0;
    for (uint64_t b = 0; b < sb->nblocks; b++) {
        int used = b < sb->data_start || block_owner[b] != 0;
        int bit = bit_test(sb->block_bitmap_start, b);
        if (used && !bit) {
            if (marked_free++ < 8)
                problem("block %llu is in use but marked free", (unsigned long long)b);
            else
                problems++, fix ? repaired++ : unrepaired++;
            if (fix)
                bit_write(sb->block_bitmap_start, b, 1);
        } else if (!used && bit) {
            if (marked_used++ < 8)
                problem("block %llu is marked used but not referenced", (unsigned long long)b);
            else
                problems++, fix ? repaired++ : unrepaired++;
            if (fix)
                bit_write(sb->block_bitmap_start, b, 0);
        }
    }
    if (marked_free > 8)
        printf("  ... %llu blocks in use but marked free in total\n", (unsigned long long)marked_free);
    if (marked_used > 8)
        printf("  ... %llu blocks marked used but unreferenced in total\n", (unsigned long long)marked_used);
    if (!bit_test(sb->inode_bitmap_start, 0) && problem("inode 0 is marked free"))
        bit_write(sb->inode_bitmap_start, 0, 1);
    for (uint64_t bit = sb->nblocks; bit < (uint64_t)sb->block_bitmap_blocks * MFS_BLOCK_SIZE * 8; bit++)
        if (bit_test(sb->block_bitmap_start, bit) &&
            problem("block bitmap bit %llu beyond the filesystem is set", (unsigned long long)bit))
            bit_write(sb->block_bitmap_start, bit, 0);
    if (fix)
        dirty = 1;
}

static void pass5_counters(void)
{
    uint64_t free_blocks = 0;
    uint32_t free_inodes = 0;
    for (uint64_t b = sb->data_start; b < sb->nblocks; b++)
        free_blocks += !bit_test(sb->block_bitmap_start, b);
    for (uint32_t i = 1; i < sb->ninodes; i++)
        free_inodes += !bit_test(sb->inode_bitmap_start, i);
    if (sb->free_blocks != free_blocks &&
        problem("free block count %llu, counted %llu", (unsigned long long)sb->free_blocks,
                (unsigned long long)free_blocks))
        sb->free_blocks = free_blocks;
    if (sb->free_inodes != free_inodes &&
        problem("free inode count %u, counted %u", sb->free_inodes, free_inodes))
        sb->free_inodes = free_inodes;
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-y") == 0)
            fix = 1;
        else if (strcmp(argv[i], "-n") == 0)
            readonly = 1;
        else if (strcmp(argv[i], "-v") == 0)
            verbose = 1;
        else if (argv[i][0] == '-' || path)
            path = NULL, i = argc;
        else
            path = argv[i];
    }
    if (!path || (fix && readonly)) {
        fprintf(stderr, "usage: fsck [-n | -y] [-v] <image>\n");
        return 8;
    }
    load(path);
    check_superblock();
    replay_journal();

    block_owner = calloc(sb->nblocks, sizeof *block_owner);
    names = calloc(sb->ninodes, sizeof *names);
    parent = calloc(sb->ninodes, sizeof *parent);
    subdirs = calloc(sb->ninodes, sizeof *subdirs);
    checked = calloc(sb->ninodes, 1);
    if (!block_owner || !names || !parent || !subdirs || !checked)
        fatal("out of memory");

    pass1_inodes();
    pass2_tree();
    pass3_links();
    pass4_bitmaps();
    pass5_counters();

    uint32_t files = 0, dirs = 0;
    for (uint32_t i = 1; i < sb->ninodes; i++) {
        if (!inode_allocated(i) || !checked[i])
            continue;
        if (is_dir(dinode(i)))
            dirs++;
        else
            files++;
    }
    int was_clean = sb->flags & MFS_FLAG_CLEAN;
    if (!was_clean) {
        if (unrepaired == 0 && !readonly) {
            sb->flags |= MFS_FLAG_CLEAN;
            dirty = 1;
        }
        printf("filesystem was not unmounted cleanly%s\n",
               unrepaired == 0 && !readonly ? ", marked clean" : "");
    }
    printf("%s: %u files, %u directories, %llu of %llu data blocks used, %d problem%s%s\n", path,
           files, dirs, (unsigned long long)(sb->nblocks - sb->data_start - sb->free_blocks),
           (unsigned long long)(sb->nblocks - sb->data_start), problems, problems == 1 ? "" : "s",
           problems ? (fix ? " repaired" : " found") : "");
    if (dirty && !readonly)
        save(path);
    return unrepaired ? 4 : problems ? 1 : 0;
}
