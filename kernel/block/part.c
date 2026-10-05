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
#include <drivers/devinfo.h>
#include <block/part.h>
#include <lib/guid.h>
#include <fs/devfs.h>
#include <fs/vfs.h>
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
        if (guid_is_zero(e->type))
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
        /* The name of a disk that ends in a digit, as nvme0n1, takes a p
         * before the number of the partition. */
        size_t dl = strlen(disk->name);
        bool digit = dl && disk->name[dl - 1] >= '0' && disk->name[dl - 1] <= '9';
        ksnprintf(p->bdev.name, sizeof p->bdev.name, "%s%s%u", disk->name, digit ? "p" : "", i + 1);
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
        char uuid[GUID_STR], type[GUID_STR];
        if (!p->bdev.nsectors)
            continue;
        guid_format(p->uuid, uuid);
        guid_format(p->type, type);
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

/* Set once part_scan has read the disks registered at boot. Disks
 * registered later read their table in part_add_disk. */
static bool scanned;

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
    __atomic_store_n(&scanned, true, __ATOMIC_RELEASE);
}

void part_add_disk(struct blockdev *disk)
{
    if (disk->disk || (disk->flags & BLOCKDEV_CDROM) || !__atomic_load_n(&scanned, __ATOMIC_ACQUIRE))
        return;
    apply_table(disk);
    update_sizes(disk);
}

void part_retain(struct blockdev *dev)
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
    if (guid_is_zero(bootinfo.boot_disk_guid))
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

/* ---- /dev/devices (docs/design/sysinfo.md) ---- */

static const char *type_name(const char *guid)
{
    static const struct { const char *guid, *name; } types[] = {
        { "c12a7328-f81f-11d2-ba4b-00a0c93ec93b", "EFI system" },
        { "21686148-6449-6e6f-744e-656564454649", "BIOS boot" },
        { "0657fd6d-a4ab-43c4-84e5-0933c84b4f4f", "swap" },
        { "4f68bce3-e8cd-4db1-96e7-fbcaf984b709", "root (x86_64)" },
        { "b921b045-1df0-41c3-af44-4c6f280d3fae", "root (aarch64)" },
        { "0fc63daf-8483-4772-8e79-3d69d8477de4", "Linux filesystem data" },
        { "933ac7e1-2eb4-4f13-b844-0e14e2aef915", "home" },
        { "ebd0a0a2-b9e5-4433-87c0-68b6b72699c7", "Microsoft basic data" },
        { "e3c9e316-0b5c-4db8-817d-f92df00215ae", "Microsoft reserved" },
        { "6d696e69-6f73-4e70-6b67-7265706f7369", "minios package repository" },
    };
    for (size_t i = 0; i < sizeof types / sizeof types[0]; i++)
        if (strcmp(types[i].guid, guid) == 0)
            return types[i].name;
    return "unknown";
}

/* The mounted filesystems, copied by vfs_for_each_mount before part_lock is
 * taken, since a statfs may sleep. */
#define DESCRIBE_MOUNTS 32

struct mount_copy {
    char path[64], source[64], type[16];
    struct fs_space space;
};

struct mount_list {
    struct mount_copy *m;
    unsigned n;
};

static void copy_mount(const struct mount_info *m, void *arg)
{
    struct mount_list *l = arg;
    if (l->n == DESCRIBE_MOUNTS)
        return;
    struct mount_copy *c = &l->m[l->n++];
    strlcpy(c->path, m->path, sizeof c->path);
    /* A device source may be written with /dev/ in front. */
    strlcpy(c->source, strncmp(m->source, "/dev/", 5) == 0 ? m->source + 5 : m->source, sizeof c->source);
    strlcpy(c->type, m->type, sizeof c->type);
    c->space = m->space;
}

/* The filesystem and the mount points of the block device name. */
static void describe_mounted(struct devinfo *d, const struct mount_list *l, const char *name)
{
    char points[128] = "";
    const char *type = NULL;
    for (unsigned i = 0; i < l->n; i++) {
        if (strcmp(l->m[i].source, name) != 0)
            continue;
        type = l->m[i].type;
        devinfo_append(points, sizeof points, ", ", l->m[i].path);
    }
    if (type) {
        devinfo_prop(d, "filesystem", "%s", type);
        devinfo_prop(d, "mount_point", "%s", points);
    }
}

