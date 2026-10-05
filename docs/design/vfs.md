# Virtual filesystem

The VFS (`kernel/fs/vfs.c`, `kernel/fs/file.c`) gives every filesystem
and device the same interface: paths resolve to inodes, inodes are opened
into files, and files are reached through per process descriptors. The
interface header is `kernel/include/fs/vfs.h`. Structures crossing the
system call boundary (`struct stat`, `struct dirent`, open flags) are defined in
`kernel/include/minios/abi.h`, which libc includes directly.

## Objects

- `struct fs_type` names a filesystem and provides `mount`, which builds a
  superblock from a source string and, since U1 of the multiuser plan
  (`users.md`), a comma separated option string that is empty when none
  was given. mfs, devfs and the initrd accept no options, and FAT takes
  `uid=`, `gid=` and `umask=`. Types register with `vfs_register_fs`.
- `struct superblock` is one mounted instance. It carries the root inode
  number, a `dev` identifier and the cache of inodes currently in use.
  `sb_ops` provides `read_inode`, `put_inode`, `sync` and `unmount`, and
  since M36 the optional `op_begin` and `op_end`, which the VFS calls
  around every modifying operation (create, write, truncate, mkdir,
  unlink, rmdir, link, rename) before it takes any inode or file lock. A
  journaling filesystem groups the changes between them into one
  transaction and may block in `op_begin` for log space; the calling
  thread records the transaction it is inside in `thread.fs_txn`, so
  nested calls (an unlinked inode released inside an operation) do not
  start a second one.
- `struct inode` contains the metadata of one object: mode, link count,
  owner and group (U1), size, modification time (seconds since the epoch, reported as
  `st_mtime`; `vfs_now` reads it from the real time clock), device
  number for device nodes, the operation tables and a filesystem
  private pointer. mfs stores the time in its inode, FAT converts the
  entry's date and time, the initrd takes it from the tar header and
  devfs dates its nodes from the boot. Inodes are reference counted. `inode_get`
  returns the cached inode for a number or reads it through the
  superblock. When the last reference drops the inode is removed from the
  cache and freed, after `put_inode` gave the filesystem a chance to
  release unlinked storage. No inodes are cached beyond their references,
  so a completed test leaves no allocations behind.
- `struct inode_ops` covers directory operations: `lookup`, `create`,
  `mkdir`, `unlink`, `rmdir`, `link`, `symlink`, `rename` and `truncate`,
  `readlink` on a symbolic link, and `setmtime` and `setattr` on any
  inode. `create` and `mkdir` receive the permission bits after the
  umask, and the new inode takes the owner `vfs_new_owner` chooses.
  `setattr` (U1) stores new permission bits and a new owner, which
  `vfs_chmod_inode` and `vfs_chown_inode` compute after checking that the
  caller may make the change. Names are passed as pointer and
  length. The caller has acquired the directory inode mutex, or the link's own
  mutex for `readlink`. A filesystem without a given operation leaves the
  pointer NULL and the VFS reports `EROFS`.
- `struct file_ops` covers open files: `open`, `release`, `read`, `write`,
  `getdents` and `lseek`. `read` and `write` receive the position by
  pointer and advance it. `getdents` fills whole `struct dirent` records
  and uses `file->pos` as the entry index.
- `struct file` is an open file description: inode, operations, position,
  flags and a reference count. Descriptors in different processes may share
  one description after `fork` or `dup`, so the position is shared as well.
- `struct fdtable` is the per process descriptor table of `OPEN_MAX`
  slots, embedded in `struct proc`. `fork` copies it with an extra
  reference per file, `exec` retains it and process exit closes everything
  before the parent is notified so pipe peers see the end promptly.
- `struct mount` records a mounted superblock and the `(superblock, inode
  number)` pair of the directory it covers. Identifying the mount point by
  number rather than pointer means the covered inode does not need to remain
  in memory.

## Path resolution

Every lookup walks from the root mount. A relative path is first joined
to the working directory, which `proc->cwd` contains as the canonical path
of the directory without symbolic links (the string `getcwd` returns).
The walk (`walk` in `vfs.c`) retains the remaining components in a buffer
and the canonical path of the directory reached so far beside it, and
takes one component at a time, calling `lookup` under the directory
mutex and dropping the reference to the previous directory. After each
step the result is checked against the mount table and replaced by the
root of a mount that covers it.

