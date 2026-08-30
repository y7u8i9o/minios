# mfs: the disk filesystem

mfs is a small inode based filesystem with 4 KiB blocks. The on disk
format is defined once in `kernel/include/fs/mfs_format.h`, shared by the
kernel (`kernel/fs/mfs/`) and the host tool (`tools/mkfs/`).

## On disk layout

| Region | Blocks | Contents |
|---|---|---|
| superblock | 0 | `struct mfs_superblock` |
| inode bitmap | `inode_bitmap_start` | one bit per inode number, bit 0 always set |
| block bitmap | `block_bitmap_start` | one bit per block, metadata blocks set |
| inode table | `inode_table_start` | 32 `struct mfs_dinode` of 128 bytes per block |
| data | `data_start` .. `nblocks` | file, directory and indirect blocks |

The superblock records the region boundaries, the free block and inode
counts, a mount counter and the flags word. `MFS_FLAG_CLEAN` is cleared
when the filesystem is mounted and set again by unmount; a mount that finds
the flag clear logs `previous shutdown was unclean`.

An inode holds the mode, link count, size, twelve direct block pointers,
one indirect pointer (1024 blocks) and one double indirect pointer
(1024 x 1024 blocks). Unused pointers are zero; reading such a block yields
zeros, which gives sparse files. Inode 1 is the root directory.

Directories are arrays of 64 byte entries: a 32 bit inode number followed
by a NUL terminated name of up to 59 characters. An entry with inode 0 is
free and is reused by the next create. Every directory starts with `.` and
`..`. A directory's link count is 2 plus the number of subdirectories.

## Host tool

`build/host/mkfs`, built from `tools/mkfs/mkfs.c`:

- `mkfs <image> <size_mb> <dir>` writes a fresh image whose root holds the
  tree under `dir`. Hidden files are skipped. The inode count is one per
  four blocks with a minimum of 64.
- `mkfs --dump <image>` prints the superblock summary, including `clean`
  or `unclean`, and the tree with inode numbers, sizes and link counts.
- `mkfs --cat <image> <path>` writes a file's contents to standard output.

`make disk` (a dependency of `make image` and `make test`) builds
`build/disk.img` from `build/initrd_root`, so the disk carries the same
programs as the initrd plus `/etc/motd` and the empty `/dev` and
`/initrd` mount points. The image is regenerated whenever a user program
changes, which discards files written during earlier runs.

## Kernel implementation

- `super.c`: mount reads and validates the superblock, clears the clean
  flag, increments the mount counter and writes block 0 back before
  returning. `read_inode` and `mfs_inode_flush` copy between
  `struct inode` plus `struct mfs_inode_info` (the block pointers) and the
  inode table through the block cache. `put_inode` runs when the last
  reference to an inode goes away and frees its blocks and inode number if
  the link count is zero, so an unlinked open file keeps its data until it
  is closed. `sync` writes the superblock and flushes the cache; `unmount`
  additionally sets the clean flag.
- `bitmap.c`: first fit allocation in the inode and block bitmaps under
  `mfs_sb.lock`. New blocks are zeroed through the cache.
- `inode.c`: `bmap` translates a file block index to a disk block, creating
  indirect tables on demand. `mfs_read_locked` and `mfs_write_locked`
  transfer byte ranges block by block; writes extend the size and flush
  the inode. `mfs_truncate_locked` frees blocks past the new size, zeroes
  the tail of the last block and releases indirect tables that became
  empty. The file operations take the inode mutex around these helpers.
- `dir.c`: directory operations run with the directory mutex held by the
  VFS. `lookup`, `create`, `mkdir`, `unlink`, `rmdir`, `link` and
  `rename` scan and rewrite entries through the data helpers. `rename`
  replaces an existing target of the same kind (a directory only when
  empty), and moving a directory between parents rewrites its `..` entry
  and adjusts both parents' link counts. `getdents` walks the entry slots
  using `file->pos` as the slot index.

Every metadata change goes through `bwrite`, so it becomes durable at the
next `sync`, unmount, or cache eviction.

## Root filesystem and shutdown

`kinit` mounts `mfs` from `vda` on `/` when the device carries a valid
superblock, then mounts the initrd on `/initrd` and devfs on `/dev`. With
no usable disk, or with `root=initrd` on the command line, the initrd
remains the root. `init=<path>` selects the first program.

The `shutdown` system call (`shutdown_system` in libc, the `shutdown`
utility, `-r` to reboot) syncs and unmounts every filesystem that has no
inode in use, most recent mount first, then powers off through ACPI.
Filesystems without on disk state (devfs, initrd) may stay mounted; a
persistent filesystem that is still busy is reported and left unclean.

## Tests

- `mfs`: kernel test through the VFS on the disk root. A 4.5 MiB file
  exercises direct, indirect and double indirect blocks and the block
  accounting is checked exactly (1152 data blocks plus three index
  blocks), followed by truncation, sparse writes, unlink, nested
  directories, `ENOTEMPTY`, hard links, renames within and across
  directories, replacement of an existing file, moving a directory (with
  `..` and link counts), `getdents`, and a final check that every block
  was returned and the cache is clean.
- `mfs_user`: `/bin/mfstest` performs the same operations through system
  calls and libc streams, including `cp` and a pipeline redirected into a
  file through the shell.
- `shutdown`: boots with `init=/bin/shutdowntest`, which writes
  `/persist.txt`, creates `/persist.d` and calls `shutdown_system`. The
  case's `post` script checks that QEMU exited through ACPI power off (exit
  code 0) and uses `mkfs --dump` and `mkfs --cat` to verify the clean flag
  and the persisted contents in the resulting image.
- `blk` boots with `root=initrd` so its raw sector writes do not touch a
  mounted filesystem.

Booting still uses the Limine ISO; the disk image is the primary root
filesystem, not the boot medium. Installing Limine on the disk image is a
possible later change.
