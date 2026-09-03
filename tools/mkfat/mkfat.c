/* mkfat: build a FAT12, FAT16 or FAT32 image from a directory tree, or
 * inspect one.
 *
 *   mkfat [-t 12|16|32] <image> <size_mb> [dir]   create an image
 *   mkfat --dump <image>                          print the volume and tree
 *   mkfat --cat <image> <path>                    print a file
 *
 * Without -t the type follows the size: FAT12 below 4 MiB, FAT16 below
 * 256 MiB, FAT32 above. Names that are not plain 8.3 names get long name
 * entries; --dump prints long names where they exist and short names in
 * lower case otherwise, which is how the kernel driver presents them.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fs/fat_format.h>

static uint8_t *img;
static uint64_t img_size;
static struct fat_bpb *bpb;
static int fat_type;                /* 12, 16 or 32 */
static uint32_t fat_start, fat_sectors, root_start, root_sectors, data_start, nclusters;
static uint32_t spc;                /* sectors per cluster */
static uint32_t next_free = 2;

static __attribute__((noreturn)) void die(const char *msg)
{
    fprintf(stderr, "mkfat: %s\n", msg);
    exit(1);
}

static uint8_t *sector(uint64_t n)
{
    if ((n + 1) * FAT_SECTOR_SIZE > img_size)
        die("sector out of range");
    return img + n * FAT_SECTOR_SIZE;
}

static uint8_t *cluster(uint32_t c)
{
    if (c < 2 || c >= nclusters + 2)
        die("cluster out of range");
    return sector(data_start + (uint64_t)(c - 2) * spc);
}

static uint32_t cluster_bytes(void)
{
    return spc * FAT_SECTOR_SIZE;
}

/* ---- the file allocation table ---- */

static uint32_t fat_get(uint32_t c)
{
    uint8_t *fat = sector(fat_start);
    if (fat_type == 12) {
        uint32_t off = c + c / 2;
        uint16_t v = (uint16_t)(fat[off] | fat[off + 1] << 8);
        return c & 1 ? v >> 4 : v & 0xfff;
    }
    if (fat_type == 16)
        return fat[c * 2] | fat[c * 2 + 1] << 8;
    uint32_t v;
    memcpy(&v, fat + c * 4, 4);
    return v & FAT32_MASK;
}

static void fat_set(uint32_t c, uint32_t v)
{
    for (int n = 0; n < bpb->nfats; n++) {
        uint8_t *fat = sector(fat_start + (uint64_t)n * fat_sectors);
        if (fat_type == 12) {
            uint32_t off = c + c / 2;
            if (c & 1) {
                fat[off] = (uint8_t)((fat[off] & 0x0f) | (v << 4));
                fat[off + 1] = (uint8_t)(v >> 4);
            } else {
                fat[off] = (uint8_t)v;
                fat[off + 1] = (uint8_t)((fat[off + 1] & 0xf0) | ((v >> 8) & 0x0f));
            }
        } else if (fat_type == 16) {
            fat[c * 2] = (uint8_t)v;
            fat[c * 2 + 1] = (uint8_t)(v >> 8);
        } else {
            uint32_t old;
            memcpy(&old, fat + c * 4, 4);
            v = (old & ~FAT32_MASK) | (v & FAT32_MASK);
            memcpy(fat + c * 4, &v, 4);
        }
    }
}

static uint32_t eoc(void)
{
    return fat_type == 12 ? 0xfff : fat_type == 16 ? 0xffff : 0x0fffffff;
}

static int is_eoc(uint32_t v)
{
    return fat_type == 12 ? v >= FAT12_EOC : fat_type == 16 ? v >= FAT16_EOC : v >= FAT32_EOC;
}

static uint32_t alloc_cluster(uint32_t prev)
{
    for (uint32_t c = next_free; c < nclusters + 2; c++) {
        if (fat_get(c) == 0) {
            fat_set(c, eoc());
            if (prev)
                fat_set(prev, c);
            memset(cluster(c), 0, cluster_bytes());
            next_free = c + 1;
            return c;
        }
    }
    die("out of clusters");
}

