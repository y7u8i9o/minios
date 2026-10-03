/* GUID partition tables (UEFI specification, chapter 5). part_scan reads
 * the table of every disk at boot, checks the CRCs of the header and of
 * the entry array, falls back to the backup header at the end of the disk
 * when the primary one is damaged, and registers every used entry as a
 * block device whose transfers go to the disk at an offset. part_rescan
 * reads the table of one disk again after a program wrote it (BLKRRPART).
 * The device of an entry number remains registered: a rescan updates it
 * and gives a partition that disappeared the size 0. /dev/partitions lists
 * the partitions of nonzero size, one line "name disk partuuid typeuuid
 * bytes" each.
 *
 * A partition and its disk have separate entries in the block cache, which
 * means a disk is not written as a whole while one of its partitions is
 * mounted, and a table is read again only when neither the root nor swap
 * lies on the disk. */
#define KLOG_SUBSYS "part"
#include <block/part.h>
#include <fs/devfs.h>
#include <lib/crc32.h>
#include <lib/printf.h>
#include <lib/string.h>
#include <mm/slab.h>
#include <boot.h>
#include <klog.h>
#include <errno.h>

#define GPT_MAX_ENTRY_BYTES (128 * 1024)

/* The partitions, their fields first, bdev.nsectors, type, uuid and
 * size_reported, the disks with a valid table with their disk GUIDs, and
 * the devices that mark a disk as busy. Protected by part_lock. */
static LIST_HEAD(partitions);
static DEFINE_SPINLOCK(part_lock);
#define PART_MAX_DISKS 16
static struct { struct blockdev *disk; uint8_t guid[16]; } tables[PART_MAX_DISKS];
static int ntables;
#define PART_MAX_BUSY 4
static struct blockdev *busy_disks[PART_MAX_BUSY];
static int nbusy;

/* GUIDs in their on-disk byte order: the first three groups are little
 * endian. */
#if defined(__x86_64__)
const uint8_t part_type_root[16] = { 0xe3, 0xbc, 0x68, 0x4f, 0xcd, 0xe8, 0xb1, 0x4d,
                                     0x96, 0xe7, 0xfb, 0xca, 0xf9, 0x84, 0xb7, 0x09 };
#else
const uint8_t part_type_root[16] = { 0x45, 0xb0, 0x21, 0xb9, 0xf0, 0x1d, 0xc3, 0x41,
                                     0xaf, 0x44, 0x4c, 0x6f, 0x28, 0x0d, 0x3f, 0xae };
#endif
const uint8_t part_type_swap[16] = { 0x6d, 0xfd, 0x57, 0x06, 0xab, 0xa4, 0xc4, 0x43,
                                     0x84, 0xe5, 0x09, 0x33, 0xc8, 0x4b, 0x4f, 0x4f };

struct gpt_header {
    char signature[8];
    uint32_t revision, header_size, header_crc, reserved;
    uint64_t my_lba, alternate_lba, first_usable, last_usable;
    uint8_t disk_guid[16];
    uint64_t entries_lba;
    uint32_t nentries, entry_size, entries_crc;
} __attribute__((packed));

struct gpt_entry {
    uint8_t type[16], uuid[16];
    uint64_t first, last, attributes;
    uint16_t name[36];
} __attribute__((packed));

static const char hexdigits[] = "0123456789abcdef";

void part_format_guid(const uint8_t g[16], char out[PART_GUID_STR])
{
    /* The order of the bytes in the string form. */
    static const int order[16] = { 3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15 };
    char *p = out;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            *p++ = '-';
        *p++ = hexdigits[g[order[i]] >> 4];
        *p++ = hexdigits[g[order[i]] & 15];
    }
    *p = '\0';
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

int part_parse_guid(const char *s, uint8_t out[16])
{
    static const int order[16] = { 3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15 };
    if (strlen(s) != 36)
        return -EINVAL;
    for (int i = 0, at = 0; i < 16; i++) {
        if (at == 8 || at == 13 || at == 18 || at == 23) {
            if (s[at] != '-')
                return -EINVAL;
            at++;
        }
        int hi = hexval(s[at]), lo = hexval(s[at + 1]);
        if (hi < 0 || lo < 0)
            return -EINVAL;
        out[order[i]] = (uint8_t)(hi << 4 | lo);
        at += 2;
    }
    return 0;
}

static bool guid_zero(const uint8_t g[16])
{
    for (int i = 0; i < 16; i++)
        if (g[i])
            return false;
    return true;
}

/* Read and check the header at lba and its entry array. Returns the array,
 * allocated, or NULL. */
