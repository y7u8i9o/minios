# Package installer

`pkg` (`user/pkg/`, `pkg(1)`) installs applications from local archives,
lists, verifies and removes them, and builds the archives. This document
describes the package format, the manifest, the installation prefix, the
records the installer keeps, the dependency and library rules, and the
changes made elsewhere in the system.

## Scope

The installer works on files of a local filesystem. There is no
repository, no download and no signature. The network stack can fetch
files over plain HTTP, but `pkg` does not use it, and without TLS a
download could not be trusted without a digest from another source.
A package may contain programs, shared libraries, scripts, data files,
manual pages, launcher entries and MIME registrations. It may not contain
scripts that run at installation: every effect of an installation is
declared in the manifest and performed by the installer, so that removal
undoes it exactly.

## Installation prefix

The root filesystem is rebuilt from `build/initrd_root` whenever a program
changes (`storage.md`), so an installation into `/bin` or `/lib` would not
survive the next build. The only persistent volume is the data volume,
mounted at `/home`. Symbolic links exist since 2026-09-30 (`vfs.md`),
but a link placed in `/bin` or `/lib` of the root image would disappear
with the next rebuild as well, and there are no bind mounts. Packages are
therefore installed under `/home/.local` (`LOCAL_PREFIX` in
`minios/local.h`), with the layout of a per-user prefix: `bin/`, `lib/`,
`share/`, and `lib/pkg/` for the installer's records. Without a data
volume the prefix lies on the root image's `/home` and disappears with
the next build, as every other file of the home directory does.
`--prefix DIR` selects another prefix, for tests and for images prepared
on another system.

Three places know the prefix:

- `/etc/profile` appends `/home/.local/bin` to `PATH`. The launcher menu
  and `mime_spawn` start programs by absolute path.
- `/lib/ld.so` searches `/lib` and then `/home/.local/lib` for a library
  named in `DT_NEEDED` (`lib_dirs` in `user/ld/ld.c`). A name with a
  slash stays refused, and no environment variable changes the list.
- The panel reads `/etc/launcher` and then `/home/.local/share/launcher`;
  `mime_load` reads the system tables and then
  `/home/.local/share/mime.types` and `mime.apps`. A package handler for
  a type the system table names is ignored, so the user's handler table,
  which the settings program edits, takes precedence, and `mime_save`
  writes the system entries only. The installer rewrites the three files
  from its records after every installation and removal.

## Package format

A package is a ustar archive compressed with gzip, the formats `tar` and
`gzip` produce on minios and on the host. The file name is
`<name>-<version>.mpk`; the MIME type is `application/x-minios-package`
with the extension `mpk`. The archive contains, in this order:

1. `manifest`, a regular file. It is the first member so that the
   installer reads it before the rest of the archive.
2. `files/`, a directory tree whose paths are relative to the prefix:
   `files/bin/pong`, `files/share/apps/pong.lua`, `files/lib/libpong.so`.

Members are regular files and directories. Modes and modification times
are preserved; the owner fields are ignored, since minios has one user.
Names longer than 100 bytes use the ustar prefix field, which both `tar`
implementations write; pax extended headers are skipped. A member outside
`files/` other than `manifest`, a path with an empty or dot component, or
a link member rejects the package before anything is written.

The package name matches `[a-z0-9][a-z0-9+.-]*`. The version is a dotted
sequence of decimal integers, `1.2.0`, compared component by component;
a missing component counts as zero.

## Manifest

The manifest is a text file of `key value` lines, blank lines and `#`
comments ignored, the format of `/etc/fstab` and `/etc/mime.apps`. Keys
repeat where the value is a list.

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
| `depends` | `name` or `name OP version`, OP one of `>=`, `=`, `<` | a package that must be installed first |
| `conflicts` | `name` | a package that may not be installed at the same time |
| `provides` | `soname abi` | a shared library in `lib/` of the package and its ABI number |
| `needs` | `soname abi` | a shared library the package's files load and the ABI number they were built against |
| `launcher` | `title command` | an entry of the launcher menu; the command is relative to the prefix |
| `mime-type` | `type extensions...` | a line for the MIME type table |
| `mime-handler` | `type command` | a line for the handler table; the command is relative to the prefix |
| `icon` | path relative to the prefix | the icon of the launcher entries |

`pkg build` derives the `needs` lines from the `DT_NEEDED` entries of every
ELF file in the package. The ABI number of a soname comes from the
package's own `provides` line, from an installed package that provides
it, or from the system table `/lib/abi`. A `needs` line in the source
manifest supplies the number of a library that is neither on the build
system nor installed, for a package built against a library that is
installed separately; such a line is used only when the other sources
have no number.

## Records

