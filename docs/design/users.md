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
same lock through `cred_get` or `cred_get_current`, which retains a
permission check consistent while another thread of the process changes
the ids. `proc_format_table` reads the effective uid without the lock,
since a single word cannot be torn and a stale value is harmless in a
listing.

The kernel process starts as root with no supplementary groups and the
mask 022 (`cred_init_root`). A new process copies the credentials of its
parent in `proc_setup`, under the parent's `proc.lock` together with the
resource limits, which gives fork, `proc_create_user` and the boot tests
the identity of their creator. Exec retains the credentials. The setuid and
setgid bits are applied by U2.

The system calls are

| Number | Call | Effect |
|---|---|---|
| 96 | `getresuid(r, e, s)` | stores the three user ids, NULL pointers are skipped |
| 97 | `getresgid(r, e, s)` | the same for the group ids |
| 98 | `setresuid(r, e, s)` | sets the user ids, -1 leaves one unchanged |
| 99 | `setresgid(r, e, s)` | sets the group ids, -1 leaves one unchanged |
| 100 | `getgroups(size, list)` | stores the supplementary groups, size 0 returns the count |
| 101 | `setgroups(size, list)` | replaces the supplementary groups, root only |
| 102 | `umask(mask)` | sets the mask to `mask & 0777` and returns the old one |

A process whose effective uid is 0 may set any id. Any other process may
set each id only to one of its current real, effective or saved ids, and
the whole call fails with `EPERM` otherwise. The group calls are governed by
the effective uid as well, which means that a process without root cannot
change its groups to a gid it does not already contain. libc builds `getuid`,
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
`/etc/passwd` contains `x`. Lines beginning with `#` and malformed lines are
skipped.

libc reads the files on every lookup (`lib/libc/src/pwd.c`), with `getpwnam`,
`getpwuid`, `getpwent`, `setpwent`, `endpwent` and `fgetpwent` for accounts,
the corresponding group functions, `getgrouplist` and `initgroups` for the
groups of an account, and `getspnam` and `fgetspent` for the hashes. The
results live in static storage that the next call overwrites. `getlogin`
returns `LOGNAME`, which login sets, or else the account of the real uid.

A hash is a SHA-256 crypt string (`$5$salt$hash` or
`$5$rounds=N$salt$hash`) as specified by Ulrich Drepper in 2008, computed by
`sha256_crypt` in `lib/libc/src/crypto/shacrypt.c`, which `crypt` exposes. The
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
never executable. devfs retains the mode and owner of each node in the node,
which `chmod` and `chown` change in memory until the next boot. The initrd
retains the mode, uid and gid of each tar header. The build archives the
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

## Enforcement (U2)

`vfs_permission(inode, mask, cred)` decides every check. The mask combines
`MAY_READ`, `MAY_WRITE` and `MAY_EXEC`. The owner bits apply when the
effective uid owns the inode, the group bits when the inode's group is the
effective or a supplementary group, and the other bits otherwise, without
falling through from one class to the next. Root passes every check except
the execution of a file that is not a directory and has no execute bit at
all. The checks are

| Operation | Permission |
|---|---|
| every path component that a lookup enters | search (`MAY_EXEC`) on the directory containing it |
| `open` | read, write or both by the access mode, write for `O_TRUNC`, none for a file the call created |
| create, `mkdir`, `link`, `symlink`, `unlink`, `rmdir` | write and search on the parent directory |
| `rename` | write and search on both parents, write on a directory that moves to another parent |
| removal or replacement in a directory with the sticky bit | the owner of the entry or of the directory, or root (`EPERM`) |
| `utimensat` | the owner or root for any time, write permission for the current time |
| `chdir` | search on the directory |
| exec | execute on a regular file, which need not be readable |
| `faccessat`, `access` | the requested bits with the real ids, or the effective ids with `AT_EACCESS` |

Each walk takes one snapshot of the credentials into `struct walk`, and
`vfs_access` replaces its effective ids by the real ones before the walk.
A relative path is resolved from the root along the working directory, so
search permission on the ancestors of the working directory counts for it
as well.

