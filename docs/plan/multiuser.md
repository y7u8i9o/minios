# Multiuser

This plan turns minios into a multiuser system with credentials, ownership
on disk, enforced permissions, accounts, a console login and a graphical
login. Milestone identifiers use the prefix `U` so that they do not
renumber the development milestones. Each milestone ends with a boot test,
extends `docs/design/users.md`, and is marked completed here when its boot
tests pass. The work is on the branch `bleeding-edge-multiuser`.

## 1. Motivation and scope

minios was designed for a single user. Processes carry no identity,
`struct inode` has a mode but no owner, the mfs disk inode stores no uid or
gid, libc returns uid 0 from constant functions, `chmod` and `chown` do
nothing, and init starts `sh` on the console with `HOME=/home`. The data
volume mounted at `/home` is itself the home of the one user and also holds
the packages in `/home/.local`.

After this plan every process has real, effective and saved ids and
supplementary groups, files have an owner and a group on mfs, FAT, devfs
and the initrd, the kernel checks permissions on every path operation, on
exec and on signals, and privileged operations require root. Accounts live
in `/etc/passwd`, `/etc/group` and `/etc/shadow`, homes in `/home/<name>`,
packages in the shared prefix `/usr/local`, and a normal boot ends in a
console login or a graphical greeter. Boot tests keep starting their
programs directly as root.

## 2. Fixed decisions

- Credentials live in `struct proc` as `struct cred`, which holds the real,
  effective and saved uid and gid, up to 16 supplementary groups and the
  umask. They are written under `proc.lock` and read through a snapshot
  under that lock. Processes created by the kernel, init and every boot test
  inherit uid 0 from the kernel process.
- The identity system calls take the numbers from 96 on in
  `kernel/include/syscall_nums.h`. libc builds the POSIX forms (`getuid`,
  `setuid`, `seteuid`, `setreuid`, `chmod`, `lchown`, `access` and the
  others) on top of them.
- Inode ownership uses two of the twelve spare words of the mfs disk inode.
  Existing images read zero there, which means root.
- `inode.mode`, `inode.uid` and `inode.gid` are protected by `inode.lock`.
  The file type bits never change and may be read without it.
- Root (euid 0) bypasses permission checks, except that executing a regular
  file requires at least one execute bit.
- Accounts are kept in `/etc/passwd`, `/etc/group` and `/etc/shadow` in the
  familiar formats, with SHA-256 crypt (`$5$`) for the hashes. The real
  files live in `/usr/local/etc` on the data volume, and the root image
  holds symbolic links to them, which keeps accounts across rebuilds of
  the root image. The image ships `root` (uid 0) and `user` (uid 1000),
  both without a password until `passwd` sets one.
- `/usr/local` on the root image is a symbolic link to `/home/.local`, the
  directory that already holds the packages. Homes are `/home/<name>`, mode
  0700, created from `/etc/skel`.
- The X12 server, audiod and the greeter run as root. Sessions are clients,
  and X12 accepts connections from root and the uid of the current session,
  identified through `SO_PEERCRED`.

## 3. Milestones

### U0. Kernel credentials and the account database (completed 2026-10-03)

- `struct cred` in `kernel/include/sched/cred.h` is part of `struct proc`,
  inherited at process creation and kept across exec.
- The system calls `getresuid` (96), `getresgid` (97), `setresuid` (98),
  `setresgid` (99), `getgroups` (100), `setgroups` (101) and `umask` (102)
  in `kernel/syscall/sys_cred.c`, with the POSIX rule that an unprivileged
  process may set each id only to one of its current real, effective or
  saved ids.
- `/dev/proc` gains a UID column with the effective uid. `ps` shows a USER
  column and `sysmon` a User column, both through `getpwuid`.
- libc reads `/etc/passwd`, `/etc/group` and `/etc/shadow` (`getpwnam`,
  `getpwuid`, `getpwent`, `fgetpwent`, `getgrnam`, `getgrgid`, `getgrent`,
  `fgetgrent`, `getgrouplist`, `initgroups`, `getspnam`, `fgetspent`), and
  provides `crypt`, `getlogin`, the id wrappers and the editing helpers of
  `minios/account.h`.
