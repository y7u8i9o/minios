# ar and tar

`/bin/ar` is a minios utility in `user/coreutils/ar.c` and `/bin/tar` is
the tar of sbase, the suckless base utilities, compiled unmodified from
`third_party/sbase/`. This document records both, the kernel and libc
additions they needed, and the tests.

## ar

No small implementation of ar with a BSD or MIT licence writes the
System V archive format: the 4.4BSD ar writes the BSD variant with
`#1/n` names, which pdpmake cannot read for `lib.a(member.o)`
prerequisites, and the FreeBSD and NetBSD programs depend on libarchive
or binutils. ar is therefore written for minios. It produces the format
of the GNU binutils: the magic `!<arch>\n`, a 60 byte printable header
per member, short names terminated by a slash, names longer than 15
characters in the `//` table referenced as `/offset`, and member data
padded to an even length with a newline. Archives from the host
`x86_64-elf-ar` are read and written unchanged, except that a symbol
table (`/`) is dropped when the archive is rewritten, because nothing
on minios reads one. BSD `#1/n` names are understood when reading.

The archive is read whole into memory and the member list is rebuilt
into a temporary file beside it, which is renamed over the archive. The
operations are `d`, `p`, `q`, `r`, `t` and `x` with the modifiers `c`,
`o`, `u`, `s` (accepted) and `v`; `ar(1)` describes them. The member
date is the file's `st_mtime` in seconds, which is what pdpmake reads
back with `artime`. pdpmake compares that whole second with the
nanosecond time of the member's file and treats an equal second as out
of date, so a `lib.a(member.o)` target is rebuilt whenever the file has
a fractional time; pdpmake behaves the same on every system.

## tar

`third_party/sbase/src/` contains `tar.c`, its manual page, the headers it
includes and the whole `libutil/` directory; `NOTICE` records the
commit, and `tools/fetch_tar.sh` downloads them again. `user/Makefile`
compiles `tar.c` and the six library files it needs, writes the empty
`compat.h` that `util.h` expects into `build/user/tar/`, and renames
sbase's `strlcpy` with `-Dstrlcpy=sbase_strlcpy` so that it does not
collide with the libc definition. `tar(1)` describes the options.

The recursion of sbase (`libutil/recurse.c`) walks directories with
`openat`, `fstatat` and `fdopendir` relative to a directory descriptor,
which the kernel did not have. `tar -z` runs `gzip -cf` and `gzip -cdf`
through `execlp` in a child connected by a pipe and does not wait for
that child, so `tar -czf` can return before the archive is completely
written; a command that reads the archive at once must wait (the test
sleeps one second). `gzip` gained `-f`, which it accepts without effect
because it always replaces its output.

## Kernel: openat and fstatat

The VFS resolves paths as strings from the working directory of the
process, so a lookup relative to a descriptor needs the path of that
directory. `struct file` gained `path`, the canonical path of a
directory opened by name, set in `vfs_open` and freed with the file.
`copy_path_at` in `kernel/syscall/sys_fs.c` copies the user path and
prefixes it with `file.path` when the path is relative and the
descriptor is not `AT_FDCWD`; a descriptor without a path yields
`ENOTDIR`. `sys_openat` and `sys_fstatat` are `open` and `stat` over
that helper, and `sys_utimensat` now accepts a directory descriptor as
well. `AT_SYMLINK_NOFOLLOW` was accepted and ignored until symbolic
links were added on 2026-09-30 (`vfs.md`); since then it selects the link
itself, and tar archives and extracts links with typeflag `2`.

A program under a boot test that leaves children behind, as tar does
with its gzip child, left zombies adopted by the kernel process, which
the `run` test counted as leaked pages. `proc_reap_children` reaps
every child of a process; `test_run` calls it on the kernel process
after the program exits.

## libc additions

- `openat`, `fstatat` (`fcntl.h`, `sys/stat.h`) with `AT_FDCWD`,
  `AT_SYMLINK_NOFOLLOW` and `AT_REMOVEDIR`.
- `pwd.h` and `grp.h`: `getpwuid`, `getpwnam`, `getgrgid`, `getgrnam`
  describe the single user, name `user`, uid and gid 0, home `/home`,
  shell `/bin/sh`; other ids and names fail with `ENOENT`. Since U0 of the
  multiuser plan they read `/etc/passwd` and `/etc/group` (`users.md`).
- `getuid`, `geteuid`, `getgid`, `getegid` return 0; `lchown` returns 0
  without effect like `chown`. Since U0 and U1 they report and change the
  real credentials and owners (`users.md`).
- `symlink` failed with `EPERM` and `readlink` with `EINVAL` until
  2026-09-30, when both became system calls; `mknod` and `mkfifo`
  fail with `EPERM`.
- `sys/sysmacros.h`: `major`, `minor` and `makedev` over the 32 bit
  halves of `st_rdev`.
- `execl` and `execlp`.

## Tests

`tests/cases/ar` runs `/etc/tests/ar.sh`: creation with and without the
message, listing, header layout, printing, long names and the `//`
table, in place replacement, verbose forms, extraction, deletion, `q`,
`u`, the error cases, and a makefile with a `libm.a(m.o)` target that
make dates through the member header. `tests/cases/tar` runs
`/etc/tests/tar.sh`: creation, listing, the ustar magic and block size,
extraction into another directory with `-C`, verbose forms, a single
member, `-z` through gzip, retained and current times (`-m`), an archive
written by the host tar (`/etc/tests/fixture.tar`), the error cases and
the standard output and input forms. `tests/cases/libc_ext` checks the
libc additions and the two system calls directly.