Exec opens the program through `vfs_open_exec`. When the program has the
set user id bit, its owner becomes the effective and saved uid of the
process, and the set group id bit does the same with its group. Such an
image receives `AT_SECURE` 1 in its auxiliary vector (`elf_info.secure`),
every other image 0.

The privileged operations require effective uid 0 and fail with `EPERM`
otherwise, namely `mount`, `umount`, `reboot` (which also remains limited to
init), `clock_settime`, setting a slew with `adjtime`, raising a hard resource limit, `prlimit` on a
process whose ids are not all the caller's effective uid, and the
`NETIOC_CONFIGURE` and `NETIOC_ARP_PROBE` requests of `/dev/net`. Binding
an AF_INET socket to a port from 1 to 1023 fails with `EACCES` for anyone
but root.

`kill` follows the rule of POSIX. Root may signal any process, and any
other sender a process whose real or saved uid equals the sender's real or
effective uid, which also governs `kill(pid, 0)`. For a process group or
-1 the signal goes to each process the sender may signal, and the call
fails with `EPERM` when the group has processes but none qualifies. The
signals that terminals, pseudo terminal hangups and `reboot` send go
through `signal_send` and `signal_send_pgrp` directly and are not checked.

Opening `/dev/ptmx` gives the slave node of the new pair to the real uid
and gid of the opener with mode 0620 (`devfs_set_owner`), and closing the
master returns it to root with mode 0600, the mode of an unused slave.

## Accounts and homes (U3)

The account databases live in `/usr/local/etc` on the data volume, and
`/etc/passwd`, `/etc/group` and `/etc/shadow` of the root image are
symbolic links to them, which retains accounts across rebuilds of the root
image. `/usr/local` is itself a symbolic link of the root image to
`/home/.local`, the package prefix that belonged to the single user
before and now belongs to root (`LOCAL_PREFIX`). Since P0 of
`docs/plan/packaging.md` packages install into the root filesystem
instead, and `/usr/local` contains the databases, the state and software
that the package installer does not manage. Without a data volume
the root image's `/home` contains the same tree.

| Path | Owner | Content |
|---|---|---|
| `/home/<name>` | the account, mode 0700 | the home, made from `/etc/skel` |
| `/home/.local` (`/usr/local`) | root | `etc/` with the databases, `state/`, software outside the packages |
| `/home/.layout` | root | the layout marker, `2` |
| `/root` | root, mode 0700 | root's home on the root image, rebuilt with it |
| `/etc/skel` | root | the skeleton of new homes, `user/skel/` in the tree |

The image ships `root` (home `/root`) and `user` (uid and gid 1000, home
`/home/user`), both with an empty hash. `user/Makefile` copies the skeleton
into `/etc/skel`, `/home/user` and `/root`, installs `user/local/` as the
prefix of the root image and of the seed `/usr/share/skel/home`, writes the
marker, sets the special modes on the build tree (`/tmp` 1777, the homes
0700, the shadow file 0600, `su` and `passwd` 4755) and links the
databases. The manifest `user/perms` gives `/home/user` to uid 1000.

The data volume's `/etc/fstab` entry carries the option `homes`. After the
mount, `fsinit` converts a volume of the single user layout, recognised by
the missing marker. Every entry but `.local` and `lost+found` moves
through a temporary directory into `user/`, which goes to uid 1000 with
mode 0700, `.local` goes to root and receives the databases of the seed,
and the marker is written. `fsinit` then makes the home of every account
of `/etc/passwd` that lies on the volume and does not exist
(`account_make_home`). `fsinit -m DIR` converts a directory alone.

`login` (run by init on the console as `console login login`, or by the
greeter in its place since 2026-10-03) asks for
the name and the password, gives the terminal to the account with mode
0620, calls `initgroups`, `setgid` and `setuid`, enters the home and runs
the shell as a login shell with `HOME`, `USER`, `LOGNAME`, `SHELL`, `TERM`
and `PATH=/usr/bin:/usr/local/bin`. At its start it returns the terminal to
root. `su` and `passwd` are set user id root. The `su` of U3 asked a
caller other than root for the target's password and read it from
standard input when that was not a terminal, and U5 replaced it with the
`su` of ubase. `passwd` changes the caller's password after the
current one, root changes any account without one, `-d` removes a
password and `-n` sets the full name. `useradd` and `userdel` are for
root. A new account gets the lowest free id from 1000 on for its uid and
its own group, a locked hash and a home from `/etc/skel`.
`account_read_password` turns echo off on a terminal, and `account_today`
gives the day count of the shadow file. The commands write the databases
through `account_replace`.

