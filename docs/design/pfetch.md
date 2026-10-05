# pfetch

`pfetch` prints the logo of the system with the user, the operating
system, the host, the kernel, the uptime, the number of packages and the
memory. It is pfetch 0.7.0 of Dylan Araps (MIT licence), one POSIX `sh`
script. Neofetch was not ported, because it requires bash.
`tools/fetch_pfetch.sh` places the unchanged script in `third_party/pfetch`
with its licence and the commit it was taken from (`ORIGIN`).
`user/Makefile` applies `user/ports/pfetch/minios.patch` and installs the
result as `/usr/bin/pfetch`, in the package `pfetch`, which the group
`standard` and therefore `desktop-system` contain.

## Support for minios

`uname -s` reports `minios`, and the patch adds that case to the functions
of the script:

| Field | Source |
|---|---|
| title | `uname -n`, since minios has no `hostname` command and no `/etc/hostname` |
| host | the first `product` of `/dev/devices`, the system of the SMBIOS tables, else its `model` from the device tree (`sysinfo.md`) |
| uptime | the `uptime` property of the node `system` of `/dev/devices` |
| pkgs | one `/var/lib/pkg/NAME/manifest` per installed package (`packages.md`) |
| memory | `MemTotal` minus `MemFree` of `/dev/meminfo` |
| logo | a small m in blue |

The operating system and the kernel need no change: the general case of
pfetch shows `minios 0.4.0` and the release of `uname -r`.

## Changes to the shell

The script needed three POSIX features that `/bin/sh` lacked: `exec`
without a command, which redirects the descriptors of the shell, `trap`
with the `EXIT` condition, and `set -f`. Parameter expansion inside
`$((...))` was also missing. `sh.md` describes them.

## Test

The case `pfetch` runs `sh /usr/bin/pfetch`, because the kernel executes
no `#!` scripts directly, and expects the title `root@minios`, the
operating system, the host, the uptime and the memory lines.