The installer keeps its state under `<prefix>/lib/pkg/`:

    lib/pkg/lock                 created exclusively during every operation
    lib/pkg/<name>/manifest      the manifest as installed
    lib/pkg/<name>/files         one line per installed path: path size crc32
    lib/pkg/<name>/dirs          the directories the package created

The lock file holds the pid of the holder; a lock whose holder no longer
exists is taken over. `files` is the file ownership record. A path
belongs to at most one package; an archive naming a path listed in
another package's record is refused before extraction, naming both
packages and the path. `dirs` lists the directories the installer created
for the package, so that removal deletes them again when they are empty
and leaves `bin/` or `share/` alone while other packages, or the
installer's own tables, use them.

The CRC-32 is the one of the gzip format, computed while the member is
written. `pkg verify` recomputes size and CRC of every recorded file and
reports the files that changed or disappeared.

## Operations

    pkg install FILE...   install or upgrade packages from archive files
    pkg check FILE...     the checks of install without writing anything
    pkg remove NAME...    remove installed packages (--force: with dependents)
    pkg list              installed packages, one `name version summary` line each
    pkg info NAME|FILE    the manifest and the files
    pkg verify [NAME...]  check the recorded files
    pkg build DIR [OUT]   make an archive from a directory holding manifest and files/

`install` decompresses each archive into memory (`gzip_decompress` in
libc, `minios/gzip.h`) and reads the manifest and every member header
before it runs the checks: the manifest syntax, the member paths, the
dependency rule, the conflict rule, the library rule and the ownership
rule, for every package on the command line. Only when every package
passes are the members written, in an order that installs dependencies
first. A package already installed with the same version is reported and
skipped; one installed with another version is upgraded, which removes
the old files that the new archive does not contain and keeps the
directories the old version created. A failure during extraction removes
what the failing package wrote and leaves the packages installed before
it in place. After the last package the installer rewrites the launcher
and MIME tables from the records of all installed packages.

`remove` refuses a package that another installed package depends on or
loads a library from, and names that package; `--force` removes it
anyway. It deletes the recorded files, then the recorded directories that
are empty, deepest first, then the record, and rewrites the tables.

Every operation that writes takes the lock first. Messages name the
package and the path concerned; the exit status is 1 when an operation
failed.

## Dependency rule

A `depends` line is satisfied when a package of that name is installed
or on the same command line and, with an operator, its version compares
as required. The dependency graph of one command line must be acyclic. An
upgrade is refused when an installed package depends on a version the
new one does not satisfy.

The base system is not a package. A dependency on it is expressed through
`needs`, which names the libraries of `/lib`; a dependency on a program
of `/bin` is not expressed, since `/bin` is the same on every image of a
build.

## Library rule

Programs are linked with `-z now` (`dynlink.md`): every symbol is
resolved at load time, and a missing library or symbol ends the program
with `ld.so: what: name` before `main`. The installer finds those
failures before the program is started, and it finds the failures the
loader cannot see, a changed structure layout or a changed meaning of a
function behind an unchanged symbol name.

Each shared library of the system carries an ABI number, `ABI` in its
Makefile (`LUA_ABI` in `user/Makefile` for the Lua core), incremented
whenever a structure, a constant, a function signature or a documented
behaviour that programs depend on changes incompatibly. Adding functions
leaves it alone. The build writes the table to `/lib/abi`:

    libc.so 1
    libfont.so 1
    libwire.so 1
    libaudio.so 1
    libgui.so 1
    liblua.so 1

The rule has three parts:

1. Every `needs` soname must be provided, by `/lib`, by an installed
   package or by one on the same command line. A soname provided by a
   package may not exist in `/lib` and may not be provided by two
   packages.
2. The recorded ABI number must equal the provider's. A different number
   refuses the installation: `pkgprog: needs libpkgfix.so ABI 1, pkgfix
   has 2`.
3. Every undefined symbol in the dynamic symbol table of every ELF file
   of the package (weak references excepted) must be defined by a
   library in the transitive `DT_NEEDED` closure of that file. The
   installer reads the section headers, which the installed files keep,
   of the package's members and of the libraries on disk. This catches a
   program built against a newer state of a library with the same ABI
   number.

When a package on the command line provides a soname that installed
packages load, those packages are checked against the new provider as
well, so a library upgrade that would break an installed program is
refused with the program's name.

## Building packages

A package is built from a directory holding `manifest` and `files/`.
`pkg build` walks the tree in sorted order, derives the `needs` lines,
writes ustar headers and compresses the archive with `gzip_compress`.
`tools/mkpkg.sh DIR OUT ROOT` does the same on the host with `tar
--format ustar` and `gzip`, deriving `needs` with `readelf -d` and the
`lib/abi` of the build tree given as `ROOT`, so a package built by the
cross build can be copied onto the data volume for installation.
`--root DIR` makes `pkg` itself read `lib/abi` and the system libraries
below another tree, which lets a host build of the installer check
packages against a build tree.

