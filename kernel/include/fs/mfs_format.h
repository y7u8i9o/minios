#pragma once
/* On disk format of mfs, shared with tools/mkfs and tools/fsck. All fields
 * are little endian. Block 0 contains the superblock, followed by the inode
 * bitmap, the block bitmap, the inode table, the journal and the data
 * blocks. */
#include <stdint.h>

#define MFS_MAGIC          0x3153464du   /* "MFS1" */
#define MFS_VERSION        5             /* 2: journal region (M36), 3: 256 byte directory entries, 4: mtime in nanoseconds, 5: owners (U1) */
/* The oldest version still mounted. Version 4 differs only in leaving the
 * owner words zero, which reads as root. */
#define MFS_VERSION_MIN    4
#define MFS_BLOCK_SIZE     4096
#define MFS_NDIRECT        12
#define MFS_INODE_SIZE     128
#define MFS_INODES_PER_BLOCK (MFS_BLOCK_SIZE / MFS_INODE_SIZE)
#define MFS_PTRS_PER_BLOCK (MFS_BLOCK_SIZE / 4)
#define MFS_NAME_MAX       251
#define MFS_DIRENT_SIZE    256
#define MFS_ROOT_INO       1
/* A symbolic link (mode S_IFLNK) retains its target, without a NUL, in the
 * first data block; its size is the target's length. */
#define MFS_SYMLINK_MAX    (MFS_BLOCK_SIZE - 1)

/* flags */
#define MFS_FLAG_CLEAN     1u   /* set by unmount, cleared by mount */

struct mfs_superblock {
    uint32_t magic;
    uint32_t version;
    uint32_t block_size;
    uint32_t flags;
    uint64_t nblocks;               /* total blocks in the filesystem */
    uint32_t ninodes;               /* inode numbers 1 .. ninodes - 1 are usable */
    uint32_t inode_bitmap_start;
    uint32_t inode_bitmap_blocks;
    uint32_t block_bitmap_start;
    uint32_t block_bitmap_blocks;
    uint32_t inode_table_start;
    uint32_t inode_table_blocks;
    uint32_t data_start;
    uint64_t free_blocks;
    uint32_t free_inodes;
    uint32_t mount_count;
    uint32_t journal_start;         /* version 2: header block of the journal */
    uint32_t journal_blocks;        /* header plus MFS_JOURNAL_SLOTS data slots */
};

struct mfs_dinode {
    uint32_t mode;                  /* S_IF* and permission bits */
    uint32_t nlink;
    uint64_t size;
    uint64_t mtime;                 /* nanoseconds since the epoch (version 4) */
    uint32_t direct[MFS_NDIRECT];
    uint32_t indirect;
    uint32_t dindirect;
    uint32_t uid;                   /* owner (version 5, zero before) */
    uint32_t gid;
    uint32_t pad[10];
};

struct mfs_dirent {
    uint32_t ino;                   /* 0 marks a free slot */
    char name[MFS_NAME_MAX + 1];    /* NUL terminated */
};

/* Byte capacity limits. */
#define MFS_MAX_FILE_BLOCKS \
    (MFS_NDIRECT + MFS_PTRS_PER_BLOCK + (uint64_t)MFS_PTRS_PER_BLOCK * MFS_PTRS_PER_BLOCK)

/* The journal (M36): a write ahead log of metadata blocks. One transaction
 * is in the journal at a time. Its blocks are written to the slots after
 * the header, then the header is written with the count and a CRC-32 over
 * the header fields and the slot contents. Only a header whose checksum
 * matches describes a committed transaction; once the blocks have reached
 * their home locations the header is written again with count 0. */
#define MFS_JOURNAL_MAGIC  0x4c4e524au   /* "JRNL" */
#define MFS_JOURNAL_SLOTS  127
#define MFS_JOURNAL_BLOCKS (1 + MFS_JOURNAL_SLOTS)

struct mfs_journal_header {
    uint32_t magic;
    uint32_t count;                 /* blocks in the transaction, 0 when empty */
    uint64_t sequence;              /* incremented per commit */
    uint32_t checksum;              /* crc32 of this header (field zero) and the slots */
    uint32_t pad;
    uint32_t block[MFS_JOURNAL_SLOTS];  /* home block of each slot */
};
