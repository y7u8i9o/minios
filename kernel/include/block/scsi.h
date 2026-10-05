#pragma once
/* SCSI disks and CD drives behind a transport (block/scsi.c,
 * docs/design/block.md). The ATAPI drives of the AHCI driver and the
 * logical units of USB mass storage carry SCSI commands. This module
 * builds the commands, reads the capacity and registers the block device:
 * a disk as sdX, a CD drive as srN. */
#include <kernel.h>
#include <block/blockdev.h>
#include <sync/mutex.h>

/* The transport returns 0 when the command completed with GOOD status,
 * SCSI_CHECK_CONDITION when the device reports CHECK CONDITION, and a
 * negative errno when the command did not reach the device or the device
 * is gone. buf has len bytes, which the command reads from the device or,
 * with write, writes to it. */
#define SCSI_CHECK_CONDITION 2
typedef int (*scsi_command_fn)(void *ctx, const uint8_t *cdb, unsigned cdb_len, void *buf, uint32_t len,
                               bool write);

/* A disk or CD drive. lock serializes the commands and protects
 * bdev.nsectors, medium and gone. The other fields are set by scsi_attach
 * and read without a lock. */
struct scsi_device {
    struct blockdev bdev;
    scsi_command_fn command;
    void *ctx;
    uint32_t max_transfer;          /* bytes of one command, from the transport */
    uint8_t type;                   /* peripheral device type: 0 disk, 5 CD drive */
    bool removable;
    char vendor[9], product[17], revision[5];
    const char *bus;                /* "atapi" or "usb", for the log and /dev/devices */
    struct mutex lock;
    bool medium;                    /* a medium is present and its capacity is known */
    bool gone;                      /* the device was removed */
};

/* Identify the device behind a transport with INQUIRY, read its capacity
 * and register it. Returns NULL for a device type other than a disk or a
 * CD drive and on failure. */
struct scsi_device *scsi_attach(scsi_command_fn command, void *ctx, uint32_t max_transfer, const char *bus);
/* Mark a device as removed. Its block device remains registered, with no
 * sectors, and every later request fails with ENODEV. */
void scsi_detach(struct scsi_device *sd);
