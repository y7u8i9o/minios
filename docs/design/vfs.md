# Virtual filesystem

The VFS (`kernel/fs/vfs.c`, `kernel/fs/file.c`) gives every filesystem
and device the same interface: paths resolve to inodes, inodes are opened
into files, and files are reached through per process descriptors. The
interface header is `kernel/include/fs/vfs.h`. Structures crossing the
system call boundary (`struct stat`, `struct dirent`, open flags) live in
`kernel/include/minios/abi.h`, which libc includes directly.

## Objects

- `struct fs_type` names a filesystem and provides `mount`, which builds a
  superblock from a source string. Types register with `vfs_register_fs`.
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
- `struct inode` holds the metadata of one object: mode, link count,
  size, modification time (seconds since the epoch, reported as
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
  `mkdir`, `unlink`, `rmdir`, `link`, `rename` and `truncate`. Names are
  passed as pointer and length. The caller holds the directory inode mutex.
  A filesystem without a given operation leaves the pointer NULL and the
  VFS reports `EROFS`.
- `struct file_ops` covers open files: `open`, `release`, `read`, `write`,
  `getdents` and `lseek`. `read` and `write` receive the position by
  pointer and advance it. `getdents` fills whole `struct dirent` records
  and uses `file->pos` as the entry index.
- `struct file` is an open file description: inode, operations, position,
  flags and a reference count. Descriptors in different processes may share
  one description after `fork` or `dup`, so the position is shared as well.
- `struct fdtable` is the per process descriptor table of `OPEN_MAX`
  slots, embedded in `struct proc`. `fork` copies it with an extra
  reference per file, `exec` keeps it and process exit closes everything
  before the parent is notified so pipe peers see the end promptly.
- `struct mount` records a mounted superblock and the `(superblock, inode
  number)` pair of the directory it covers. Identifying the mount point by
  number rather than pointer means the covered inode does not need to stay
  in memory.

## Path resolution

`vfs_canonicalize` folds a path against the process working directory
into an absolute path without `.`, `..`, duplicate or trailing slashes.
The working directory is kept as a canonical string in `proc->cwd`, so
`..` is resolved lexically. Without symbolic links this matches the
directory structure.

`vfs_lookup` walks the canonical path from the root mount one component
at a time, calling `lookup` under the directory mutex and dropping the
reference to the previous directory. After each step the result is checked
against the mount table and replaced by the root of a mount that covers
it. `vfs_lookup_parent` stops before the last component and returns its
name, which the create, unlink, mkdir, rmdir, link and rename paths use.
`rename` locks two directories in inode number order so concurrent renames
between the same pair cannot deadlock.

## Filesystems in M11

- `initrd` (`fs/initrdfs.c`) exposes the ustar archive parsed by
  `fs/initrd.c` read only. Inode 1 is the root, entry i is inode i + 2. A
  directory lists entries whose name has the directory as prefix and no
  further slash.
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
process is being terminated. Each end holds a reference (`pipe.refs`);
`pipe_release` drops it as its last step, after waking the peers and the
pollers, so the pipe outlives both releases even when the two ends are
closed at the same moment on different CPUs, as happens when the
processes of a pipeline exit together.

## System calls

`open`, `close`, `read`, `write`, `lseek`, `dup`, `dup2`, `stat`,
`fstat`, `getdents`, `mkdir`, `unlink`, `rmdir`, `rename`, `link`,
`pipe`, `mount`, `umount`, `sync`, `chdir`, `getcwd` and, since the make
port (`make.md`), `utimensat` are implemented in `syscall/sys_fs.c`. User buffers are checked with `user_range_ok` and
then accessed directly. `read` and `write` on descriptors 0 to 2 go
through the console device like any other file.

## libc and programs

libc implements the calls above plus `opendir`, `readdir`, `closedir`
over `getdents`, `isatty` over `fstat`, and `fopen`, `fdopen`, `fclose`,
`fseek`, `ftell` and `rewind` over descriptors. Streams opened with
`fopen` are flushed by `exit`.

The shell parses pipelines with `|` and the redirections `<`, `>` and
`>>`. Each stage is a forked child with its ends wired through `dup2`.
Coreutils added: `cat`, `ls` (`-l`), `wc` (`-l`, `-w`, `-c`), `mkdir`,
`rm` (`-d`), `mv` and `cp`. The modifying utilities return `EROFS` on the
initrd until the disk filesystem of M13 is mounted.

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
- `pipe_close` (`kernel/tests/test_pipe.c`) has two kernel threads release
  the read ends and the write ends of 256 pipes in step, forty rounds, so
  both ends of a pipe are closed at the same moment on different CPUs; it
  hung before `pipe_release` held its own reference across the wakeups.
