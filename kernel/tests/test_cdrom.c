#include <tests/ktest.h>
#include <block/blockdev.h>
#include <block/bcache.h>
#include <fs/vfs.h>
#include <mm/slab.h>
#include <lib/cmdline.h>
#include <lib/printf.h>
#include <lib/string.h>
#include <console.h>
#include <errno.h>

/* R3 and R4: a CD drive with an ISO 9660 image, and one without a medium.
 * cddev= names the drive with the image (sr0 by default), and cdempty=
 * names an empty drive when the case attaches one. The image is the boot
 * CD of the case or an image of the harness file cd. The test reads the
 * volume descriptors, the last sector and the device file, and checks
 * that writes and reads past the end fail. */
static void test_cdrom(void)
{
    char name[BLOCKDEV_NAME_LEN] = "sr0", empty[BLOCKDEV_NAME_LEN] = "", path[32];
    if (!cmdline_lookup("cddev", name, sizeof name) || !name[0])
        strlcpy(name, "sr0", sizeof name);
    if (!cmdline_lookup("cdempty", empty, sizeof empty))
        empty[0] = '\0';
    struct blockdev *dev = blockdev_find(name);
    ktest_assert(dev != NULL, "no %s", name);
    ktest_assert((dev->flags & BLOCKDEV_CDROM) && (dev->flags & BLOCKDEV_READONLY), "%s flags 0x%x", name,
                 dev->flags);
    ktest_assert(dev->sector_size == 2048, "%s sector size %u", name, dev->sector_size);
    ktest_assert(dev->nsectors > 17, "%s capacity %lu", name, dev->nsectors);
    kprintf("cdrom: %s has %lu sectors\n", name, dev->nsectors);

    /* The primary volume descriptor at sector 16 and the terminator after
     * the descriptors. */
    uint8_t *b = kmalloc(65536);
    ktest_assert(b != NULL, "alloc");
    ktest_assert(blockdev_read(dev, 16, 1, b) == 0, "read sector 16");
    ktest_assert(b[0] == 1 && memcmp(b + 1, "CD001", 5) == 0, "no primary volume descriptor");
    bool terminator = false;
    for (uint64_t s = 17; s < 32 && !terminator; s++) {
        ktest_assert(blockdev_read(dev, s, 1, b) == 0, "read sector %lu", s);
        terminator = b[0] == 255 && memcmp(b + 1, "CD001", 5) == 0;
    }
    ktest_assert(terminator, "no volume descriptor set terminator");

    /* A transfer of many sectors, the last sector, and the end. */
    ktest_assert(blockdev_read(dev, 0, 32, b) == 0, "read 32 sectors");
    ktest_assert(blockdev_read(dev, dev->nsectors - 1, 1, b) == 0, "read the last sector");
    ktest_assert(blockdev_read(dev, dev->nsectors, 1, b) < 0, "read past the end");
    ktest_assert(blockdev_write(dev, 20, 1, b) == -EROFS, "write refused");

    /* The device file through the block cache, also in the last block. */
    struct file *f;
    ksnprintf(path, sizeof path, "/dev/%s", name);
    ktest_assert(vfs_open(path, O_RDONLY, 0, &f) == 0, "open %s", path);
    ktest_assert(f->inode->size == dev->nsectors * 2048 && S_ISBLK(f->inode->mode), "%s inode", path);
    ktest_assert(file_lseek(f, 16 * 2048 + 1, SEEK_SET) == 16 * 2048 + 1, "lseek");
    ktest_assert(file_read(f, (char *)b, 5) == 5 && memcmp(b, "CD001", 5) == 0, "read CD001 via the file");
    ktest_assert(file_lseek(f, (long)(dev->nsectors * 2048 - 100), SEEK_SET) > 0, "lseek to the end");
    ktest_assert(file_read(f, (char *)b, 1000) == 100, "read the last bytes");
    ktest_assert(file_read(f, (char *)b, 10) == 0, "eof");
    file_put(f);

    if (empty[0]) {
        struct blockdev *e = blockdev_find(empty);
        ktest_assert(e != NULL, "no %s", empty);
        ktest_assert(e->nsectors == 0, "%s reports %lu sectors without a medium", empty, e->nsectors);
        int r = blockdev_read(e, 0, 1, b);
        ktest_assert(r == -ENOMEDIUM, "%s read gave %d, expected ENOMEDIUM", empty, r);
        kprintf("cdrom: %s has no medium\n", empty);
    }
    kfree(b);
}
KTEST_DEFINE("cdrom", test_cdrom);
