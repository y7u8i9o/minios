# Users

This document describes the multiuser support of minios as implemented by
the milestones of `docs/plan/multiuser.md`.

## Credentials (U0)

Every process carries a `struct cred` (`kernel/include/sched/cred.h`) with
its real, effective and saved user ids, its real, effective and saved group
ids, up to `NGROUPS_MAX` (16) supplementary groups and its file creation
mask. The credentials belong to the process rather than to its threads,
which matches what POSIX requires of `setuid` in a threaded process. They
are written under `proc.lock`, and every reader takes a snapshot under the
same lock through `cred_get` or `cred_get_current`, which keeps a
permission check consistent while another thread of the process changes
the ids. `proc_format_table` reads the effective uid without the lock,
since a single word cannot be torn and a stale value is harmless in a
listing.

The kernel process starts as root with no supplementary groups and the
mask 022 (`cred_init_root`). A new process copies the credentials of its
parent in `proc_setup`, under the parent's `proc.lock` together with the
resource limits, which gives fork, `proc_create_user` and the boot tests
the identity of their creator. Exec keeps the credentials. The setuid and
setgid bits are applied by U2.

The system calls are

| Number | Call | Effect |
|---|---|---|
| 96 | `getresuid(r, e, s)` | stores the three user ids, NULL pointers are skipped |
| 97 | `getresgid(r, e, s)` | the same for the group ids |
| 98 | `setresuid(r, e, s)` | sets the user ids, -1 keeps one |
| 99 | `setresgid(r, e, s)` | sets the group ids, -1 keeps one |
| 100 | `getgroups(size, list)` | stores the supplementary groups, size 0 returns the count |
| 101 | `setgroups(size, list)` | replaces the supplementary groups, root only |
| 102 | `umask(mask)` | sets the mask to `mask & 0777` and returns the old one |

A process whose effective uid is 0 may set any id. Any other process may
set each id only to one of its current real, effective or saved ids, and
the whole call fails with `EPERM` otherwise. The group calls are governed by
the effective uid as well, which means that a process without root cannot
change its groups to a gid it does not already hold. libc builds `getuid`,
`geteuid`, `getgid`, `getegid`, `setuid`, `setgid`, `seteuid`, `setegid`,
`setreuid` and `setregid` on these calls. `setuid` and `setgid` set all
three ids for root and only the effective id otherwise, and `setreuid`
moves the saved id to the new effective id when the real id is set or the
effective id differs from the old real id, as POSIX describes.

`/dev/proc` has a UID column with the effective uid of each process,
between RSS and NAME. `ps` prints it as a USER column with the account
name, and `sysmon` shows it as the User column of its process table.

## Account databases (U0)

The accounts are lines of `/etc/passwd` in the form
`name:password:uid:gid:gecos:home:shell`, the groups lines of `/etc/group`
in the form `name:password:gid:member,member`, and the password hashes lines
of `/etc/shadow` in the form `name:hash:lastchange:min:max:warn:inactive:expire:flag`,
of which minios uses the name and the hash. The password field of
`/etc/passwd` holds `x`. Lines beginning with `#` and malformed lines are
skipped.

libc reads the files on every lookup (`libc/src/pwd.c`), with `getpwnam`,
`getpwuid`, `getpwent`, `setpwent`, `endpwent` and `fgetpwent` for accounts,
the corresponding group functions, `getgrouplist` and `initgroups` for the
groups of an account, and `getspnam` and `fgetspent` for the hashes. The
results live in static storage that the next call overwrites. `getlogin`
returns `LOGNAME`, which login sets, or else the account of the real uid.

A hash is a SHA-256 crypt string (`$5$salt$hash` or
`$5$rounds=N$salt$hash`) as specified by Ulrich Drepper in 2008, computed by
`sha256_crypt` in `libc/src/crypto/shacrypt.c`, which `crypt` exposes. The
host crypto test of `make check-pkg` compares it with hashes made by
`openssl passwd -5`. `minios/account.h` adds the helpers that the account
programs share. `account_hash` hashes a password with a salt of 16
characters from `/dev/urandom`, and `account_check` compares a password with
a stored hash in time independent of the position of the first difference,
accepting only the empty password for an empty hash and nothing for a hash
that starts with `!` or `*`. `account_replace` rewrites the line of one
name in a database, appends it or removes it, writing the new contents to
a temporary file beside the target of any symbolic link and renaming it
over the target with the mode and owner of the old file. `account_name_valid`
accepts names of up to 32 characters that start with a lowercase letter and
continue with lowercase letters, digits, `_` and `-`.