A component that names a symbolic link (mode `S_IFLNK`) is replaced in
the buffer by the link's target, read with the `readlink` operation
under the link's mutex alone. A relative target continues from the
directory that contains the link, an absolute one from the root. Links are
followed in every intermediate component and in the last one, unless
the caller passes `VFS_NOFOLLOW` and the last component has no trailing
slash. A lookup follows at most `SYMLOOP_MAX` (40) links in total and
fails with `ELOOP` beyond that, which also ends every loop. A target
contains 1 to `VFS_SYMLINK_MAX` (255) bytes: an empty target resolves to
`ENOENT`, and the remainder buffer of four paths bounds the growth by
nested targets (`ENAMETOOLONG`).

Since the canonical path of the current directory contains no link,
`..` removes its last component and the walk finds that directory again
from the root. `..` after a link therefore leaves the directory the link
led to, not the one containing the link, and `..` at the root of a mounted
filesystem reaches the parent of the directory it covers. A component
after a regular file fails with `ENOTDIR`, also for `.` and `..`, and
an empty path with `ENOENT`.

`vfs_lookup` follows every link; `vfs_lookup_path` takes the flags and
also returns the canonical path of the result, which `chdir` stores,
`open` retains in `file.path` for the `*at` system calls and `mount`
records in the mount table (so `/dev/mounts` and `umount` name mounts by
that path, and `umount` accepts a path through links).
`vfs_lookup_parent` stops before the last component and returns its
name, never following it, which the create, unlink, mkdir, rmdir, link,
symlink and rename paths use; `.` and `..` are refused there with
`EINVAL`. Unlink, rename and rmdir therefore act on a link itself: rmdir
of a link to a directory fails with `ENOTDIR`, rename moves the link,
and `link` makes a hard link to a link rather than to its target (POSIX
leaves this choice to the implementation). `rename` locks two
directories in inode number order so concurrent renames between the same
pair cannot deadlock.

`open` resolves the last component like any lookup; `O_NOFOLLOW` makes a
link there fail with `ELOOP`. With `O_CREAT` the parent is resolved and
the name looked up under the directory mutex: a missing name is created,
and a link is followed by releasing the directory and repeating the
parent lookup for its target, with the same count of followed links, so
a dangling link creates the file it names, as POSIX specifies. `O_EXCL`
fails with `EEXIST` on a link whether it dangles or not, and `O_NOFOLLOW`
with `ELOOP`.

`lstat`, `fstatat` with `AT_SYMLINK_NOFOLLOW`, `readlink` and `utimensat`
with `AT_SYMLINK_NOFOLLOW` look the last component up with
`VFS_NOFOLLOW`. `inode_stat` reports a link with its own inode, mode
`S_IFLNK | 0777` and the target's length as `st_size`; `getdents` gives
it `DT_LNK`.

`vfs_symlink` checks the target length and calls the directory's
`symlink` operation. mfs stores links (`mfs.md`), the initrd reads them
from the archive, FAT and devfs return `EPERM`, and a filesystem without
the operation reports `EROFS`.

## Filesystems in M11

- `initrd` (`fs/initrdfs.c`) exposes the ustar archive parsed by
  `fs/initrd.c` read only. Inode 1 is the root, entry i is inode i + 2. A
  directory lists entries whose name has the directory as prefix and no
  further slash. A member of typeflag `2` is a symbolic link whose target
  is the header's link name field (at most 100 bytes). `initrd_init`
  counts the members first and allocates a table with one entry for each,
  which removes the earlier limit of 1024 members.
- `devfs` (`fs/devfs.c`) is a flat in memory directory of nodes registered
  by drivers with `devfs_register(name, mode, fops, priv)`. It provides
  `/dev/console` (keyboard line discipline for reading, console for
  writing), `/dev/input/eventN` (M47), `/dev/null` and `/dev/zero`. Block devices add
  `/dev/vda` in M12.

