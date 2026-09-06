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
| journal | `journal_start`, 128 blocks | header block and 127 slots (format version 2, M36) |
| data | `data_start` .. `nblocks` | file, directory and indirect blocks |

The superblock records the region boundaries, the free block and inode
counts, a mount counter and the flags word. `MFS_FLAG_CLEAN` is cleared
when the filesystem is mounted and set again by unmount; a mount that finds
the flag clear logs `previous shutdown was unclean`. Version 1 images
(without a journal) are refused by the kernel and the tools; `make disk`
rebuilds the image in version 2.

An inode holds the mode, link count, size, twelve direct block pointers,
one indirect pointer (1024 blocks) and one double indirect pointer
(1024 x 1024 blocks). Unused pointers are zero; reading such a block yields
zeros, which gives sparse files. Inode 1 is the root directory.

Directories are arrays of 256 byte entries (format version 3; version 2
used 64 byte entries and 59 character names): a 32 bit inode number
followed by a NUL terminated name of up to 251 characters, any bytes but
`/` and NUL. An entry with inode 0 is
free and is reused by the next create. Every directory starts with `.` and
`..`. A directory's link count is 2 plus the number of subdirectories.

## Host tool

`build/host/mkfs`, built from `tools/mkfs/mkfs.c`:

- `mkfs <image> <size_mb> <dir>` writes a fresh image whose root holds the
  tree under `dir`. Hidden files are skipped. The inode count is one per
  four blocks with a minimum of 64.
- `mkfs --dump <image>` prints the superblock summary, including `clean`
  or `unclean`, the journal state (sequence number and pending blocks),
  and the tree with inode numbers, sizes and link counts.
- `mkfs --cat <image> <path>` writes a file's contents to standard output.

`build/host/fsck`, built from `tools/fsck/fsck.c` (M36), checks and
repairs an image: `fsck [-n | -y] [-v] <image>`. It replays the journal
first, then runs five passes: inodes (mode, link count, block pointers
in range and used once, size against the block count), the directory
tree from the root (`.` and `..`, entries pointing at usable inodes, a
directory linked once), connectivity (unreferenced inodes are moved to
`/lost+found`, created when missing, as `#<inode>`; link counts are
recomputed), the bitmaps against the blocks in use, and the free counts.
Without `-y` problems are only reported; `-n` never writes the image, so
the replay happens in memory. An image with no problems left is marked
clean. The exit status is 0 (no problems), 1 (repaired), 4 (problems
left) or 8 (usage or I/O error).

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
  inode table through the block cache; the `mtime` field of the disk
  inode holds the modification time in nanoseconds since the epoch
  (format version 4; versions 2 and 3 stored seconds), set by
  `mkfs` from the host file, by `mfs_inode_new`, and by every write and
  truncation (a write that does not grow the file flushes the inode once
  at its end). `put_inode` runs when the last
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

## Journal

Since M36 metadata changes are journaled (`journal.c`): the inode table,
both bitmaps, the superblock counters, indirect blocks and directory
contents. File data is not journaled.

Every modifying VFS operation runs between the superblock hooks
`op_begin` and `op_end` (see `vfs.md`); `mfs_journal_begin` waits while a
commit is in progress or fewer than 16 slots are free, then counts the
operation in. Metadata buffers changed by the operation go through
`mfs_journal_write`, which pins them in the block cache (`bpin`): they
stay dirty in memory and are neither evicted nor written by
`bcache_sync`. Nested operations by the same thread (an unlinked inode
released while its directory entry is removed) are counted in
`thread.fs_txn_depth` and join the outer transaction; the release of an
unlinked file at its last close, outside any operation, opens a
transaction of its own.

When the last operation of a group ends, that thread commits:

1. `bcache_sync` writes the dirty data blocks of the device, so no
   committed metadata points at data that never reached the disk
   (ordered mode); the pinned blocks are skipped.
2. Each pinned block is written to its journal slot and folded into a
   CRC-32, then the device cache is flushed.
3. The header block is written with the sequence number, the count, the
   home block numbers and the checksum over header and slots, and the
   device is flushed again. From this point the transaction is durable.
4. The blocks are written to their home locations with `bwrite_now`,
   unpinned, and the device is flushed.
5. The header is written with count zero.

`sync` and unmount call `mfs_journal_flush`, which waits for running
operations and commits what is pending. A mount reads the header raw: a
non zero count whose checksum matches is replayed by copying the slots
home through the cache (the superblock is re-read afterwards) and logged
as `journal replayed, N blocks`; a mismatch means the crash happened
before step 3 and the transaction is discarded. Journal blocks are never
accessed through the block cache, so the raw writes of the commit cannot
be shadowed by stale cached copies.

The journal holds one transaction of up to 127 blocks; a single operation
touches around ten metadata blocks at most (`free_from` no longer clears
the entries of an indirect table that is released as a whole), so with
the 16 block reservation up to seven operations run concurrently and
further ones wait in `op_begin`, where no lock is held.

`mfs_journal_set_crash` is a test hook: mode 1 makes the next commit do
nothing, mode 2 stops it after step 3; while a crash mode is set, `sync`
writes nothing and unmount discards every pinned and cached buffer of the
device (`mfs_journal_discard`), which models a power loss.

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
- `mfs_journal` (`kernel/tests/test_journal.c`): a second, empty mfs
  image (case file `mfs2`) is mounted on `/mnt`. With crash mode 2 a
  20000 byte file is written, a directory created and a file removed; the
  device is unmounted as after a power loss and mounted again, and the
  changes must be there. With crash mode 1 a file write and a rename must
  leave no trace. Eight kernel threads then create, write and unlink files
  concurrently (more than seven, so some wait for journal space); the
  superblock counters must agree with the bitmaps after every step. The
  root image is finally left with a committed, not checkpointed
  transaction; the `post` script runs `fsck` on both images: the second
  reports no problems, the first prints `journal: replaying transaction`
  and afterwards holds `/journaled.txt`, and a second `fsck` run finds it
  clean. Repair paths of `fsck` were checked by hand on images corrupted
  with `dd` (bitmap zeroed, link count wrong, entry removed).
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