/* ---- directories ---- */

/* A directory is either the FAT12/16 root region or a cluster chain. */
struct dir {
    uint32_t first_cluster;         /* 0 for the fixed root */
};

static uint32_t dir_entries(struct dir *d)
{
    if (d->first_cluster == 0)
        return bpb->root_entries;
    uint32_t n = 0;
    for (uint32_t c = d->first_cluster; !is_eoc(c); c = fat_get(c))
        n += cluster_bytes() / 32;
    return n;
}

static struct fat_dirent *dir_entry(struct dir *d, uint32_t index, int grow)
{
    if (d->first_cluster == 0) {
        if (index >= bpb->root_entries)
            die("root directory full");
        return (struct fat_dirent *)(sector(root_start) + index * 32);
    }
    uint32_t per = cluster_bytes() / 32;
    uint32_t c = d->first_cluster, prev = 0;
    for (uint32_t i = index / per; ; i--) {
        if (is_eoc(c)) {
            if (!grow)
                return NULL;
            c = alloc_cluster(prev);
        }
        if (i == 0)
            break;
        prev = c;
        c = fat_get(c);
    }
    return (struct fat_dirent *)(cluster(c) + (index % per) * 32);
}

static uint16_t fat_time(const struct tm *tm)
{
    return (uint16_t)(tm->tm_hour << 11 | tm->tm_min << 5 | tm->tm_sec / 2);
}

static uint16_t fat_date(const struct tm *tm)
{
    return (uint16_t)((tm->tm_year - 80) << 9 | (tm->tm_mon + 1) << 5 | tm->tm_mday);
}

static void stamp(struct fat_dirent *e)
{
    time_t now = time(NULL);
    struct tm *tm = gmtime(&now);
    e->ctime = e->mtime = fat_time(tm);
    e->cdate = e->mdate = e->adate = fat_date(tm);
}

/* Short name generation: upper case, invalid characters replaced, and a
 * ~N tail when the name does not fit or collides. */
static int short_char_ok(int c)
{
    if (c < 0x20 || c > 0x7e)
        return 0;
    return !strchr("\"*+,/:;<=>?[\\]| .", c);
}

static int name_is_plain_83(const char *name, uint8_t out[11])
{
    const char *dot = strrchr(name, '.');
    size_t base = dot ? (size_t)(dot - name) : strlen(name);
    size_t ext = dot ? strlen(dot + 1) : 0;
    if (base == 0 || base > 8 || ext > 3 || (dot && ext == 0))
        return 0;
    memset(out, ' ', 11);
    for (size_t i = 0; i < base; i++) {
        if (!short_char_ok((unsigned char)name[i]) || islower((unsigned char)name[i]))
            return 0;
        out[i] = (uint8_t)name[i];
    }
    for (size_t i = 0; i < ext; i++) {
        if (!short_char_ok((unsigned char)dot[1 + i]) || islower((unsigned char)dot[1 + i]))
            return 0;
        out[8 + i] = (uint8_t)dot[1 + i];
    }
    return 1;
}

static int short_name_used(struct dir *d, const uint8_t name[11])
{
    uint32_t n = dir_entries(d);
    for (uint32_t i = 0; i < n; i++) {
        struct fat_dirent *e = dir_entry(d, i, 0);
        if (!e || e->name[0] == FAT_NAME_FREE)
            break;
        if (e->name[0] != FAT_NAME_DELETED && e->attr != FAT_ATTR_LFN && memcmp(e->name, name, 11) == 0)
            return 1;
    }
    return 0;
}

