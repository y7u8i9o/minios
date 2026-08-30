#pragma once
/* On disk format of mfs, shared with tools/mkfs. All fields are little
 * endian. Block 0 holds the superblock, followed by the inode bitmap,
 * the block bitmap, the inode table and the data blocks. */
#include <stdint.h>

#define MFS_MAGIC          0x3153464du   /* "MFS1" */
#define MFS_VERSION        1
#define MFS_BLOCK_SIZE     4096
#define MFS_NDIRECT        12
#define MFS_INODE_SIZE     128
#define MFS_INODES_PER_BLOCK (MFS_BLOCK_SIZE / MFS_INODE_SIZE)
#define MFS_PTRS_PER_BLOCK (MFS_BLOCK_SIZE / 4)
#define MFS_NAME_MAX       59
#define MFS_DIRENT_SIZE    64
#define MFS_ROOT_INO       1

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
};

struct mfs_dinode {
    uint32_t mode;                  /* S_IF* and permission bits */
    uint32_t nlink;
    uint64_t size;
    uint64_t mtime;
    uint32_t direct[MFS_NDIRECT];
    uint32_t indirect;
    uint32_t dindirect;
    uint32_t pad[12];
};

struct mfs_dirent {
    uint32_t ino;                   /* 0 marks a free slot */
    char name[MFS_NAME_MAX + 1];    /* NUL terminated */
};

/* Byte capacity limits. */
#define MFS_MAX_FILE_BLOCKS \
    (MFS_NDIRECT + MFS_PTRS_PER_BLOCK + (uint64_t)MFS_PTRS_PER_BLOCK * MFS_PTRS_PER_BLOCK)
