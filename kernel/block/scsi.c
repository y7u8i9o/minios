/* SCSI disks and CD drives (R3 of docs/plan/release-0.5.0.md,
 * docs/design/block.md), written from the SCSI Primary Commands (SPC-4),
 * SCSI Block Commands (SBC-3) and Multimedia Commands (MMC-6)
 * specifications. A transport, the ATAPI path of the AHCI driver or USB
 * mass storage, carries the command blocks. This module identifies the
 * device, follows the presence and the changes of a removable medium, and
 * turns block transfers into READ and WRITE commands. */
#define KLOG_SUBSYS "scsi"
#include <block/scsi.h>
#include <block/bcache.h>
#include <fs/devfs.h>
#include <drivers/timer.h>
#include <mm/slab.h>
#include <lib/printf.h>
#include <lib/string.h>
#include <klog.h>
#include <errno.h>

#define CMD_TEST_UNIT_READY     0x00
#define CMD_REQUEST_SENSE       0x03
#define CMD_INQUIRY             0x12
#define CMD_READ_CAPACITY_10    0x25
#define CMD_READ_10             0x28
#define CMD_WRITE_10            0x2a
#define CMD_SYNC_CACHE_10       0x35
#define CMD_READ_16             0x88
#define CMD_WRITE_16            0x8a
#define CMD_SERVICE_ACTION_IN   0x9e    /* with service action 0x10, READ CAPACITY (16) */

#define KEY_NOT_READY           0x2
#define KEY_ILLEGAL_REQUEST     0x5
#define KEY_UNIT_ATTENTION      0x6
#define ASC_NOT_READY           0x04
#define ASC_MEDIUM_NOT_PRESENT  0x3a

#define TYPE_DISK               0x00
#define TYPE_CDROM              0x05

#define READY_TIMEOUT_MS        10000   /* a drive that reports "becoming ready" */

/* Run one command. CHECK CONDITION is followed by REQUEST SENSE, and the
 * sense data become an errno: ENOMEDIUM for a missing medium, EAGAIN for
 * a unit attention, EBUSY for a unit that is becoming ready and EIO for
 * the rest. *key receives the sense key, 0 without one. The caller has
 * locked sd->lock. */
static int run(struct scsi_device *sd, const uint8_t *cdb, unsigned cdb_len, void *buf, uint32_t len, bool write,
               uint8_t *key)
{
    if (key)
        *key = 0;
    int r = sd->command(sd->ctx, cdb, cdb_len, buf, len, write);
    if (r != SCSI_CHECK_CONDITION)
        return r;
    uint8_t sense[18] = { 0 };
    const uint8_t rs[6] = { CMD_REQUEST_SENSE, 0, 0, 0, sizeof sense, 0 };
    r = sd->command(sd->ctx, rs, sizeof rs, sense, sizeof sense, false);
    if (r != 0)
        return r < 0 ? r : -EIO;
    /* Fixed format (response codes 70h and 71h) or descriptor format (72h
     * and 73h). */
    uint8_t k, asc, ascq;
    if ((sense[0] & 0x7f) >= 0x72) {
        k = sense[1] & 0xf;
        asc = sense[2];
        ascq = sense[3];
    } else {
        k = sense[2] & 0xf;
        asc = sense[12];
        ascq = sense[13];
    }
    if (key)
        *key = k;
    if (k == KEY_NOT_READY && asc == ASC_MEDIUM_NOT_PRESENT)
        return -ENOMEDIUM;
    if (k == KEY_NOT_READY && asc == ASC_NOT_READY)
        return -EBUSY;
    if (k == KEY_UNIT_ATTENTION)
        return -EAGAIN;
    if (k != KEY_ILLEGAL_REQUEST || cdb[0] != CMD_SYNC_CACHE_10)
        klog_warn("%s: command %02x: sense key %x, code %02x/%02x", sd->bdev.name, cdb[0], k, asc, ascq);
    return -EIO;
}

/* Find out whether a medium is present and read its capacity. A unit
 * attention, which follows a reset or a change of the medium, is
 * repeated, and a unit that is becoming ready is waited for. The caller
 * has locked sd->lock. */
