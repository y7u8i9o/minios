# make

`/bin/make` is pdpmake, the public domain POSIX make of Ron Yorston,
compiled unmodified from `third_party/make/src/`. The port consists of a
system call and the libc functions the source needed, a build rule, a
manual page and a boot test.

## Sources

`third_party/make/src/` contains the ten C files, `make.h` and the manual
page `pdpmake.1` of the upstream repository; `LICENSE`, `README.md` and
the upstream `Makefile` sit beside it, and `NOTICE` records the commit
and the date. `tools/fetch_make.sh` downloads the master branch as a
tarball and lays the files out this way; a later version is taken by
running it again. The vendored files are exempt from the minios line
count and naming conventions.

`make.h` is left as shipped: the non-POSIX extensions and the POSIX 2024
features are enabled. Its `GETOPT_RESET` falls to the default branch,
which sets `optind` to 0, and the libc `getopt` treats that as a reset.
The `+` that begins the option string is meant for glibc and is read by
the libc `getopt` as an ordinary option letter, which only matters for
an argument of `-+`.

## Build

`user/Makefile` compiles the sources into `build/user/make/` with the
user flags plus the warning exclusions of the other ports and links
`/bin/make` against libc alone.

## Kernel: utimensat

`make -t` touches its targets with `utimensat`, and a make that cannot
set modification times is of little use with `touch`, so the kernel
gained the system call. `struct stat` now carries `st_mtim`, a
`struct timespec`, and `st_mtime` is a macro for its `tv_sec`, as POSIX
specifies. The structure grew by eight bytes, which every program picks
up from `minios/abi.h`.

pdpmake compares times with `timespec_le`: a target whose time equals
the time of a prerequisite is out of date. On a filesystem with one
second resolution every target built in the same second as its
prerequisite, which is every second level target of a normal build,
would be rebuilt once more by the next run. Inode times are therefore
nanoseconds now: `vfs_now` returns nanoseconds, `inode.mtime` contains
them, `inode_stat` splits them into `st_mtim`, the mfs disk inode stores
them (format version 4; `mkfs` takes them from the host file), FAT
converts its two second entries at the boundary (`fat_epoch`,
`fat_time_of`), and initrdfs multiplies the seconds of the tar header.

`sys_utimensat(dirfd, path, times, flags)` (`kernel/syscall/sys_fs.c`)
accepts `AT_FDCWD` as the only directory descriptor and 0 as the only
flag value. A null `times` sets the current time; otherwise the second
`timespec` is the modification time, `UTIME_NOW` in its `tv_nsec` means
the current time and `UTIME_OMIT` leaves the file unchanged. The access
time is not stored and the first `timespec` is ignored. `vfs_utimens`
looks the path up and calls the new inode operation `setmtime` with the
inode lock acquired between `op_begin` and `op_end`; mfs sets `mtime` and
writes the inode through the journal (`mfs_inode_flush`), FAT converts
the time to a directory entry date and time (`fat_time_of`, which
`fat_now` now uses as well) and writes the entry
(`fat_inode_flush_time`), so FAT retains two second resolution. A filesystem without the operation, devfs and
initrdfs, returns `EROFS`.

`touch` now sets the modification time of every named file after
creating the missing ones, and `touch -c` does not create files.

## libc additions

- `utimensat` (`sys/stat.h`) with `AT_FDCWD`, `UTIME_NOW` and
  `UTIME_OMIT`.
- `strndup` and `stpcpy` (`string.h`).
- `realpath` (`stdlib.h`): the absolute path with `.`, `..` and
  repeated slashes removed, checked with `stat`. Since symbolic links
  were added (2026-09-30, `vfs.md`) it also resolves every link.
- `access` with `F_OK`, `R_OK`, `W_OK` and `X_OK` (`unistd.h`): the
  file must exist. Since U2 of the multiuser plan the kernel checks the
  permission bits against the real ids (`faccessat`, `users.md`).
- `confstr(_CS_PATH)` (`unistd.h`) returns `/usr/bin`, where make looks
  for `sh`. It returned `/bin` before P1 of `docs/plan/packaging.md` made
  `/bin` a link to `/usr/bin`.
- `ar.h`: the archive member header, which make reads to date
  `lib.a(member.o)` prerequisites.

## Shell

In POSIX mode (`--posix`, `.POSIX` or `PDPMAKE_POSIXLY_CORRECT`) pdpmake
prefixes every command with `set -e;`. The shell's `set` builtin printed
the variables for any argument other than `--`, so every command in
POSIX mode started with a listing of the environment. `set -e` and `set
+e` now switch the errexit option (`sh.md`), and unknown option letters
are refused.

## Behaviour

The manual page `make(1)` lists the options and the extensions. `-j` is
accepted and ignored. Commands run through `/bin/sh -c`, so everything
the shell supports (see `sh.md`) is available in rules. With `-n`,
pdpmake prints the commands of the targets that are out of date at the
start and does not assume that a printed command would have updated its
target, so a prerequisite chain shows only its first stage.

## Tests

`tests/cases/make` runs `/etc/tests/make.sh`, which builds a two object
program with suffix rules and checks the rebuild decisions after `touch`
(`-q`, the up to date message, `-n`, `-s`, `-t`), command errors (`-`
prefix, `-i`, `-k`, the exit status), `clean`, command line macros, `-e`,
the macro forms `=`, `:=`, `?=`, `+=` and `$(X:.c=.o)`, the conditionals,
suffix rules with `$<`, `$?` and `$@`, `include`, `-C`, double colon
rules, wildcards, `.PHONY`, `.SILENT`, `-p`, several `-f` and `--posix`.
`tests/cases/libc_ext` checks the libc additions and `utimensat`
directly. The cases `fs`, `mfs_user`, `fat_user` and `utils` cover the
changed `struct stat` and `touch`.