The shell prints `#` for `\$` when the effective uid is 0 and expands
`~name`. `term` sets `HOME`, `USER`, `LOGNAME` and `SHELL` of the window's
user. `conf_home` in `minios/conf.h` gives `HOME`, or the home of the real
uid when it is unset, which replaces the fixed `/home` of the desktop, the
files window, the input method daemon and the applications that save
files. `conf_user_file` and `conf_user_write_file` give the per-user
launcher menu `~/.config/launcher` and MIME handler table
`~/.config/mime.apps`, which replace `/etc/launcher` and `/etc/mime.apps`
for that user.

`pkg` refuses to change a root the caller cannot write and names
`sudo pkg`. `ld.so` searches `/usr/lib`, `/usr/local/lib` and
`$HOME/.local/lib`, the last only without `AT_SECURE`. `/etc/profile`
adds `/usr/bin`, `/usr/local/bin` and `~/.local/bin` to `PATH`, and the
panel reads the launcher entries of `/var/lib/pkg/launcher` and
`~/.local/share/launcher`. Before P0 of `docs/plan/packaging.md`
packages were installed into `/usr/local` and `pkg` offered
`--prefix ~/.local` as well.

## The graphical login (U4)

AF_UNIX sockets report the process at the other end through
`getsockopt(SOL_SOCKET, SO_PEERCRED)` as a `struct ucred` with its pid,
effective uid and effective gid (`sockets.md`). X12 reads it for every new
client (`client_attach`) and admits root, the user who runs the server
and the session user, `session_uid`, which only a root client may change
through the setting of the same name (`x12settings set session_uid UID`).
A change disconnects the clients of a user who is no longer admitted.
init uses the same option to authorize its control requests (`init.md`).

The greeter (`user/greeter/greeter.c`, `greeter(1)`) runs as root in the
console entry of init (`console greeter greeter`), which is the entry of
`/etc/init.conf` since 2026-10-03. Without `/dev/fb0`, or when X12 does
not answer within five seconds or the login window fails to start five
times in a row, it runs `login` in its place. Once X12 answers, it prints
"greeter: display server running" on the console, which the boot check of
the release pipeline waits for. The typed console tests start init with
a copy of `init.conf` whose console entry is `login`
(`ktest_start_init`), and the cases `greeter_boot` and `greeter_fallback`
boot the default configuration with and without a display. The greeter
starts X12, restarts it when it ends, and runs its login window as a
second process,
`greeter --window`, which reports `login NAME`, `poweroff` or `reboot` on
a pipe. Since 2026-10-03 the window is a layer surface over the whole
screen in the manner of GDM. Its background is the desktop colour of
`/etc/desktop.conf` with a gradient, or its wallpaper. A top bar shows the
host name, the clock and the Restart and Shut down buttons, and a card in
the middle shows one page at a time. The first page lists root and the
accounts from uid 1000 on, each with an avatar that shows its initial,
with the account after root selected. Enter, a click or the arrow keys
choose an account. The second page checks the password with
`account_check` in a masked text field (`textfield_set_masked`, which
shows one `*` per byte and never copies its text) and pauses after a
wrong one. The third page asks an account without a password for a new
one. For a login the
greeter sets the session uid of X12 and of init and starts
`startgui -s`, which starts the session programs without a server, as
the account in a process group of its own, with `initgroups`, `setgid`,
`setuid`, its home and a fresh environment. After the panel's Log out it
ends that process group, clears both session uids and shows the window
again. Running the window as a separate process gives every login a new
connection to X12 and a new toolkit state. `greeter -a NAME` starts the session of the
account `NAME` once without the login window and without a password, and
the login window follows when that session ends. The live medium uses the
option for its account `live` (`live.md`).