static int update_medium(struct scsi_device *sd)
{
    uint64_t old = sd->bdev.nsectors;
    uint64_t end = timer_ms() + READY_TIMEOUT_MS;
    int r;
    for (int attentions = 0;;) {
        const uint8_t tur[6] = { CMD_TEST_UNIT_READY };
        r = run(sd, tur, sizeof tur, NULL, 0, false, NULL);
        if (r == -EAGAIN && ++attentions < 8)
            continue;
        if (r == -EBUSY && timer_ms() < end) {
            sleep_ms(100);
            continue;
        }
        break;
    }
    uint64_t sectors = 0;
    uint32_t block = 0;
    if (r == 0) {
        uint8_t cap[32];
        const uint8_t rc10[10] = { CMD_READ_CAPACITY_10 };
        r = run(sd, rc10, sizeof rc10, cap, 8, false, NULL);
        if (r == 0) {
            uint32_t last = (uint32_t)cap[0] << 24 | (uint32_t)cap[1] << 16 | (uint32_t)cap[2] << 8 | cap[3];
            block = (uint32_t)cap[4] << 24 | (uint32_t)cap[5] << 16 | (uint32_t)cap[6] << 8 | cap[7];
            sectors = (uint64_t)last + 1;
            if (last == 0xffffffffu) {
                const uint8_t rc16[16] = { CMD_SERVICE_ACTION_IN, 0x10, [13] = 32 };
                r = run(sd, rc16, sizeof rc16, cap, 32, false, NULL);
                uint64_t last64 = 0;
                for (int i = 0; i < 8; i++)
                    last64 = last64 << 8 | cap[i];
                block = (uint32_t)cap[8] << 24 | (uint32_t)cap[9] << 16 | (uint32_t)cap[10] << 8 | cap[11];
                sectors = last64 + 1;
            }
        }
        if (r == 0 && block != sd->bdev.sector_size) {
            klog_warn("%s: medium with %u byte blocks, the device uses %u", sd->bdev.name, block,
                      sd->bdev.sector_size);
            r = -EIO;
        }
    }
    sd->medium = r == 0;
    sd->bdev.nsectors = r == 0 ? sectors : 0;
    if (sd->bdev.nsectors != old) {
        bcache_discard(&sd->bdev);
        devfs_set_size(sd->bdev.name, blockdev_size(&sd->bdev));
        if (sd->medium)
            klog_info("%s: medium of %lu sectors (%lu MiB)", sd->bdev.name, sectors,
                      (sectors * sd->bdev.sector_size) >> 20);
        else
            klog_info("%s: no medium", sd->bdev.name);
    }
    return r == -EBUSY || r == -EAGAIN ? -EIO : r;
}

/* One READ or WRITE command, READ (10) where the address and the count
 * fit, else READ (16). */
static int transfer(struct scsi_device *sd, uint64_t sector, uint32_t n, void *buf, bool write)
{
    uint8_t cdb[16] = { 0 };
    unsigned len;
    if (sector + n <= 0xffffffffu && n <= 0xffff) {
        cdb[0] = write ? CMD_WRITE_10 : CMD_READ_10;
        for (int i = 0; i < 4; i++)
            cdb[2 + i] = (uint8_t)(sector >> (24 - 8 * i));
        cdb[7] = (uint8_t)(n >> 8);
        cdb[8] = (uint8_t)n;
        len = 10;
    } else {
        cdb[0] = write ? CMD_WRITE_16 : CMD_READ_16;
        for (int i = 0; i < 8; i++)
            cdb[2 + i] = (uint8_t)(sector >> (56 - 8 * i));
        for (int i = 0; i < 4; i++)
            cdb[10 + i] = (uint8_t)(n >> (24 - 8 * i));
        len = 16;
    }
    return run(sd, cdb, len, buf, n * sd->bdev.sector_size, write, NULL);
}

static int scsi_rw(struct blockdev *dev, uint64_t sector, uint32_t count, void *buf, bool write)
{
    struct scsi_device *sd = dev->priv;
    int r = 0;
    mutex_lock(&sd->lock);
    if (sd->gone) {
        r = -ENODEV;
        goto out;
    }
    if (write && (dev->flags & BLOCKDEV_READONLY)) {
        r = -EROFS;
        goto out;
    }
    /* A drive without a medium is asked again, since one may have been
     * inserted. */
    if (!sd->medium && (r = update_medium(sd)) < 0)
        goto out;
    if (sector + count > dev->nsectors || sector + count < sector) {
        r = -EINVAL;
        goto out;
    }
    uint32_t per = MAX(sd->max_transfer / dev->sector_size, 1u);
    while (count) {
        uint32_t n = MIN(count, per);
        r = transfer(sd, sector, n, buf, write);
        /* A unit attention reports a reset or a new medium. The command is
         * repeated once when the capacity did not change. */
        if (r == -EAGAIN) {
            uint64_t before = dev->nsectors;
            if (update_medium(sd) == 0 && dev->nsectors == before)
                r = transfer(sd, sector, n, buf, write);
            else
                r = -EIO;
        }
        if (r < 0)
            break;
        buf = (uint8_t *)buf + (size_t)n * dev->sector_size;
        sector += n;
        count -= n;
    }
out:
    mutex_unlock(&sd->lock);
    return r;
}