- The commands `id`, `whoami` and `groups`.
- The boot test `user_cred` runs `/bin/credtest`, which checks the root
  identity, drops to uid 1000 in children and checks that it cannot regain
  root, that fork and exec keep the ids and that `/dev/proc` reports them,
  moves between the effective ids with root in the saved uid, checks
  `umask`, parses account files with malformed lines, and checks the
  password helpers and the line editor of the account files. `initctl` and
  `lua_sys` must still pass.
- During the work the parsers of the account files moved into the boot
  test instead of a host test, because the host C library has its
  own `struct passwd`. The SHA-256 crypt vectors, made with
  `openssl passwd -5`, joined the host crypto test of `make check-pkg`.

### U1. Ownership and modes in the filesystems

- `struct inode` gains `uid` and `gid`, reported by `stat`. A `setattr`
  inode operation changes mode and owner as one journaled metadata update.
- The system calls `fchmodat`, `fchmod`, `fchownat` and `fchown`.
- New files, directories and symbolic links take the creator's effective
  uid and gid and the requested mode masked by the umask.
- FAT reports the owner and mode of the mount options `uid=`, `gid=` and
  `umask=` and refuses `chown`. devfs nodes carry an owner. The initrd keeps
  the tar header's mode, uid and gid.
- `mkfs` takes the permission bits of the host files and reads the manifest
  `user/etc/perms` for owners and special modes. `fsck` accepts the new
  format version.
- The commands `chmod`, `chown` and `chgrp`, owner and group columns in
  `ls -l` and `stat`, and the `umask` builtin of the shell.
- In the boot test `fs_owner`, files created as several uids on mfs keep
  their owners and modes across a remount, and FAT and devfs report theirs.
  `mfs`, `fat`, `mfs_user` and `user_cred` must still pass.

### U2. Permission enforcement

- `vfs_permission` checks search permission on every path component, read
  and write access on open and truncation, write and search permission on
  the parent for creation, removal, links and renames, the sticky bit, and
  ownership for `utimensat`, `chmod` and `chown`.
- Exec requires an execute bit. The setuid and setgid bits set the
  effective and saved ids, and the kernel passes `AT_SECURE`, which makes
  `ld.so` ignore `LD_LIBRARY_PATH` and the library path in the home.
  `faccessat` checks with the real ids.
- Mount, unmount, reboot, setting the clock, raising a hard resource limit,
  network configuration and binding ports below 1024 require root.
- Signals follow the POSIX permission rule. Signals generated by terminals
  are not checked.
- The slave side of a pseudo terminal belongs to the opener of the master.
- In the boot test `perm_user`, `/bin/permtest` checks every denial and
  permitted case as uid 1000, including a setuid program, the sticky `/tmp`
  and signals across users. `shell`, `shell2`, `initctl`, `lua_sys`,
  `fs_owner` and `user_cred` must still pass.

### U3. Accounts, home directories and the console login

- `/etc/skel` replaces the old home skeleton, homes move to `/home/<name>`,
  and the account databases move to `/usr/local/etc` with links in `/etc`.
- `fsinit` migrates a volume of the old layout into `/home/user`, keeps the
  packages in `/home/.local` and creates missing homes after every mount.
- The commands `login`, `su`, `passwd`, `useradd` and `userdel`. init runs
  `login` on the console.
- The shell prints `#` for root and expands `~name`, `term` takes the
  account of its user, and every fixed `/home` path follows `$HOME`, the
  account or `/usr/local`.
- `pkg` installs into `/usr/local` as root. `ld.so`, `PATH` and the panel
  search `/usr/local` and `~/.local`. The launcher and MIME settings are
  saved per user.
- The boot test `login_console` logs in, runs `id`, `passwd` and `su` and
  logs out, and `fs_migrate` runs `fsinit -m` on an old layout. The cases of
  every touched module must still pass.

### U4. The graphical login and session

- `greeter` runs as root, supervises X12, shows a login window with the
  accounts, Restart and Shut down, and starts `startgui -s` as the chosen
  user. Logging out returns to the greeter.
- AF_UNIX sockets report the peer's credentials through `SO_PEERCRED`. X12
  accepts root and the session user, and init accepts service control only
  from root.
- The panel menu shows the user name. The settings program gains a Users
  page for passwords and accounts.
- The message domain `greeter` with catalogues for es, fr, ja, ru and zh_CN.
- The boot test `gui_greeter` logs in through the greeter, finds the panel
  running as uid 1000, logs out, and finds the greeter again. `gui_settings`
  opens the Users page, and `gui_desktop`, `gui_files`, `comp_shell` and
  `initctl` must still pass.
