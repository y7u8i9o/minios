# Package installer

This document is the design of `pkg`, an offline installer for applications
distributed as local archives. It fixes the package format, the manifest,
the installation prefix, the records the installer keeps, the dependency
and library compatibility rules, and the changes the rest of the system
needs. Nothing in it is implemented yet.

## Scope

The first version installs, lists, verifies and removes packages from
files on a local filesystem. There is no repository, no download and no
signature, since there is no networking. A package may contain programs,
shared libraries, Lua scripts, data files, manual pages, launcher entries
and MIME registrations. It may not contain scripts that run at
installation: every effect of an installation is declared in the manifest
and performed by the installer, so that removal can undo it exactly.

## Installation prefix

The root filesystem is rebuilt from `build/initrd_root` whenever a program
changes (`storage.md`), so an installation into `/bin` or `/lib` would not
survive the next build. The only persistent volume is the data volume,
mounted at `/home`, and the system has neither symbolic links nor bind
mounts with which another directory could be placed on it. Packages are
therefore installed under

    /home/.local

with the layout of a per-user prefix: `bin/`, `lib/`, `share/` (with
`apps/`, `man/`, `icons/`, `applications/`) and `lib/pkg/` for the
installer's own records. Without a data volume the prefix lies on the root
image's `/home` and disappears with the next build, as every other file of
the home directory does. The prefix is a constant of the installer;
`--prefix DIR` overrides it for tests and for images prepared on another
system.

Three places must learn the prefix:

- `/etc/profile` adds `/home/.local/bin` to `PATH` after `/bin`. The
  window server's launcher and `mime_spawn` start programs by absolute
  path and need no change.
- `/lib/ld.so` searches `/lib` and then `/home/.local/lib` for a library
  named in `DT_NEEDED`. The search list is a table of two directories in
  `user/ld/ld.c`; a name with a slash stays refused, and no environment
  variable changes the list.
- The panel reads `/etc/launcher` and then `/home/.local/share/launcher`,
  and `mime_load` reads the system tables and then
  `/home/.local/share/mime.types` and `mime.apps`; the entries of the
  later files take precedence, as the user's `$HOME/.config` files do for
  other settings. The installer writes those three files from its records
  after every installation and removal; they are never edited by hand.

## Package format

A package is a ustar archive compressed with gzip, the formats that `tar`
and `gzip` on minios and on the host produce. The file name is
`<name>-<version>.mpk`, the MIME type `application/x-minios-package` with
the extension `mpk`. The archive contains, in this order:

1. `manifest`, a regular file, described below. It is the first member so
   that the installer reads it before the rest of the stream.
2. `files/`, a directory tree whose paths are relative to the prefix:
   `files/bin/pong`, `files/share/apps/pong.lua`, `files/lib/libpong.so`.

Members are regular files and directories only. Modes and modification
times are preserved; the owner fields are ignored, since minios has one
user. Names longer than 100 bytes use the ustar prefix field, which both
`tar` implementations write. A member outside `files/` other than
`manifest`, a member whose path contains `..`, or a link member is an
error and the package is rejected before anything is written.

The package name matches `[a-z0-9][a-z0-9+.-]*`. The version is a dotted
sequence of decimal integers, `1.2.0`, compared component by component;
a missing component counts as zero.

## Manifest

The manifest is a text file of `key value` lines, one per line, blank
lines and `#` comments ignored, the format of `/etc/fstab` and
`/etc/mime.apps`. Keys may repeat where the value is a list.

    name pong
    version 1.2.0
    summary Pong for the desktop
    depends lua >= 5.5
    needs libgui.so 1
    needs libc.so 1
    launcher Pong bin/pong
    mime-type text/x-pong pong
    mime-handler text/x-pong bin/pong
    icon share/icons/pong.svg

| key | value | meaning |
|---|---|---|
| `name` | package name | required, once |
| `version` | version | required, once |
| `summary` | one line of text | required, once |
| `depends` | `name` or `name OP version`, OP one of `>=`, `=`, `<` | another package that must be installed first |
| `conflicts` | `name` | a package that may not be installed at the same time |
| `provides` | `soname abi` | a shared library in `lib/` of the package and its ABI number |
| `needs` | `soname abi` | a shared library the package's programs load and the ABI number they were built against |
| `launcher` | `title command` | an entry of the launcher menu; the command is relative to the prefix |
| `mime-type` | `type extensions...` | a line for the MIME type table |
| `mime-handler` | `type command` | a line for the handler table; the command is relative to the prefix |
| `icon` | path relative to the prefix | the icon of the launcher entries |

`needs` lines are not written by hand. `pkg build` derives them from the
`DT_NEEDED` entries of every ELF file in the package and the ABI table of
the system it runs on, so a package records the libraries and ABI numbers
it was actually linked against.

## Records

The installer keeps its state under `/home/.local/lib/pkg/`:

    lib/pkg/lock                 held with flock during every operation
    lib/pkg/<name>/manifest      the manifest as installed
    lib/pkg/<name>/files         one line per installed path: path size crc32
    lib/pkg/<name>/dirs          the directories the package created, deepest last

`files` is the file ownership record. A path belongs to at most one
package; an installation whose archive names a path listed in another
package's record is refused before extraction, naming both packages and
the path. `dirs` lists the directories the installer had to create for
the package, so that removal deletes them again when they are empty, and
leaves `bin/` or `share/apps/` alone when other packages still use them.

The CRC-32 is the one of the gzip format, computed while the member is
written. `pkg verify` recomputes size and CRC of every recorded file and
reports files that changed or disappeared; it is also how a corrupt data
volume is examined.