The panel shows the account name at the end of the Log out row of its
menu. The settings program gained the Users page (`user/settings/users.c`).
It lists the accounts, sets the full name of the user's own account with
`passwd -n`, changes its password with `passwd`, and adds and removes
accounts. The passwords go to these programs through a pipe, one per
line, and `passwd` reads a line for every password it could ask for when
its input is not a terminal, which lets the page pass them without
knowing which accounts have one. Until U5 the page added and removed
accounts through `su -c` with the root password, and since U5 it uses
`sudo` with the password of the logged in user (below).

## su, doas and sudo (U5)

Three existing programs give a user the rights of another account, each
compiled without changes to its source. `tools/fetch_privilege.sh` places
their sources in `third_party/` (`ubase/`, `opendoas/` and `sudo/`, each
with a file `ORIGIN` naming the revision), and `user/Makefile` builds them
into `/bin` as set user id root programs.

| Program | Source | Configuration | Manual |
|---|---|---|---|
| `su` | ubase, commit e8249b4 | none | `su(1)` |
| `doas` | OpenDoas 6.8.2 | `/etc/doas.conf` | `doas(1)`, `doas.conf(5)` |
| `sudo` | sudo 1.9.17p1 | `/etc/sudoers` | `sudo(1)`, `sudoers(5)` |

The `su` of ubase takes `-l` and `-p` and no command. It checks the
password of the target account with `crypt` against `/etc/shadow` and
refuses an account with an empty or locked hash to a caller other than
root, for which `crypt` returns `*0` on a setting it does not support
instead of a null pointer. OpenDoas is built with `USE_SHADOW` and a
`config.h` that `user/Makefile` writes, the compatibility sources of its
`libopenbsd` directory for the functions the libc lacks, and its grammar
`parse.y` translated by the host `yacc`. Without `USE_TIMESTAMP` doas
asks for the password every time and ignores the rule option `persist`.

sudo has a configure script that the cross compiler ran once against the
minios libc with the sudoers policy linked statically, without PAM, mail,
the log server, LDAP, Python or zlib. The headers it generated
(`config.h`, `pathnames.h`, `sudo_usage.h`) and its signal name table
`signame.c` are retained in `user/ports/sudo`, and `user/Makefile` compiles
the objects that the configured makefiles list from `src/`,
`plugins/sudoers/`, `lib/util/`, `lib/iolog/`, `lib/eventlog/` and
`lib/protobuf-c/` into one static program linked against the shared
libc. Its time stamps are retained in `/var/run/sudo/ts` and the lecture
records in `/var/db/sudo/lectured`, directories the image creates. A
time stamp is valid for one terminal and one session, which sudo
recognises by the session id of the caller.

The image ships the group `wheel` (gid 10) with `user` as its member.
`/etc/doas.conf` permits the members of wheel with their password and
root without one, and `/etc/sudoers` (mode 0440) permits root and the
members of wheel and sets the secure path `/usr/bin:/usr/local/bin`. Both
files are part of the root image. The membership of wheel lives in
`/etc/group` on the data volume. A volume set up before U5 has a group
file without wheel. `fsinit` adds `wheel:x:10:` with the account of uid
1000 to such a file when it mounts the volume, unless gid 10 is in use.

Until 2026-10-05 the Users page of settings ran `sudo -S -k -p ''` with
the password of a field Your password as the first line of the input.
Since then it uses the authentication dialog (below).

The programs needed the following additions to the kernel and the libc.

- `struct proc` has a session id beside its process group, both under
  `proc_tree_lock`. `setsid` (111) makes the caller the
  leader of a new session and process group and refuses a process group
  leader, and `getsid` (112) reports the session. init starts its console
  entry and the greeter starts a graphical session with `setsid`.
  Sessions carry no controlling terminal.
- `/dev/tty`, mode 0666, opens the terminal on the first of the
  descriptors 0 to 2 that is the console or the slave side of a pseudo
  terminal. It is where `getpass` and the three programs read passwords.
- The terminal retains the whole `struct termios` with the flag values of
  Linux, `TCSETS` stores it, and `TCFLSH` and `TCSAFLUSH` discard the
  typed input. Password prompts flush that input as on other Unix
  systems.