static struct gpt_entry *read_table(struct blockdev *disk, uint64_t lba, struct gpt_header *h)
{
    uint32_t ss = disk->sector_size;
    if (ss < sizeof *h || lba >= disk->nsectors)
        return NULL;
    uint8_t *sector = kmalloc(ss);
    if (!sector)
        return NULL;
    struct gpt_entry *entries = NULL;
    if (blockdev_read(disk, lba, 1, sector) < 0)
        goto out;
    memcpy(h, sector, sizeof *h);
    if (memcmp(h->signature, "EFI PART", 8) != 0 || h->header_size < sizeof *h || h->header_size > ss ||
        h->my_lba != lba || h->entry_size < sizeof(struct gpt_entry) || h->entry_size % 8 ||
        (uint64_t)h->nentries * h->entry_size > GPT_MAX_ENTRY_BYTES || h->nentries == 0)
        goto out;
    uint32_t crc = h->header_crc;
    memset(sector + 16, 0, 4);
    if (crc32(0, sector, h->header_size) != crc)
        goto out;
    size_t bytes = (size_t)h->nentries * h->entry_size;
    uint32_t sectors = (uint32_t)((bytes + ss - 1) / ss);
    if (h->entries_lba + sectors > disk->nsectors || !(entries = kmalloc((size_t)sectors * ss)))
        goto out;
    if (blockdev_read(disk, h->entries_lba, sectors, entries) < 0 || crc32(0, entries, bytes) != h->entries_crc) {
        kfree(entries);
        entries = NULL;
    }
out:
    kfree(sector);
    return entries;
}

static int part_rw(struct blockdev *dev, uint64_t sector, uint32_t count, void *buf, bool write)
{
    struct partition *p = container_of(dev, struct partition, bdev);
    spin_lock(&part_lock);
    uint64_t first = p->first, size = dev->nsectors;
    spin_unlock(&part_lock);
    if (sector + count > size || sector + count < sector)
        return -EIO;
    return dev->disk->rw(dev->disk, first + sector, count, buf, write);
}

static int part_flush(struct blockdev *dev)
{
    struct blockdev *disk = dev->disk;
    return disk->flush ? disk->flush(disk) : 0;
}

static struct partition *find_index(struct blockdev *disk, int index)
{
    struct list_head *pos;
    list_for_each(pos, &partitions) {
        struct partition *p = list_entry(pos, struct partition, link);
        if (p->bdev.disk == disk && p->index == index)
            return p;
    }
    return NULL;
}

static void set_table(struct blockdev *disk, const uint8_t guid[16])
{
    int i = 0;
    while (i < ntables && tables[i].disk != disk)
        i++;
    if (!guid) {
        if (i < ntables)
            tables[i] = tables[--ntables];
        return;
    }
    if (i == ntables && ntables == PART_MAX_DISKS)
        return;
    if (i == ntables)
        ntables++;
    tables[i].disk = disk;
    memcpy(tables[i].guid, guid, 16);
}

/* Register or update the partitions of disk from its table, or empty all
 * of them when it has none. Returns the number of used entries. */
static int apply_table(struct blockdev *disk)
{
    struct gpt_header h;
    struct gpt_entry *entries = read_table(disk, 1, &h);
    if (!entries && (entries = read_table(disk, disk->nsectors - 1, &h)))
        klog_warn("%s: the primary partition table is damaged, using the backup", disk->name);
    spin_lock(&part_lock);
    set_table(disk, entries ? h.disk_guid : NULL);
    struct list_head *pos;
    list_for_each(pos, &partitions) {
        struct partition *p = list_entry(pos, struct partition, link);
        if (p->bdev.disk == disk)
            p->bdev.nsectors = 0;
    }
    spin_unlock(&part_lock);
    int count = 0;
    for (uint32_t i = 0; entries && i < h.nentries; i++) {
        const struct gpt_entry *e = (const void *)((const uint8_t *)entries + (size_t)i * h.entry_size);
        if (guid_zero(e->type))
            continue;
        if (e->first < h.first_usable || e->last > h.last_usable || e->first > e->last) {
            klog_warn("%s: entry %u lies outside the usable sectors", disk->name, i + 1);
            continue;
        }
        count++;
        spin_lock(&part_lock);
        struct partition *p = find_index(disk, (int)i + 1);
        if (p) {
            p->first = e->first;
            p->bdev.nsectors = e->last - e->first + 1;
            memcpy(p->type, e->type, 16);
            memcpy(p->uuid, e->uuid, 16);
        }
        spin_unlock(&part_lock);
        if (p)
            continue;
        if (!(p = kzalloc(sizeof *p)))
            break;
        ksnprintf(p->bdev.name, sizeof p->bdev.name, "%s%u", disk->name, i + 1);
        p->bdev.sector_size = disk->sector_size;
        p->bdev.nsectors = e->last - e->first + 1;
        p->bdev.rw = part_rw;
        p->bdev.flush = part_flush;
        p->bdev.disk = disk;
        p->first = e->first;
        p->index = (int)i + 1;
        memcpy(p->type, e->type, 16);
        memcpy(p->uuid, e->uuid, 16);
        spin_lock(&part_lock);
        list_add_tail(&p->link, &partitions);
        spin_unlock(&part_lock);
        if (blockdev_register(&p->bdev) < 0)
            klog_warn("%s: cannot register", p->bdev.name);
    }
    kfree(entries);
    if (entries || count)
        klog_info("%s: GPT with %d partitions", disk->name, count);
    return count;
}