static void make_short_name(struct dir *d, const char *name, uint8_t out[11])
{
    if (name_is_plain_83(name, out))
        return;
    const char *dot = strrchr(name, '.');
    if (dot == name)
        dot = NULL;
    char base[9] = "", ext[4] = "";
    size_t bl = 0, el = 0;
    for (const char *p = name; *p && (!dot || p < dot) && bl < 6; p++) {
        if (*p == '.' || *p == ' ')
            continue;
        base[bl++] = short_char_ok((unsigned char)*p) ? (char)toupper((unsigned char)*p) : '_';
    }
    if (dot)
        for (const char *p = dot + 1; *p && el < 3; p++)
            if (*p != ' ')
                ext[el++] = short_char_ok((unsigned char)*p) ? (char)toupper((unsigned char)*p) : '_';
    if (bl == 0)
        base[bl++] = '_';
    for (int n = 1; n < 1000000; n++) {
        char tail[8];
        snprintf(tail, sizeof tail, "~%d", n);
        size_t keep = 8 - strlen(tail);
        if (keep > bl)
            keep = bl;
        memset(out, ' ', 11);
        memcpy(out, base, keep);
        memcpy(out + keep, tail, strlen(tail));
        memcpy(out + 8, ext, el);
        if (!short_name_used(d, out))
            return;
    }
    die("cannot make a short name");
}

/* UTF-8 to UTF-16 (basic multilingual plane only). */
static size_t utf16_from_utf8(const char *s, uint16_t *out, size_t max)
{
    size_t n = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p && n < max) {
        uint32_t cp;
        if (*p < 0x80) {
            cp = *p++;
        } else if ((*p & 0xe0) == 0xc0 && (p[1] & 0xc0) == 0x80) {
            cp = ((p[0] & 0x1f) << 6) | (p[1] & 0x3f);
            p += 2;
        } else if ((*p & 0xf0) == 0xe0 && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80) {
            cp = ((p[0] & 0x0f) << 12) | ((p[1] & 0x3f) << 6) | (p[2] & 0x3f);
            p += 3;
        } else {
            cp = '_';
            p++;
        }
        out[n++] = (uint16_t)cp;
    }
    return n;
}

static void utf8_from_utf16(const uint16_t *s, size_t n, char *out, size_t max)
{
    size_t o = 0;
    for (size_t i = 0; i < n && o + 4 < max; i++) {
        uint32_t cp = s[i];
        if (cp < 0x80) {
            out[o++] = (char)cp;
        } else if (cp < 0x800) {
            out[o++] = (char)(0xc0 | cp >> 6);
            out[o++] = (char)(0x80 | (cp & 0x3f));
        } else {
            out[o++] = (char)(0xe0 | cp >> 12);
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3f));
            out[o++] = (char)(0x80 | (cp & 0x3f));
        }
    }
    out[o] = '\0';
}

/* Append an entry (with long name pieces when needed). */
static struct fat_dirent *add_entry(struct dir *d, const char *name, uint8_t attr, uint32_t first, uint32_t size)
{
    uint8_t shortname[11];
    make_short_name(d, name, shortname);
    uint16_t lfn[FAT_LFN_MAX + 1];
    size_t len = utf16_from_utf8(name, lfn, FAT_LFN_MAX);
    int need_lfn = !name_is_plain_83(name, (uint8_t[11]){0});
    uint32_t pieces = need_lfn ? (uint32_t)((len + FAT_LFN_CHARS - 1) / FAT_LFN_CHARS) : 0;

    /* Find the first free entry with pieces + 1 free entries after it. */
    uint32_t n = dir_entries(d), start = 0, run = 0;
    for (uint32_t i = 0; ; i++) {
        struct fat_dirent *e = i < n ? dir_entry(d, i, 0) : NULL;
        if (!e || e->name[0] == FAT_NAME_FREE) {
            if (run == 0)
                start = i;
            break;
        }
        if (e->name[0] == FAT_NAME_DELETED) {
            if (run == 0)
                start = i;
            if (++run == pieces + 1)
                break;
        } else {
            run = 0;
        }
    }
    uint8_t sum = fat_short_checksum(shortname);
    for (uint32_t p = 0; p < pieces; p++) {
        uint32_t seq = pieces - p;      /* stored highest first */
        struct fat_lfn *l = (struct fat_lfn *)dir_entry(d, start + p, 1);
        memset(l, 0xff, sizeof *l);
        l->sequence = (uint8_t)(seq | (p == 0 ? FAT_LFN_LAST : 0));
        l->attr = FAT_ATTR_LFN;
        l->type = 0;
        l->checksum = sum;
        l->cluster = 0;
        uint16_t chars[FAT_LFN_CHARS];
        for (int k = 0; k < FAT_LFN_CHARS; k++) {
            size_t idx = (seq - 1) * FAT_LFN_CHARS + (size_t)k;
            chars[k] = idx < len ? lfn[idx] : idx == len ? 0 : 0xffff;
        }
        memcpy(l->name1, chars, 10);
        memcpy(l->name2, chars + 5, 12);
        memcpy(l->name3, chars + 11, 4);
    }
    struct fat_dirent *e = dir_entry(d, start + pieces, 1);
    memset(e, 0, sizeof *e);
    memcpy(e->name, shortname, 11);
    e->attr = attr;
    e->cluster_lo = (uint16_t)first;
    e->cluster_hi = (uint16_t)(first >> 16);
    e->size = size;
    stamp(e);
    return e;
}