static int scsi_flush(struct blockdev *dev)
{
    struct scsi_device *sd = dev->priv;
    if (dev->flags & BLOCKDEV_READONLY)
        return 0;
    mutex_lock(&sd->lock);
    int r = -ENODEV;
    if (!sd->gone) {
        uint8_t key;
        const uint8_t cdb[10] = { CMD_SYNC_CACHE_10 };
        r = run(sd, cdb, sizeof cdb, NULL, 0, false, &key);
        /* A device without a write cache may refuse the command. */
        if (r == -EIO && key == KEY_ILLEGAL_REQUEST)
            r = 0;
    }
    mutex_unlock(&sd->lock);
    return r;
}

static void copy_ident(char *dst, const uint8_t *src, size_t len)
{
    memcpy(dst, src, len);
    dst[len] = '\0';
    while (len && (dst[len - 1] == ' ' || dst[len - 1] == '\0'))
        dst[--len] = '\0';
}

struct scsi_device *scsi_attach(scsi_command_fn command, void *ctx, uint32_t max_transfer, const char *bus)
{
    struct scsi_device *sd = kzalloc(sizeof *sd);
    if (!sd)
        return NULL;
    sd->command = command;
    sd->ctx = ctx;
    sd->max_transfer = max_transfer;
    sd->bus = bus;
    mutex_init(&sd->lock, "scsi_device");
    uint8_t inq[36] = { 0 };
    const uint8_t cdb[6] = { CMD_INQUIRY, 0, 0, 0, sizeof inq, 0 };
    mutex_lock(&sd->lock);
    int r = run(sd, cdb, sizeof cdb, inq, sizeof inq, false, NULL);
    mutex_unlock(&sd->lock);
    if (r != 0 || (inq[0] >> 5) != 0) {
        klog_warn("%s device: no INQUIRY data (%d)", bus, r);
        kfree(sd);
        return NULL;
    }
    sd->type = inq[0] & 0x1f;
    sd->removable = inq[1] & 0x80;
    copy_ident(sd->vendor, inq + 8, 8);
    copy_ident(sd->product, inq + 16, 16);
    copy_ident(sd->revision, inq + 32, 4);
    if (sd->type == TYPE_DISK) {
        blockdev_next_name(sd->bdev.name, sizeof sd->bdev.name, "sd", true);
        sd->bdev.sector_size = 512;
    } else if (sd->type == TYPE_CDROM) {
        blockdev_next_name(sd->bdev.name, sizeof sd->bdev.name, "sr", false);
        sd->bdev.sector_size = 2048;
        sd->bdev.flags = BLOCKDEV_CDROM | BLOCKDEV_READONLY;
    } else {
        klog_info("%s device %s %s: type %u not supported", bus, sd->vendor, sd->product, sd->type);
        kfree(sd);
        return NULL;
    }
    sd->bdev.rw = scsi_rw;
    sd->bdev.flush = scsi_flush;
    sd->bdev.priv = sd;
    /* A disk reports its block size with the capacity. The size is taken
     * from the first READ CAPACITY, before the device is registered. */
    mutex_lock(&sd->lock);
    if (sd->type == TYPE_DISK) {
        uint8_t cap[8];
        const uint8_t rc10[10] = { CMD_READ_CAPACITY_10 };
        const uint8_t tur[6] = { CMD_TEST_UNIT_READY };
        for (int i = 0; i < 8 && run(sd, tur, sizeof tur, NULL, 0, false, NULL) != 0; i++)
            sleep_ms(50);
        if (run(sd, rc10, sizeof rc10, cap, sizeof cap, false, NULL) == 0) {
            uint32_t block = (uint32_t)cap[4] << 24 | (uint32_t)cap[5] << 16 | (uint32_t)cap[6] << 8 | cap[7];
            if (block >= 512 && block <= 4096 && !(block & (block - 1)))
                sd->bdev.sector_size = block;
        }
    }
    update_medium(sd);
    mutex_unlock(&sd->lock);
    if (blockdev_register(&sd->bdev) < 0) {
        kfree(sd);
        return NULL;
    }
    klog_info("%s: %s %s %s (%s), %s, %u byte sectors, %lu sectors", sd->bdev.name, sd->vendor, sd->product,
              sd->revision, bus, sd->type == TYPE_CDROM ? "CD drive" : sd->removable ? "removable disk" : "disk",
              sd->bdev.sector_size, sd->bdev.nsectors);
    return sd;
}

void scsi_detach(struct scsi_device *sd)
{
    mutex_lock(&sd->lock);
    sd->gone = true;
    sd->medium = false;
    sd->bdev.nsectors = 0;
    mutex_unlock(&sd->lock);
    bcache_discard(&sd->bdev);
    devfs_set_size(sd->bdev.name, 0);
    klog_info("%s: removed", sd->bdev.name);
}
