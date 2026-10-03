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