static uint32_t write_file_data(const uint8_t *data, uint64_t len)
{
    if (len == 0)
        return 0;
    uint32_t first = 0, prev = 0;
    for (uint64_t off = 0; off < len; off += cluster_bytes()) {
        uint32_t c = alloc_cluster(prev);
        if (!first)
            first = c;
        uint64_t n = len - off < cluster_bytes() ? len - off : cluster_bytes();
        memcpy(cluster(c), data + off, n);
        prev = c;
    }
    return first;
}

static void add_tree(struct dir *d, const char *path)
{
    DIR *dir = opendir(path);
    if (!dir)
        die(path);
    struct dirent *e;
    while ((e = readdir(dir))) {
        if (e->d_name[0] == '.')
            continue;
        char full[1024];
        snprintf(full, sizeof full, "%s/%s", path, e->d_name);
        struct stat st;
        if (stat(full, &st) < 0)
            die(full);
        if (S_ISDIR(st.st_mode)) {
            uint32_t c = alloc_cluster(0);
            struct fat_dirent *de = add_entry(d, e->d_name, FAT_ATTR_DIRECTORY, c, 0);
            struct dir sub = { c };
            struct fat_dirent *dot = dir_entry(&sub, 0, 1);
            memset(dot, 0, sizeof *dot);
            memcpy(dot->name, ".          ", 11);
            dot->attr = FAT_ATTR_DIRECTORY;
            dot->cluster_lo = (uint16_t)c;
            dot->cluster_hi = (uint16_t)(c >> 16);
            stamp(dot);
            struct fat_dirent *dotdot = dir_entry(&sub, 1, 1);
            memset(dotdot, 0, sizeof *dotdot);
            memcpy(dotdot->name, "..         ", 11);
            dotdot->attr = FAT_ATTR_DIRECTORY;
            stamp(dotdot);
            uint32_t parent = d->first_cluster == bpb->f32.root_cluster && fat_type == 32 ? 0 : d->first_cluster;
            dotdot->cluster_lo = (uint16_t)parent;
            dotdot->cluster_hi = (uint16_t)(parent >> 16);
            (void)de;
            add_tree(&sub, full);
        } else if (S_ISREG(st.st_mode)) {
            FILE *f = fopen(full, "rb");
            if (!f)
                die(full);
            uint8_t *data = malloc(st.st_size ? (size_t)st.st_size : 1);
            if (fread(data, 1, (size_t)st.st_size, f) != (size_t)st.st_size)
                die("short read");
            fclose(f);
            uint32_t first = write_file_data(data, (uint64_t)st.st_size);
            add_entry(d, e->d_name, FAT_ATTR_ARCHIVE, first, (uint32_t)st.st_size);
            free(data);
        }
    }
    closedir(dir);
}

/* ---- formatting ---- */