The root image ships the accounts `root` (uid 0, home `/root`) and `user`
(uid 1000, group `user` 1000, home `/home/user`), both with an empty hash.

The commands `id`, `whoami` and `groups` print the ids of the caller or of
an account (`id(1)`).

## Ownership (U1)

Every inode carries an owner and a group (`inode.uid`, `inode.gid`),
reported by `stat` as `st_uid` and `st_gid`. The permission bits of
`inode.mode` and the owner are protected by `inode.lock`, while the file
type bits never change. A new file, directory or symbolic link takes the
effective uid of its creator and its effective gid, or the group of the
directory when that directory has the set group id bit, which a new
directory also inherits (`vfs_new_owner`, `vfs_mkdir`). The requested mode
is masked by the umask of the creator in `open_create` and `vfs_mkdir`.
Objects without an inode, such as pipes and sockets, report the caller's
own ids from `fstat`.

The system calls `fchmodat` (103), `fchmod` (104), `fchownat` (105) and
`fchown` (106), with `AT_SYMLINK_NOFOLLOW` for the `at` forms, reach
`vfs_chmod_inode` and `vfs_chown_inode`, which decide whether the caller
may make the change and then call the `setattr` operation of the
filesystem inside one journaled operation.

| Change | Allowed for |
|---|---|
| permission bits | the owner and root, the set group id bit only for members of the file's group (it is dropped for others) |
| owner | root |
| group | root, and the owner to one of the owner's groups |

A change of owner or group by anyone but root clears the set user id and
set group id bits of anything but a directory.

mfs stores the owner in two former spare words of its disk inode (format
version 5, `mfs.md`). FAT stores no owner and no permissions. Its mount
options `uid=`, `gid=` and `umask=` give every file of the volume an owner,
a group and the bits `0666` (files) or `0777` (directories) less the mask
(default 022), less the write bits for an entry with the read only
attribute. `chmod` on FAT can only set or clear the owner's write bit,
which maps to that attribute, and `chown` fails with `EPERM`. FAT files are
never executable. devfs keeps the mode and owner of each node in the node,
which `chmod` and `chown` change in memory until the next boot. The initrd
keeps the mode, uid and gid of each tar header. The build archives the
initrd with owner and group 0.

`mount` takes the options as a fourth argument, which libc exposes as
`mount_options`, `mount -o` passes them on, and `fsinit` hands every
option of `/etc/fstab` that it does not interpret itself to the
filesystem. mfs, devfs and the initrd refuse any option with `EINVAL`.

`mkfs` gives the root image the permission bits of the build tree and
root as the owner of everything, and the manifest `user/perms` (`mkfs -p`)
lists the exceptions as `path mode uid gid` lines, among them `/tmp` with
mode 1777 and `/etc/shadow` with mode 0600. `mkfs --dump` prints mode,
uid and gid of every entry. `fsck` accepts versions 4 and 5 and creates
`lost+found` with mode 0700.

The commands `chmod` (octal and symbolic modes, `-R`), `chown` and `chgrp`
(`-R`, `-h`) change modes and owners, `ls -l` prints owner and group
columns and the set id and sticky letters, `stat` prints uid and gid, and
the shell has a `umask` builtin.

## Test

The case `user_cred` runs `/bin/credtest` as root. It checks the initial
identity and mask, drops a child to uid 1000 with groups 1000 and 50 and
checks that the child cannot regain root or change its groups, that
`/dev/proc` reports its uid, that a grandchild inherits the ids and that an
exec of `credtest --check` keeps them. A second child keeps root in its
saved uid and moves its effective uid back and forth until `setreuid`
discards the saved root. The program then parses account files with
malformed lines, looks up the accounts of the image, checks the password
helpers and edits a database through a symbolic link.

The case `fs_owner` is a kernel test with an empty mfs volume on vdb and an
empty FAT12 volume on vdc. It creates files, directories and a link as
root and as uid 1000 with umask 077 and the supplementary group 50, checks
the modes and owners, the inheritance below a set group id directory, the
rules for `chmod` and `chown` and the clearing of the set id bits, and
finds every value again after an unmount and a new mount. It then mounts
the FAT volume with and without owner options and changes a devfs node.