- Signals deliver a `siginfo_t` to handlers installed with `SA_SIGINFO`,
  with `si_pid` and `si_uid` of the sender for `kill` (`SI_USER`) and
  `SI_KERNEL` for signals of the kernel. `SA_RESETHAND` is honoured.
  `sigpending` (109) and `alarm` (110) are new, and `alarm` retains its
  timers in a list that the timer interrupt checks (`kernel/ipc/alarm.c`).
- `fchdir` (108) changes the directory to an open one, and `fcntl` accepts
  the record locks `F_GETLK`, `F_SETLK`
  and `F_SETLKW` as no-ops, which programs that lock their own files on a
  single system need. `ftruncate` works on regular files through
  `vfs_truncate`, which `O_TRUNC` uses too.
- The libc gained `syslog` (appending to `/var/log/messages`), `getpass`,
  `ttyname`, `sysconf`, `gethostname`, `killpg`, `clearenv`, `strsep`,
  the reentrant account lookups `getpwnam_r` and the like, `getspent`,
  `openpty` with the BSD signature, the `cf*` and `tc*` terminal
  functions, and the headers `paths.h`, `utmp.h`, `poll.h`, `utime.h`,
  `alloca.h`, `net/if.h` and `netinet/tcp.h` (`libc.md`).

## The authentication dialog (2026-10-05)

A program of the desktop runs a command as root through
`app_run_privileged` of libgui (`lib/libgui/src/command.c`,
`<gui/privilege.h>`). root runs the command directly. Another user runs
`sudo -A -p REASON -- COMMAND`, and `/etc/sudo.conf` names
`/usr/bin/askpass` as the askpass program of sudo. sudo runs askpass with
the prompt as its argument and the credentials of the user, and reads the
password from its standard output. Every program that runs `sudo -A`, also
in the terminal, therefore gets the same dialog. A prompt that gives no
reason, such as the own prompt of sudo, shows "Authentication is required
to run a command as root."

askpass (`user/askpass/askpass.c`, package `desktop`) is a layer surface
of the overlay layer with the size of the screen, in the manner of the
authentication dialog of GNOME. The screen is dimmed around a card in the
middle, which shows the reason, the avatar and the full name of the user
(`painter_avatar`, shared with the greeter), the password field, and the
buttons Cancel and Authenticate. Enter in the field authenticates, and
Escape cancels. The window has ARGB buffers (`gui_set_translucent`). Its
opaque region is the card, and X12 blends the rest, which askpass fills
with black of alpha 0x80. The overlay receives the keyboard when it maps.
While an overlay has the keyboard, X12 refuses Alt+Tab and Alt+F4, and a
click reaches no window below it, including the server side title bars.

sudo runs askpass again after a wrong password, up to three times. Each
run of sudo has a state file, `/tmp/.askpass-UID-PID` with the pid of
sudo, which askpass creates at its first prompt. A later prompt that finds
the file shows "The password was wrong. Try again." Cancel writes the
word `cancel` into the file and ends askpass with status 1, after which
sudo fails. `app_run_privileged` reads and removes the file after sudo and
returns `-ECANCELED` for a cancelled dialog. askpass removes the state
files of runs of sudo that have ended when it starts.

`app_run_privileged` runs the command with `app_run_command`, which
collects its output through a pipe watched by the event loop of the
application. The windows of the application therefore continue to paint
and to answer X12 while the dialog is open, and X12 does not report the
program as not responding. The caller waits in a nested loop. A second
command cannot start during that time (`-EBUSY`). The time stamp of sudo
applies, so a second change within its validity runs without the dialog.

The Region and language page sets the time zone with `ln -sf ZONE
/etc/localtime` through `app_run_privileged`. The zones selected while the
dialog is open are applied after it, the last one once. A cancelled or
failed change selects the zone of `/etc/localtime` again. The Users page
adds accounts with `useradd -c NAME ... && passwd ...` and removes them
with `userdel -r NAME` the same way, and `passwd` reads the password of
the new account twice from the input.

## The display of a console session (2026-10-03)

login gives `/dev/fb0` to the account of the session, as it gives the
terminal, and returns both to root before it asks for the next name.
`startgui` started from a console session runs X12 as the account, which
admits the user who runs it. Before this change X12 failed with
"framebuffer: Permission denied" for every account but root, because
`/dev/fb0` has mode 0600. The input devices are readable by everyone, and
the greeter runs X12 as root. The case `login_gui` logs in as user on the
console, runs `startgui` and finds the panel and the terminal mapped.