static void layout(void)
{
    spc = bpb->sectors_per_cluster;
    fat_start = bpb->reserved_sectors;
    fat_sectors = bpb->fat_size16 ? bpb->fat_size16 : bpb->f32.fat_size32;
    root_start = fat_start + bpb->nfats * fat_sectors;
    root_sectors = (bpb->root_entries * 32 + FAT_SECTOR_SIZE - 1) / FAT_SECTOR_SIZE;
    data_start = root_start + root_sectors;
    uint32_t total = bpb->total_sectors16 ? bpb->total_sectors16 : bpb->total_sectors32;
    nclusters = (total - data_start) / spc;
    if (fat_type == 0)
        fat_type = nclusters < FAT12_MAX_CLUSTERS ? 12 : nclusters < FAT16_MAX_CLUSTERS ? 16 : 32;
}

static void format(uint64_t size, int type)
{
    img_size = size;
    img = calloc(1, size);
    if (!img)
        die("out of memory");
    uint32_t total = (uint32_t)(size / FAT_SECTOR_SIZE);
    if (type == 0)
        type = size < 4u << 20 ? 12 : size < 256u << 20 ? 16 : 32;
    fat_type = type;
    /* Cluster size: the largest up to 4 KiB that keeps the cluster count
     * inside the type's range. */
    uint32_t want = 8;
    uint32_t min = type == 12 ? 1 : type == 16 ? FAT12_MAX_CLUSTERS + 16 : FAT16_MAX_CLUSTERS + 16;
    uint32_t max = type == 12 ? FAT12_MAX_CLUSTERS - 16 : type == 16 ? FAT16_MAX_CLUSTERS - 16 : 0x0ffffff0u;
    while (want > 1 && total / want < min)
        want /= 2;
    while (want < 128 && total / want > max)
        want *= 2;
    if (total / want < min)
        die(type == 16 ? "image too small for FAT16 (use -t 12)" : "image too small for FAT32 (use -t 16)");
    bpb = (struct fat_bpb *)img;
    bpb->jump[0] = 0xeb;
    bpb->jump[1] = 0x58;
    bpb->jump[2] = 0x90;
    memcpy(bpb->oem, "MINIOS  ", 8);
    bpb->bytes_per_sector = FAT_SECTOR_SIZE;
    bpb->sectors_per_cluster = (uint8_t)want;
    bpb->nfats = 2;
    bpb->media = 0xf8;
    bpb->sectors_per_track = 63;
    bpb->heads = 255;
    if (total < 0x10000)
        bpb->total_sectors16 = (uint16_t)total;
    else
        bpb->total_sectors32 = total;
    uint32_t clusters_max = total / want;
    if (type == 32) {
        bpb->reserved_sectors = 32;
        bpb->root_entries = 0;
        bpb->f32.fat_size32 = ((clusters_max + 2) * 4 + FAT_SECTOR_SIZE - 1) / FAT_SECTOR_SIZE;
        bpb->f32.root_cluster = 2;
        bpb->f32.fsinfo_sector = 1;
        bpb->f32.backup_boot_sector = 6;
        bpb->f32.drive = 0x80;
        bpb->f32.boot_signature = 0x29;
        bpb->f32.volume_id = 0x4d494e49;
        memcpy(bpb->f32.label, "MINIOS     ", 11);
        memcpy(bpb->f32.fs_type, "FAT32   ", 8);
    } else {
        bpb->reserved_sectors = 1;
        bpb->root_entries = 512;
        uint32_t bytes = type == 12 ? (clusters_max + 2) * 3 / 2 + 1 : (clusters_max + 2) * 2;
        bpb->fat_size16 = (uint16_t)((bytes + FAT_SECTOR_SIZE - 1) / FAT_SECTOR_SIZE);
        bpb->f16.drive = 0x80;
        bpb->f16.boot_signature = 0x29;
        bpb->f16.volume_id = 0x4d494e49;
        memcpy(bpb->f16.label, "MINIOS     ", 11);
        memcpy(bpb->f16.fs_type, type == 12 ? "FAT12   " : "FAT16   ", 8);
    }
    img[510] = 0x55;
    img[511] = 0xaa;
    layout();
    if (fat_type != type)
        die("cluster count does not match the requested type");
    /* Media byte and end marks in the first two FAT entries. */
    fat_set(0, 0x0fffff00u | bpb->media);
    fat_set(1, eoc());
    if (type == 32) {
        struct fat_fsinfo *fi = (struct fat_fsinfo *)sector(1);
        fi->lead_signature = FAT_FSINFO_LEAD;
        fi->struct_signature = FAT_FSINFO_STRUCT;
        fi->free_clusters = 0xffffffff;
        fi->next_free = 0xffffffff;
        fi->trail_signature = FAT_FSINFO_TRAIL;
        uint32_t root = alloc_cluster(0);
        if (root != 2)
            die("root cluster is not 2");
        memcpy(sector(6), sector(0), FAT_SECTOR_SIZE);
    }
}

