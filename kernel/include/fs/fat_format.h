#pragma once
/* On disk format of FAT12, FAT16 and FAT32, shared with tools/mkfat. All
 * fields are little endian and unaligned, so the structures are packed. */
#include <stdint.h>

#ifndef __packed
#define __packed __attribute__((packed))
#endif

#define FAT_SECTOR_SIZE     512

/* BIOS parameter block at the start of the boot sector. */
struct fat_bpb {
    uint8_t  jump[3];
    char     oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  nfats;
    uint16_t root_entries;          /* FAT12/16 root directory entries, 0 on FAT32 */
    uint16_t total_sectors16;
    uint8_t  media;
    uint16_t fat_size16;            /* sectors per FAT, 0 on FAT32 */
    uint16_t sectors_per_track;
    uint16_t heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors32;
    union {
        struct {
            uint8_t  drive;
            uint8_t  reserved;
            uint8_t  boot_signature;    /* 0x29 when the next three fields are valid */
            uint32_t volume_id;
            char     label[11];
            char     fs_type[8];
        } __packed f16;
        struct {
            uint32_t fat_size32;
            uint16_t ext_flags;
            uint16_t version;
            uint32_t root_cluster;
            uint16_t fsinfo_sector;
            uint16_t backup_boot_sector;
            uint8_t  reserved[12];
            uint8_t  drive;
            uint8_t  reserved1;
            uint8_t  boot_signature;
            uint32_t volume_id;
            char     label[11];
            char     fs_type[8];
        } __packed f32;
    };
} __packed;

#define FAT_BOOT_SIGNATURE  0xaa55      /* at byte 510 of the boot sector */

/* FAT32 FSInfo sector. */
#define FAT_FSINFO_LEAD     0x41615252u
#define FAT_FSINFO_STRUCT   0x61417272u
#define FAT_FSINFO_TRAIL    0xaa550000u
struct fat_fsinfo {
    uint32_t lead_signature;
    uint8_t  reserved[480];
    uint32_t struct_signature;
    uint32_t free_clusters;         /* 0xffffffff when unknown */
    uint32_t next_free;             /* hint, 0xffffffff when unknown */
    uint8_t  reserved2[12];
    uint32_t trail_signature;
} __packed;

/* Directory entry. */
struct fat_dirent {
    uint8_t  name[11];              /* 8 + 3, space padded, upper case */
    uint8_t  attr;
    uint8_t  nt_reserved;           /* 0x08: base lower case, 0x10: extension lower case */
    uint8_t  ctime_tenths;
    uint16_t ctime;
    uint16_t cdate;
    uint16_t adate;
    uint16_t cluster_hi;            /* FAT32 only */
    uint16_t mtime;
    uint16_t mdate;
    uint16_t cluster_lo;
    uint32_t size;
} __packed;

#define FAT_ATTR_READ_ONLY  0x01
#define FAT_ATTR_HIDDEN     0x02
#define FAT_ATTR_SYSTEM     0x04
#define FAT_ATTR_VOLUME     0x08
#define FAT_ATTR_DIRECTORY  0x10
#define FAT_ATTR_ARCHIVE    0x20
#define FAT_ATTR_LFN        0x0f    /* long file name piece */

#define FAT_NAME_FREE       0x00    /* first byte: this and every later entry is free */
#define FAT_NAME_DELETED    0xe5
#define FAT_NAME_E5         0x05    /* first byte stored for a name starting with 0xe5 */

/* Long file name entry: 13 UTF-16 characters, sequence numbers from 1
 * upwards with FAT_LFN_LAST set on the entry stored first. */
struct fat_lfn {
    uint8_t  sequence;
    uint16_t name1[5];
    uint8_t  attr;                  /* FAT_ATTR_LFN */
    uint8_t  type;                  /* 0 */
    uint8_t  checksum;              /* of the short name that follows */
    uint16_t name2[6];
    uint16_t cluster;               /* 0 */
    uint16_t name3[2];
} __packed;

#define FAT_LFN_LAST        0x40
#define FAT_LFN_CHARS       13
#define FAT_LFN_MAX         255
#define FAT_LFN_ENTRIES_MAX 20

/* Cluster values. */
#define FAT12_EOC           0xff8u
#define FAT12_BAD           0xff7u
#define FAT16_EOC           0xfff8u
#define FAT16_BAD           0xfff7u
#define FAT32_EOC           0x0ffffff8u
#define FAT32_BAD           0x0ffffff7u
#define FAT32_MASK          0x0fffffffu

/* Type by cluster count, as the specification defines it. */
#define FAT12_MAX_CLUSTERS  4084u
#define FAT16_MAX_CLUSTERS  65524u

static inline uint8_t fat_short_checksum(const uint8_t name[11])
{
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + name[i]);
    return sum;
}