`kinit` mounts `initrd` on `/` and `devfs` on `/dev` (an empty directory
on the initrd) before any program runs. New user processes created from
the kernel get `/dev/console` on descriptors 0, 1 and 2, each opened
read/write as a terminal is on Unix: a program whose input is a pipe can
read keys from the terminal behind its output (`less` does this with
`dup(1)`) without opening `/dev/console`, which inside a terminal window
would be another terminal.

## tmpfs (P4)

`fs/tmpfs.c` stores files, directories and symbolic links in memory. Each
is a node that the filesystem retains as long as it has a name, found by
its inode number in a hash table. The VFS caches inodes only while they
are referenced, and an inode is therefore a view of its node:
`read_inode` fills it from the node, and every operation changes both.
The data of a file are whole pages of the page allocator, a hole reads as
zeros, a directory lists `.`, `..` and its entries in the order of their
creation, and the last reference to a node without links frees it. The
options are `size=MIB`, a quarter of the memory by default, `mode`, 1777
by default, `uid` and `gid`. A write beyond the size fails with `ENOSPC`.
Unmounting frees every node and the superblock. `/etc/fstab` mounts a
tmpfs on `/tmp` and on `/run`, which `base-files` creates, at every boot
through `fsinit`. For a boot test that starts its program without init,
`/tmp` remains on the root filesystem.

## 9p (V5 of the 0.6.0 release)

`fs/9p/` mounts a folder that the host shares through virtio-9p, with the
protocol 9P2000.L (`9p.md`). The source of the mount is the mount tag of
the device, as in `mount -t 9p host /mnt/host`. The inode number is the
path of the qid of the server. The client stores no data. Reads and writes
go to the host at once, and every lookup reads the attributes again.
`read_inode` receives the walked fid of a lookup through a list, because
the VFS passes only the inode number. `fsinit`
mounts every share at `/mnt/TAG` after `/etc/fstab`.

## Mount capacity snapshots

The optional `sb_ops.statfs` fills `struct fs_space` with total blocks,
free blocks and block size. mfs reads its superblock counters under the
filesystem mutex; FAT reports data-cluster and free-cluster counts using
the cluster size (the in-memory count also maintains FSInfo). Virtual
filesystems without the operation report zero capacity.

Opening read-only `/dev/mounts` creates a text snapshot, one record per
mount: `path type total-blocks free-blocks block-size`. `vfs_format_mounts`
pins mounts with a reader count while collecting statistics outside the
mount spinlock, since filesystem mutexes may sleep. Unmount reports busy
while a snapshot is being constructed. The open file owns only the
finished text, not mount references, and successive reads use its offset.
`df` skips zero-block-size records and converts capacity to 1 KiB units.
The `mfs` and `fat` tests check the statistics against allocation counters;
`utils` exercises the devfs-to-df path.

## Pipes

`ipc/pipe.c` implements anonymous pipes as two files without an inode
sharing a `struct pipe`: a 4 KiB ring buffer, reader and writer counts and
two wait queues, all under `pipe.lock`. A read on an empty pipe blocks
until data arrives or the last writer closes, in which case it returns 0.
A write blocks while the buffer is full and fails with `EPIPE` once no
reader remains. Blocked readers and writers return `EINTR` when their
process is being terminated. Each end contains a reference (`pipe.refs`);
`pipe_release` drops it as its last step, after waking the peers and the
pollers, so the pipe outlives both releases even when the two ends are
closed at the same moment on different CPUs, as happens when the
processes of a pipeline exit together.

## System calls

`open`, `close`, `read`, `write`, `lseek`, `dup`, `dup2`, `stat`,
`fstat`, `getdents`, `mkdir`, `unlink`, `rmdir`, `rename`, `link`,
`pipe`, `mount`, `umount`, `sync`, `chdir`, `getcwd`, `utimensat` (since
the make port, `make.md`), `openat` and `fstatat` (since the tar port,
`artar.md`; a directory opened by name retains its canonical path in
`file.path` for them), and `symlink`, `symlinkat`, `readlink`,
`readlinkat` and `lstat` (numbers 91 to 95, 2026-09-30), and `fchmodat`,
`fchmod`, `fchownat` and `fchown` (numbers 103 to 106, U1, `users.md`) are
implemented in `syscall/sys_fs.c`. `mount` takes a fourth argument, an
option string or NULL, and `mkdir` passes its mode, which the kernel
ignored before U1. `readlink` copies at most the given size of the
target without a NUL and returns the byte count; a size of zero is
`EINVAL`. User buffers are checked with `user_range_ok` and
then accessed directly. `read` and `write` on descriptors 0 to 2 go
through the console device like any other file.