static void load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        die(path);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    img_size = (uint64_t)size;
    img = malloc((size_t)size);
    if (!img || fread(img, 1, (size_t)size, f) != (size_t)size)
        die("cannot read image");
    fclose(f);
    bpb = (struct fat_bpb *)img;
    if (img[510] != 0x55 || img[511] != 0xaa || bpb->bytes_per_sector != FAT_SECTOR_SIZE)
        die("not a FAT image");
    fat_type = 0;
    layout();
}

/* ---- inspection ---- */

static struct dir root_dir(void)
{
    struct dir d = { fat_type == 32 ? bpb->f32.root_cluster : 0 };
    return d;
}

/* Iterate a directory, assembling long names. Returns the index after the
 * entry found or 0 at the end. */
static uint32_t next_entry(struct dir *d, uint32_t index, struct fat_dirent **out, char *name, size_t namesize)
{
    uint16_t lfn[FAT_LFN_MAX + FAT_LFN_CHARS];
    int have_lfn = 0;
    size_t lfn_len = 0;
    uint32_t n = dir_entries(d);
    for (; index < n; index++) {
        struct fat_dirent *e = dir_entry(d, index, 0);
        if (!e || e->name[0] == FAT_NAME_FREE)
            return 0;
        if (e->name[0] == FAT_NAME_DELETED) {
            have_lfn = 0;
            continue;
        }
        if (e->attr == FAT_ATTR_LFN) {
            struct fat_lfn *l = (struct fat_lfn *)e;
            uint32_t seq = l->sequence & 0x3f;
            if (seq == 0 || seq > FAT_LFN_ENTRIES_MAX) {
                have_lfn = 0;
                continue;
            }
            uint16_t chars[FAT_LFN_CHARS];
            memcpy(chars, l->name1, 10);
            memcpy(chars + 5, l->name2, 12);
            memcpy(chars + 11, l->name3, 4);
            memcpy(lfn + (seq - 1) * FAT_LFN_CHARS, chars, sizeof chars);
            if (l->sequence & FAT_LFN_LAST) {
                lfn_len = seq * FAT_LFN_CHARS;
                have_lfn = 1;
            }
            continue;
        }
        if (e->attr & FAT_ATTR_VOLUME) {
            have_lfn = 0;
            continue;
        }
        if (have_lfn && fat_short_checksum(e->name) == (((struct fat_lfn *)dir_entry(d, index - 1, 0))->checksum)) {
            size_t len = 0;
            while (len < lfn_len && lfn[len] != 0 && lfn[len] != 0xffff)
                len++;
            utf8_from_utf16(lfn, len, name, namesize);
        } else {
            size_t o = 0;
            for (int i = 0; i < 8 && e->name[i] != ' '; i++)
                name[o++] = (char)tolower(e->name[i]);
            if (e->name[8] != ' ') {
                name[o++] = '.';
                for (int i = 8; i < 11 && e->name[i] != ' '; i++)
                    name[o++] = (char)tolower(e->name[i]);
            }
            name[o] = '\0';
            if (name[0] == FAT_NAME_E5)
                name[0] = (char)0xe5;
        }
        *out = e;
        return index + 1;
    }
    return 0;
}

static uint32_t first_cluster_of(const struct fat_dirent *e)
{
    return e->cluster_lo | (fat_type == 32 ? (uint32_t)e->cluster_hi << 16 : 0);
}

