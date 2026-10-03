# FAT12, FAT16 and FAT32 (M36)

`kernel/fs/fat/` mounts FAT volumes of all three types, read and write,
with long file names. The on disk format is in
`kernel/include/fs/fat_format.h`, shared with the host tool
`tools/mkfat`.

## Layout

The boot sector's BIOS parameter block gives the sector size (512 is
required), the sectors per cluster, the reserved sectors, the number of
FATs and their size, the root entry count (FAT12/16) and the root
cluster (FAT32). The type follows the cluster count as the specification
defines it: below 4085 clusters FAT12, below 65525 FAT16, above FAT32.
Every access goes through the block cache by byte offset (`fat_rw`), so
clusters of 512 bytes to 32 KiB work regardless of the 4 KiB cache block.

`table.c` reads and writes table entries (12 bit entries straddle bytes,
32 bit entries retain their top nibble) in every FAT copy, allocates
zeroed clusters from a search hint, frees chains and walks a file's
chain from a cached position (`fat_cluster_at`, which also extends the
chain). The free cluster count is computed at mount and retained in
`fat_sb`; on FAT32 `sync` writes it to the FSInfo sector.

## Inodes

The root directory is inode 1. Every other inode number is the byte
offset of the file's short directory entry divided by 32, which is
unique and stable while the entry exists; a rename moves the entry and
therefore changes the number of an open file (the driver updates the
cached inode in place, so the file remains usable). An unlinked entry is
marked in the inode (`unlinked`) so that nothing is written back to a
slot another file may reuse; the clusters are released when the last
reference goes away, as for mfs.

Directories present `.` and `..` from the getdents position, not from
the on disk entries (the root has none). Timestamps written to entries
come from the real time clock of M35. Files are limited to 4 GiB; there
are no holes, so a write beyond the end allocates the clusters in
between; there are no hard links (`link` returns `EROFS`) and no symbolic
links: FAT has no entry type for one, so `symlink` returns `EPERM`. A
link on another filesystem may lead into a FAT volume, and lookups below
it work as for any path.

## Names

`dir.c` iterates entries and assembles long names from the 13 character
pieces preceding a short entry (checked by sequence and checksum). A
short name without long name pieces is presented in lower case. Lookups
compare names case insensitively (ASCII). A created name gets long name
entries unless it already is a valid upper case 8.3 name; its short name
is up to six characters of the base, `~N` with the first N that is unused
in the directory, and up to three of the extension. Names may contain any
UTF-8 character of the basic multilingual plane except the characters
FAT forbids.

## Host tool

`build/host/mkfat`, from `tools/mkfat/mkfat.c`:

- `mkfat [-t 12|16|32] <image> <size_mb> [dir]` writes an image with the
  tree under `dir`; without `-t` the type follows the size (FAT12 below
  4 MiB, FAT16 below 256 MiB). The cluster size is the largest up to
  4 KiB that retains the cluster count inside the type's range.
- `mkfat --dump <image>` prints the type, cluster count and the tree
  with clusters and sizes; `mkfat --cat <image> <path>` prints a file.

Images written by the tool mount on macOS (`hdiutil attach`), which was
used to check the long name entries.

## Tests

- `fat`: three images (case file `fat`, one per line) of 2, 8 and 80 MiB
  formatted as FAT12, FAT16 and FAT32 from `tests/cases/fat/tree`, which
  has upper case, lower case, non ASCII and long names and a 20000 byte
  file. For each type the kernel test reads the tree with folded case,
  lists directories, creates a directory and a 100000 byte file with a
  long name, checks that two long names with the same prefix get distinct
  short names and inodes, renames within and across directories and over
  an existing file, moves a directory, truncates, appends, unlinks an
  open file and reads it until close, and compares the free cluster
  count with the table before and after a remount. The `post` script
  reads the file the kernel left with `mkfat`.
- `fat_user`: `/bin/fattest` mounts a FAT16 image with `mount(2)` and
  uses `opendir`, `stat`, stdio, `rename`, `unlink`, `mkdir`, `rmdir`
  and `umount`.