/* The devices of the partitions of disk report their current sizes. */
static void update_sizes(struct blockdev *disk)
{
    struct list_head *pos;
    for (;;) {
        /* devfs_set_size takes an inode mutex, which may not be acquired
         * while part_lock is, and the walk therefore restarts for each. */
        struct partition *next = NULL;
        spin_lock(&part_lock);
        list_for_each(pos, &partitions) {
            struct partition *p = list_entry(pos, struct partition, link);
            if (p->bdev.disk == disk && p->size_reported != blockdev_size(&p->bdev)) {
                next = p;
                p->size_reported = blockdev_size(&p->bdev);
                break;
            }
        }
        spin_unlock(&part_lock);
        if (!next)
            return;
        devfs_set_size(next->bdev.name, next->size_reported);
    }
}

/* The text of /dev/partitions, made when the file is opened. */
struct listing { char *text; size_t len; };

static int listing_open(struct inode *ino, struct file *f)
{
    struct listing *l = kzalloc(sizeof *l);
    size_t size = 128;
    struct list_head *pos;
    spin_lock(&part_lock);
    list_for_each(pos, &partitions)
        size += 128;
    spin_unlock(&part_lock);
    if (!l || !(l->text = kmalloc(size))) {
        kfree(l);
        return -ENOMEM;
    }
    spin_lock(&part_lock);
    list_for_each(pos, &partitions) {
        struct partition *p = list_entry(pos, struct partition, link);
        char uuid[PART_GUID_STR], type[PART_GUID_STR];
        if (!p->bdev.nsectors)
            continue;
        part_format_guid(p->uuid, uuid);
        part_format_guid(p->type, type);
        int n = ksnprintf(l->text + l->len, size - l->len, "%s %s %s %s %lu\n", p->bdev.name, p->bdev.disk->name,
                          uuid, type, (unsigned long)blockdev_size(&p->bdev));
        if (n > 0 && (size_t)n < size - l->len)
            l->len += (size_t)n;
    }
    spin_unlock(&part_lock);
    f->priv = l;
    return 0;
}

static long listing_read(struct file *f, char *buf, size_t n, uint64_t *pos)
{
    struct listing *l = f->priv;
    if (*pos >= l->len)
        return 0;
    n = MIN(n, l->len - (size_t)*pos);
    memcpy(buf, l->text + *pos, n);
    *pos += n;
    return (long)n;
}

static void listing_release(struct file *f)
{
    struct listing *l = f->priv;
    kfree(l->text);
    kfree(l);
}

static const struct file_ops listing_fops = {
    .open = listing_open, .read = listing_read, .release = listing_release,
};

void part_scan(void)
{
    struct blockdev *devs[PART_MAX_DISKS];
    int n = blockdev_list(devs, PART_MAX_DISKS);
    for (int i = 0; i < n; i++)
        if (!devs[i]->disk) {
            apply_table(devs[i]);
            update_sizes(devs[i]);
        }
    devfs_register("partitions", S_IFCHR | 0444, &listing_fops, NULL, 0);
}

void part_hold(struct blockdev *dev)
{
    if (!dev)
        return;
    spin_lock(&part_lock);
    if (nbusy < PART_MAX_BUSY)
        busy_disks[nbusy++] = dev;
    spin_unlock(&part_lock);
}

int part_rescan(struct blockdev *disk)
{
    spin_lock(&part_lock);
    bool busy = false;
    for (int i = 0; i < nbusy; i++)
        if (busy_disks[i] == disk || busy_disks[i]->disk == disk)
            busy = true;
    spin_unlock(&part_lock);
    if (busy)
        return -EBUSY;
    apply_table(disk);
    update_sizes(disk);
    return 0;
}

struct blockdev *part_boot_disk(void)
{
    struct blockdev *found = NULL;
    if (guid_zero(bootinfo.boot_disk_guid))
        return NULL;
    spin_lock(&part_lock);
    for (int i = 0; i < ntables && !found; i++)
        if (memcmp(tables[i].guid, bootinfo.boot_disk_guid, 16) == 0)
            found = tables[i].disk;
    spin_unlock(&part_lock);
    return found;
}

bool part_has_table(struct blockdev *disk)
{
    bool found = false;
    spin_lock(&part_lock);
    for (int i = 0; i < ntables && !found; i++)
        found = tables[i].disk == disk;
    spin_unlock(&part_lock);
    return found;
}

struct partition *part_find_type(struct blockdev *disk, const uint8_t type[16])
{
    struct list_head *pos;
    struct partition *found = NULL;
    spin_lock(&part_lock);
    list_for_each(pos, &partitions) {
        struct partition *p = list_entry(pos, struct partition, link);
        if (p->bdev.disk == disk && p->bdev.nsectors && memcmp(p->type, type, 16) == 0) {
            found = p;
            break;
        }
    }
    spin_unlock(&part_lock);
    return found;
}

struct partition *part_find_uuid(const uint8_t uuid[16])
{
    struct list_head *pos;
    struct partition *found = NULL;
    spin_lock(&part_lock);
    list_for_each(pos, &partitions) {
        struct partition *p = list_entry(pos, struct partition, link);
        if (p->bdev.nsectors && memcmp(p->uuid, uuid, 16) == 0) {
            found = p;
            break;
        }
    }
    spin_unlock(&part_lock);
    return found;
}