static void dump_tree(struct dir *d, const char *prefix, int depth)
{
    uint32_t index = 0;
    struct fat_dirent *e;
    char name[FAT_LFN_MAX * 3 + 1];
    while ((index = next_entry(d, index, &e, name, sizeof name)) != 0) {
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        int isdir = e->attr & FAT_ATTR_DIRECTORY;
        printf("%s/%s%s cluster %u size %u\n", prefix, name, isdir ? "/" : "", first_cluster_of(e), e->size);
        if (isdir && depth < 16) {
            char sub[1024];
            snprintf(sub, sizeof sub, "%s/%s", prefix, name);
            struct dir sd = { first_cluster_of(e) };
            dump_tree(&sd, sub, depth + 1);
        }
    }
}

static int name_equal(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
    return *a == *b;
}

static struct fat_dirent *lookup(const char *path)
{
    struct dir d = root_dir();
    struct fat_dirent *found = NULL;
    char buf[1024];
    strncpy(buf, path, sizeof buf - 1);
    buf[sizeof buf - 1] = '\0';
    char *save = NULL;
    for (char *comp = strtok_r(buf, "/", &save); comp; comp = strtok_r(NULL, "/", &save)) {
        uint32_t index = 0;
        struct fat_dirent *e;
        char name[FAT_LFN_MAX * 3 + 1];
        found = NULL;
        while ((index = next_entry(&d, index, &e, name, sizeof name)) != 0) {
            if (name_equal(name, comp)) {
                found = e;
                break;
            }
        }
        if (!found)
            die("path not found");
        d.first_cluster = first_cluster_of(found);
    }
    return found;
}

static uint32_t count_free(void)
{
    uint32_t n = 0;
    for (uint32_t c = 2; c < nclusters + 2; c++)
        n += fat_get(c) == 0;
    return n;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--dump") == 0) {
        load(argv[2]);
        printf("FAT%d: %u clusters of %u bytes, %u free, %u sectors per FAT, root %s\n", fat_type,
               nclusters, cluster_bytes(), count_free(), fat_sectors,
               fat_type == 32 ? "in clusters" : "region");
        struct dir root = root_dir();
        dump_tree(&root, "", 0);
        return 0;
    }
    if (argc == 4 && strcmp(argv[1], "--cat") == 0) {
        load(argv[2]);
        struct fat_dirent *e = lookup(argv[3]);
        if (e->attr & FAT_ATTR_DIRECTORY)
            die("is a directory");
        uint32_t left = e->size;
        for (uint32_t c = first_cluster_of(e); left && !is_eoc(c) && c >= 2; c = fat_get(c)) {
            uint32_t n = left < cluster_bytes() ? left : cluster_bytes();
            fwrite(cluster(c), 1, n, stdout);
            left -= n;
        }
        return 0;
    }
    int type = 0, argi = 1;
    if (argc > 2 && strcmp(argv[1], "-t") == 0) {
        type = atoi(argv[2]);
        if (type != 12 && type != 16 && type != 32)
            die("type must be 12, 16 or 32");
        argi = 3;
    }
    if (argc - argi != 2 && argc - argi != 3) {
        fprintf(stderr, "usage: mkfat [-t 12|16|32] <image> <size_mb> [dir] | --dump <image> | --cat <image> <path>\n");
        return 2;
    }
    uint64_t mb = strtoull(argv[argi + 1], NULL, 10);
    if (mb < 1)
        die("size must be at least 1 MiB");
    format(mb << 20, type);
    if (argc - argi == 3) {
        struct dir root = root_dir();
        add_tree(&root, argv[argi + 2]);
    }
    if (fat_type == 32) {
        struct fat_fsinfo *fi = (struct fat_fsinfo *)sector(1);
        fi->free_clusters = count_free();
        fi->next_free = next_free;
    }
    FILE *f = fopen(argv[argi], "wb");
    if (!f)
        die(argv[argi]);
    if (fwrite(img, 1, img_size, f) != img_size)
        die("short write");
    fclose(f);
    printf("mkfat: %s: FAT%d, %u clusters of %u bytes, %u free\n", argv[argi], fat_type, nclusters,
           cluster_bytes(), count_free());
    return 0;
}