## Operations

    pkg install FILE...   install packages from archive files
    pkg remove NAME...    remove installed packages
    pkg list              installed packages, one `name version summary` line each
    pkg info NAME|FILE    the manifest and, for an installed package, its files
    pkg verify [NAME...]  check the recorded files
    pkg build DIR [OUT]   make an archive from a directory holding manifest and files/
    pkg check FILE...     the checks of install without writing anything

`install` reads each archive twice. The first pass, through a `gzip -cd`
child connected by a pipe as `tar -z` does, reads the manifest and the
member headers, skipping the data, and runs every check: manifest syntax,
the name and version, member paths, the dependency rule, the conflict
rule, the library rule and the ownership rule. Only when every package on
the command line passes are the archives read a second time and
extracted, in an order that satisfies their dependencies among each
other. A package already installed with the same version is skipped; with
another version it is upgraded, which removes the old files that the new
archive does not contain and replaces the rest. A failure during
extraction removes what the failing package wrote and leaves the packages
installed before it in place. After the last package the installer
rewrites the launcher and MIME tables from the records of all installed
packages.

`remove` refuses to remove a package that another installed package
depends on and names it; `--force` removes it anyway. It deletes the
recorded files, then the recorded directories that are empty, deepest
first, then the record, and rewrites the tables.

Every operation takes the lock first, so two installers cannot interleave.
Messages name the package and the path concerned; the exit status is 0
when every requested operation succeeded and 1 otherwise.

## Dependency rule

A `depends` line is satisfied when a package of that name is installed
and, with an operator, its version compares as required. Packages on the
same `install` command line count as installed for the check, so a
program and its library can be installed together. The dependency graph
of one command line must be acyclic; a cycle is an error.

The base system is not a package. A dependency on the base system is
expressed through `needs`, which names the libraries of `/lib`; a
dependency on a program of `/bin` is not expressed, since `/bin` is the
same on every image of a given build.

## Library compatibility rule

Programs are linked with `-z now` (`dynlink.md`): every symbol is
resolved at load time and a missing library or symbol ends the program
with `ld.so: what: name` before `main`. The installer has to find those
failures before the program is started, and it has to find the failures
the loader cannot see, a changed structure layout or a changed meaning of
a function behind an unchanged symbol name.

Each shared library of the system carries an ABI number, a small integer
in its Makefile (`ABI := 1` in `libgui/Makefile`), incremented whenever a
structure, a constant, a function signature or a documented behaviour
that programs depend on changes incompatibly. Adding functions does not
change it. The build writes the table to `/lib/abi`:

    libc.so 1
    libgui.so 1
    libfont.so 1
    libwire.so 1
    libaudio.so 1
    liblua.so 1

A package's `needs` lines record the number for every library its
programs and libraries load, taken from that table when the package is
built; `provides` records the number of a library the package ships. The
rule has three parts:

1. Every `needs` soname must be present, in `/lib` or provided by an
   installed package or one on the same command line. A library provided
   by a package and one in `/lib` may not share a soname.
2. The recorded ABI number must equal the number the system or the
   providing package has. A different number refuses the installation
   with the message `pong: needs libgui.so ABI 1, system has 2`.
3. Every undefined symbol in the dynamic symbol table of every ELF file
   of the package must be defined by one of the libraries it names,
   searched in the loader's order. The installer reads the `.dynsym`
   tables of the package's files during the first pass and of the
   libraries from disk. This catches a program built against a newer
   minor state of a library with the same ABI number, the case an ABI
   number alone cannot express.

The rule is exact for the loader's view and conservative for the
semantic one: a program that passes it loads, and a program whose
library changed incompatibly is refused as long as the number was
incremented.

## Building packages

A package is built from a directory holding `manifest` and `files/`.
`pkg build` on minios walks the tree, adds the derived `needs` lines to
the manifest it writes into the archive, writes ustar headers and streams
the archive through `gzip -c`. `tools/mkpkg.sh` does the same on the host
with `tar --format=ustar` and `gzip`, deriving `needs` with `readelf -d`
and the `/lib/abi` of `build/initrd_root`, so a package can be produced by
the cross build and copied onto the data volume or a FAT image for
installation.

## Program structure

`user/pkg/` is one program, `/bin/pkg`, linked against libc only:

- `manifest.c`: parsing, validation, version comparison, writing.
- `archive.c`: the gzip child, the ustar reader for both passes, the
  ustar writer for `build`, CRC-32.
- `db.c`: the lock, the records, ownership lookup across packages, the
  rewriting of the launcher and MIME tables.
- `elf.c`: `DT_NEEDED` and `.dynsym` of a file in memory or on disk.
- `pkg.c`: the commands, the check order and the messages.

## Tests

`tests/cases/pkg` runs `/etc/tests/pkg.sh`, with packages built on minios
by `pkg build` from directories under `/etc/tests/pkg/`: install and
list; a launcher and a MIME registration appearing in the prefix tables;
a second package depending on the first, and the refusals of the missing
dependency, the version constraint, the conflict, a foreign ABI number,
an undefined symbol, a path owned by another package and a member outside
`files/`; upgrade; removal with and without a dependent; `verify` after a
file is altered; and a removal that leaves a shared directory in place.
`tests/cases/pkg_host` installs a package built by `tools/mkpkg.sh` from
a FAT image, which checks the archives of the host tools. `tests/cases/ld`
gains a program that loads a library from `/home/.local/lib`.

## Later

- A window for the installer, opened by Files for `.mpk` files, showing
  the manifest and the checks before installation.
- Packages of the programs now built into the root image, so that the
  image shrinks to the base system and the installer manages the rest.
- Signatures, once a key can reach the system by a trusted path.