## libc and programs

libc implements the calls above plus `opendir`, `readdir`, `closedir`
over `getdents`, `isatty` over `fstat`, and `fopen`, `fdopen`, `fclose`,
`fseek`, `ftell` and `rewind` over descriptors. Streams opened with
`fopen` are flushed by `exit`. `realpath` resolves one component at a
time with `lstat` and `readlink`, with the same rules and limit as the
kernel, and `remove` uses `lstat`, so it removes a link and not the
directory it may lead to.

The shell parses pipelines with `|` and the redirections `<`, `>` and
`>>`. Each stage is a forked child with its ends wired through `dup2`.
Coreutils added: `cat`, `ls` (`-l`), `wc` (`-l`, `-w`, `-c`), `mkdir`,
`rm` (`-d`), `mv` and `cp`. The modifying utilities return `EROFS` on the
initrd until the disk filesystem of M13 is mounted.

With symbolic links `ln -s` creates links and `readlink` prints them.
`ls` lists the entries of a directory with `lstat` (`l` and
`name -> target` in the long format, `@` with `-F`) and follows a link
named as an operand unless `-l`, `-d` or `-F` is given. `stat` reports a
link itself unless `-L` is given, `find` does not follow links and
selects them with `-type l`, `du` and `tree` do not enter them, `rm`
removes the link, and `cp` copies a link as a link with `-P` and by
default with `-R`, and follows it otherwise. The Files program copies
links as links and deletes the link, not the tree behind it. The sbase
`tar` archives links with typeflag `2` and extracts them with `symlink`
and `utimensat(AT_SYMLINK_NOFOLLOW)`.

## Tests

- `fs` runs `/bin/fstest`: file reads and seeks, shared offsets through
  `dup` and across `fork`, `stat` on files, directories and devices,
  directory listing of `/`, `/bin` and `/dev`, the read only errors,
  relative paths, `/dev/zero` and `/dev/null`, pipes with a 20 KB
  transfer, `EPIPE`, `exec` into a pipe, stdio streams and descriptor
  exhaustion.
- `pipes` types `ls /dev`, `cat < /etc/motd`, `cat /etc/motd | wc`, output
  redirection and a four stage pipeline into the keyboard buffer and runs
  the shell on it, checking the output and that no physical page leaks.
- `symlink` runs `/etc/tests/symlink.sh`, which starts `/bin/symlinktest`
  for the system calls: readlink without a NUL and truncated, relative
  and absolute targets, the `*at` forms, a 255 byte target and the
  refusal of a 256 byte one, chains, 40 links resolving and 41 failing
  with `ELOOP`, loops, `O_NOFOLLOW`, `lstat` against `stat`,
  `utimensat` on a link, dangling links with `O_CREAT` and `O_EXCL`,
  unlink and rename of links, a hard link to a link, `..` after a link,
  `getcwd`, `realpath`, links into, out of and onto a mounted mfs volume
  (`vdb`), `umount` through a link, links that survive the unmount, the
  links of the initrd and of the root image, and `EPERM` on devfs and on
  a FAT volume (`vdc`) whose files remain reachable through a link. The
  script then checks `ln`, `readlink`, `ls`, `stat`, `find`, `tree`,
  `cp`, `du` and `rm` and a `tar` round trip, unmounts the volume and
  syncs. The `post` script runs `fsck` on both images and finds the
  links in `mkfs --dump`.
- `pipe_close` (`kernel/tests/test_pipe.c`) has two kernel threads release
  the read ends and the write ends of 256 pipes in step, forty rounds, so
  both ends of a pipe are closed at the same moment on different CPUs; it
  hung before `pipe_release` contained its own reference across the wakeups.

## Read only files (R5)

`open` with write access to a regular file whose file operations have no
`write` fails with `EROFS`. The files of the initrd and of an ISO 9660
volume are such files (`iso9660.md`). Creating a file in a directory
without `create` fails with `EROFS` as before.