## The first password (2026-10-03)

The image ships root and user with an empty hash. An empty hash accepts
only the empty password, and every account that logs in with it has to
choose a password before its session starts. login prints "The account
NAME has no password. Choose one now." and asks for the new password
twice until both entries match and are not empty. The greeter opens a
second window for the same purpose. Both store the hash with
`account_set_password` from `minios/account.h`, which hashes the password
with `account_hash` and writes it with `account_set_hash`. The second
function took over the shadow update of `passwd`, which preserves the aging
fields of the line and sets the day of the last change. An account whose
password `passwd -d` removed is asked again at its next login. su, doas
and sudo accept no empty password, which leaves the console login and the
greeter as the only places where an account without a password can be
used.

## Test

The case `user_cred` runs `/bin/credtest` as root. It checks the initial
identity and mask, drops a child to uid 1000 with groups 1000 and 50 and
checks that the child cannot regain root or change its groups, that
`/dev/proc` reports its uid, that a grandchild inherits the ids and that an
exec of `credtest --check` retains them. A second child retains root in its
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

The case `perm_user` runs `/bin/permtest` as root. It builds a tree below
`/tmp/perm` with files and directories of several modes, a sticky
directory, programs without execute bits and with execute bits only, and a
setuid root copy of itself. Children that drop to uid 1000 then check the
refused and the permitted file operations, the sticky bit, the rename of a
directory to another parent, `utimensat`, `access`, exec, the setuid copy
(which must report `AT_SECURE` 1 and euid 0 and see `access` refuse what
`open` allows), the privileged operations, signals to processes of root
and of their own, and the ownership of a pseudo terminal. Root finally
checks that it retains its access but cannot execute a file without execute
bits.

The case `login_console` types on the console before init starts. root
logs in, sets its password, creates `anna` and gives her a password, and
logs out. A wrong password for `anna` fails with "Login incorrect", the
right one gives a shell in `/home/anna` with the console hers, in which
`doas` and `sudo` refuse her because she is not a member of wheel and
`/etc/shadow` cannot be read. root logs in again, removes `anna` with her
home and powers off. The case `fs_migrate` runs `/bin/migratetest`, which
builds a directory of the single user layout below `/tmp`, including an
entry named `user`, a symbolic link and a package prefix, converts it with
`fsinit -m` and checks the result and that a second run changes nothing.

The case `gui_greeter` starts the greeter with `-s`, after a short first
process that takes pid 1, which the kernel protects from `SIGKILL`. It
waits for the login window, logs in the preselected account `user` with
Enter, finds the panel and the desktop running as uid 1000 in
`/dev/proc`, chooses Log out in the panel's menu, finds the login window
again, and has `doas -u user` run a settings request of uid 1000, which
X12 must refuse. `pause=1` leaves the window open for screenshots. `gui_settings`
opens the Users page with the other pages.

The case `gui_askpass` sets the password of `user`, starts X12 with the
session uid 1000 and runs `settings region` as `user` through `doas -u`.
The first change of the time zone shows the dialog, and a pixel above the
window must lose at least a quarter of its brightness. A wrong password
shows the dialog again, and the right one sets the zone. The second change
passes through the time stamp of sudo without a dialog. After `sudo -K`
the third change shows the dialog, and Escape cancels it. `/etc/localtime`
must then name the second zone. The qmp file of the case takes a
screenshot of the dialog.

The case `privilege` drives init on the console with pauses, because
password prompts discard the input typed before them. root sets the
passwords of root and `user` and writes a doas rule file that denies.
`user` runs `doas id -u`, checks the rule file with `doas -C`, enters a
root shell with `su`, finds the doas command in `/var/log/messages`, runs
`sudo` with its lecture and prompt, fails `sudo -S -k` with a wrong
password and succeeds with the right one, adds an account with the
command line of the Users page, reads its hash with `sudo -n` through the
time stamp, and removes it again. Each result passes through `sed`,
which adds a tag that the echo of the typed command does not contain.