## Bundled application packages

`make user` (also `make packages`) builds fourteen optional applications as
packages: `calc`, `code`, `gedit`, `hexview`, `luasynth`, `mandel`, `paint`, `player`,
`playtone`, `pong`, `sequencer`, `synth`, `unicode` and `view`. The archives
are in `build/packages/` on the host and `/usr/share/packages/` in the
image. They are an offline archive shelf, not installed applications.
For example, on minios:

    pkg install /usr/share/packages/calc-*.mpk
    calc
    pkg install /usr/share/packages/*.mpk
    pkg verify
    pkg remove calc

Installation is explicit. An application removed by the user is not
reinstalled on boot. Installed programs and their records survive root
image rebuilds on the data volume. Rebuilding with a newer `VERSION`
creates archives that can be installed as upgrades. Development changes
within the same version require removal and reinstallation, because
`pkg install` skips an already installed version.

The base image keeps init, the shell and console editor, command-line
utilities and language/development tools, shared libraries and fonts,
X12, the panel, desktop, Terminal, Files, settings, the clock, and the
system diagnostics (`sysmon`, `logview`, `evtest`, `x12settings`). These
remain usable before installing any application.

`user/packages/packages.mk` builds application binaries under
`build/user/app-bin/`, independently of the base image's `bin/`. Each
`user/packages/NAME/manifest` declares its summary, launcher and MIME
handlers; the build adds the version from `VERSION` and derives library
requirements. Optional `files/` trees supply data and manuals. Code owns
its Lua source and `code(1)` manual, and Pong also owns its Lua example.
The base Lua interpreter supports both. `man` searches the base manual
tree and `/home/.local/share/man`, including keyword searches.

Application entries are absent from the base launcher and MIME handler
table. Installing packages registers these entries; removing packages
unregisters them. The panel reloads the launcher when opening its menu,
and file opening refreshes the MIME tables. Files remains the base
handler for directories. New home skeletons omit the former Code and
Pong desktop shortcuts; application launchers now come from packages.
Existing home directories are not modified, so users with old shortcuts
can update their `exec=` paths to the installed locations or remove them.

Incremental builds remove former application binaries, data, manual
pages and skeleton shortcuts from the generated root tree and replace
its archive shelf with the current release. This cleanup never edits
`data.img` or an installed package database.

`tests/cases/pkg_apps` checks the real archive collection: absence of
built-in copies, installation, all file records, launcher and MIME
registration, manual lookup and search, calculator execution, removal,
reinstallation, and fallback handlers. Application GUI tests install
the relevant archive before opening its program from `/home/.local/bin`.

## Program structure

`user/pkg/` is one program, `/bin/pkg`, linked against libc only:

- `manifest.c`: parsing, validation, version comparison, writing.
- `archive.c`: the in-memory ustar reader and writer.
- `db.c`: the lock, the records, ownership lookup across packages, the
  rewriting of the launcher and MIME tables, the ABI table.
- `elf.c`: `DT_NEEDED` and the dynamic symbol table of a file in memory.
- `pkg.c`: the commands, the check order and the messages.

The gzip codec moved from `user/coreutils/gzip.c` to `libc/src/gzip.c`
so that the installer and the gzip program share it.

## Tests

`tests/cases/pkg` runs `/etc/tests/pkg.sh` with fixtures the build places
under `/etc/tests/pkgfix` (`user/pkg/tests/fixtures.mk`): a library in two
builds, one without the symbol the program calls, and the program that
loads it. The script builds packages with `pkg build` and checks: the
derived `needs` lines; the refusal of a missing dependency; installation
of two packages in dependency order regardless of the command line
order; the program running with its library from `/home/.local/lib`; the
launcher and MIME tables; the records; a repeated installation; `verify`;
the refusals of a conflict, of a library upgrade with another ABI number,
of a library build without the symbol, of a wrong system ABI number, of a
path owned by another package and of a member outside `files/`; an
upgrade that removes a file; `verify` after a change and after a
deletion; removal with and without a dependent; the removal of an
emptied directory; and the installation and removal of
`/etc/tests/pkghello-1.0.mpk`, which `tools/mkpkg.sh` builds on the host
from the hello program, so the archives of the host tools are covered as
well. The variables at the top of the script let the same script drive a
host build of the installer.

## Later

- A window for the installer, opened by Files for `.mpk` files, showing
  the manifest and the checks before installation.
- Signatures, once a key can reach the system by a trusted path.
- The profiler reads the symbol tables of `/lib` only; a library
  installed under the prefix appears in a profile without symbols.