static void describe_filesystems(struct devinfo *d, const struct mount_list *l)
{
    devinfo_node(d, "storage/filesystems", "Mounted filesystems");
    devinfo_prop(d, "mounts", "%u", l->n);
    for (unsigned i = 0; i < l->n; i++) {
        const struct mount_copy *m = &l->m[i];
        char path[48];
        ksnprintf(path, sizeof path, "storage/filesystems/%u", i);
        devinfo_node(d, path, "%s: %s%s%s", m->path, m->type, m->source[0] ? " on " : "", m->source);
        devinfo_prop(d, "mount_point", "%s", m->path);
        devinfo_prop(d, "filesystem", "%s", m->type);
        devinfo_prop(d, "source", "%s", m->source[0] ? m->source : "none");
        if (!m->space.block_size)
            continue;
        uint64_t total = m->space.blocks * m->space.block_size;
        uint64_t free = m->space.free_blocks * m->space.block_size;
        devinfo_prop(d, "block_size", "%u bytes", m->space.block_size);
        devinfo_size(d, "size", total);
        devinfo_size(d, "used", total - free);
        devinfo_size(d, "available", free);
        if (total)
            devinfo_prop(d, "use", "%lu%%", (unsigned long)((total - free) * 100 / total));
    }
}

/* The disks and their partitions. Block devices are never removed, so the
 * list from blockdev_list remains valid. The partitions and the tables
 * are read under part_lock, which a rescan takes to change them. */
void block_describe(struct devinfo *d)
{
    struct mount_copy *copies = kzalloc(DESCRIBE_MOUNTS * sizeof *copies);
    struct mount_list mounts = { copies, 0 };
    if (copies)
        vfs_for_each_mount(copy_mount, &mounts);
    struct blockdev *devs[64];
    int n = blockdev_list(devs, 64);
    unsigned disks = 0;
    for (int i = 0; i < n; i++)
        disks += devs[i]->disk == NULL;
    devinfo_node(d, "storage", "Storage");
    devinfo_prop(d, "disks", "%u", disks);
    for (int i = 0; i < n; i++) {
        struct blockdev *disk = devs[i];
        if (disk->disk)
            continue;
        char path[48], guid[GUID_STR];
        ksnprintf(path, sizeof path, "storage/%s", disk->name);
        devinfo_node(d, path, "%s, %lu MiB", disk->name, (unsigned long)(blockdev_size(disk) >> 20));
        devinfo_prop(d, "device_node", "/dev/%s", disk->name);
        devinfo_size(d, "size", blockdev_size(disk));
        devinfo_prop(d, "sector_size", "%u bytes", disk->sector_size);
        devinfo_prop(d, "sectors", "%lu", (unsigned long)disk->nsectors);
        devinfo_prop(d, "driver", "%s", strncmp(disk->name, "vd", 2) == 0 ? "virtio-blk" : "unknown");
        spin_lock(&part_lock);
        int t = 0;
        while (t < ntables && tables[t].disk != disk)
            t++;
        bool boot = t < ntables && !guid_is_zero(bootinfo.boot_disk_guid) &&
                    memcmp(tables[t].guid, bootinfo.boot_disk_guid, 16) == 0;
        if (t < ntables) {
            guid_format(tables[t].guid, guid);
            devinfo_prop(d, "partition_table", "GPT");
            devinfo_prop(d, "disk_guid", "%s", guid);
        } else {
            devinfo_prop(d, "partition_table", "none");
        }
        devinfo_prop(d, "boot_disk", "%s", boot ? "yes" : "no");
        describe_mounted(d, &mounts, disk->name);
        struct list_head *pos;
        list_for_each(pos, &partitions) {
            struct partition *p = list_entry(pos, struct partition, link);
            if (p->bdev.disk != disk || !p->bdev.nsectors)
                continue;
            char ppath[64], type[GUID_STR];
            guid_format(p->type, type);
            guid_format(p->uuid, guid);
            ksnprintf(ppath, sizeof ppath, "%s/%s", path, p->bdev.name);
            devinfo_node(d, ppath, "%s, %s, %lu MiB", p->bdev.name, type_name(type),
                         (unsigned long)(blockdev_size(&p->bdev) >> 20));
            devinfo_prop(d, "device_node", "/dev/%s", p->bdev.name);
            devinfo_prop(d, "entry", "%d", p->index);
            devinfo_prop(d, "partition_type", "%s (%s)", type_name(type), type);
            devinfo_prop(d, "unique_guid", "%s", guid);
            devinfo_prop(d, "first_sector", "%lu", (unsigned long)p->first);
            devinfo_prop(d, "sectors", "%lu", (unsigned long)p->bdev.nsectors);
            devinfo_size(d, "size", blockdev_size(&p->bdev));
            devinfo_prop(d, "boot_partition", "%s",
                         memcmp(p->uuid, bootinfo.boot_part_guid, 16) == 0 ? "yes" : "no");
            describe_mounted(d, &mounts, p->bdev.name);
        }
        spin_unlock(&part_lock);
    }
    describe_filesystems(d, &mounts);
    kfree(copies);
}
