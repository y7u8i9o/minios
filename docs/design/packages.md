# Package installer

`pkg` (`user/pkg/`, `pkg(1)`) installs applications from local archives
and from signed repositories over HTTP, lists, verifies and removes them,
and builds the archives. This document describes the package format, the
manifest, the installation prefix, the records the installer keeps, the
dependency and library rules, the repository format with its trust
model, and the changes made elsewhere in the system.

## Scope

The installer works on archive files of a local filesystem and on
repositories served over plain HTTP. The network stack has no TLS, so
the transport is not trusted. A repository's index carries the size and
SHA-256 digest of every archive and is signed with Ed25519, and `pkg`
uses nothing from a repository that the signature and the digests do
not cover (see Repositories below).
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
therefore installed under `/home/.local`, with the layout of a per-user
prefix: `bin/`, `lib/`, `share/`, and `lib/pkg/` for the installer's
records. Since U3 of the multiuser plan (`users.md`) the prefix is
`/usr/local` (`LOCAL_PREFIX` in `minios/local.h`), a symbolic link of the
root image to `/home/.local`, which belongs to root and is shared by all
users, and installing or removing a package needs root (`su -c 'pkg
install NAME'`) or a prefix of the user's own such as `--prefix
~/.local`. Without a data volume the prefix lies on the root image's
`/home` and disappears with the next build.
`--prefix DIR` selects another prefix, for tests and for images prepared
on another system.

Three places know the prefix:

- `/etc/profile` appends `/usr/local/bin` and `~/.local/bin` to `PATH`. The launcher menu
  and `mime_spawn` start programs by absolute path.
- `/lib/ld.so` searches `/lib`, then `/usr/local/lib` and then `~/.local/lib` for a library
  named in `DT_NEEDED` (`lib_dirs` in `user/ld/ld.c`). A name with a
  slash stays refused, and no environment variable changes the list.
- The panel reads `/usr/local/share/launcher`, `~/.local/share/launcher`
  and then `/etc/launcher` or the user's `~/.config/launcher`;
  `mime_load` reads the system tables and then
  `/usr/local/share/mime.types` and `mime.apps`. A package handler for
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
| `arch` | `x86_64` or `aarch64` | the machine of the ELF files, once, absent in a package without ELF files |
| `depends` | `name` or `name OP version`, OP one of `>=`, `=`, `<` | a package that must be installed first |
| `conflicts` | `name` | a package that may not be installed at the same time |
| `provides` | `soname abi` | a shared library in `lib/` of the package and its ABI number |
| `needs` | `soname abi` | a shared library the package's files load and the ABI number they were built against |
| `launcher` | `title command` | an entry of the launcher menu; the command is relative to the prefix |
| `mime-type` | `type extensions...` | a line for the MIME type table |
| `mime-handler` | `type command` | a line for the handler table; the command is relative to the prefix |
| `icon` | path relative to the prefix | the icon of the launcher entries |

`pkg build` writes the `arch` line from the machine of the ELF files in the
package and refuses a package whose ELF files are built for two machines
(A9). The installer refuses a package whose `arch` differs from the
machine of the running system, as `uname -m` prints it, and an ELF file
built for another machine, also in a package without an `arch` line.

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

    pkg install FILE|NAME[-VERSION]...  install or upgrade packages
    pkg check FILE|NAME[-VERSION]...    the checks of install without writing anything
    pkg update                          fetch and verify the repository indexes
    pkg search [PATTERN]                list the entries of the indexes
    pkg upgrade [NAME...]               install newer versions from the repositories
    pkg remove NAME...                  remove installed packages (--force: with dependents)
    pkg list                            installed packages, one `name version summary` line each
    pkg info NAME|FILE                  the manifest and the files
    pkg verify [NAME...]                check the recorded files
    pkg build DIR [OUT]                 make an archive from a directory holding manifest and files/

This section describes the operations on archive files; the commands
that use repositories are described under Repositories, and they hand
the archives they fetched to the same installation path.

`install` decompresses each archive into memory (`gzip_decompress` in
libc, `minios/gzip.h`) and reads the manifest and every member header
before it runs the checks: the manifest syntax, the member paths, the
dependency rule, the conflict rule, the library rule and the ownership
rule, for every package on the command line. Only when every package
passes are the members written, in an order that installs dependencies
first. A package already installed with the same version and the same
files is reported and skipped. A package installed with the same version
is replaced when the size or CRC of a file differs from the record or a
file is added or missing, because a rebuild changes the programs without
changing the version. One installed with another version is upgraded, which removes
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
creates archives that can be installed as upgrades. `pkg install` also
replaces a package whose archive has the installed version but other
files, so the archives of a rebuild replace the installed programs.

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

## Repositories

A repository is a directory served over HTTP. It holds the archives, a
file `index` that lists them, and `index.sig`, the Ed25519 signature of
the index. Each architecture has its own repository (A9). `make repo`
writes the repository of the bundled applications to `build/repo/x86_64/`
and `make ARCH=aarch64 repo` to `build/repo/aarch64/`. On the host,
`python3 -m http.server -d build/repo 8000` serves both to a guest under
QEMU user networking. The shipped `/etc/pkg.conf` names
`http://10.0.2.2:8000/$arch`, and `pkg` replaces `$arch` in a repository
URL with the machine name of the system. An index entry whose `arch`
differs from that machine is skipped, so `search`, `install` and
`upgrade` see only the packages that run on the system.

### Index format

The index is a text file whose first line, `minios-pkg-index 1`, names
the format and its version. Each entry that follows begins with a `name`
line and describes one archive:

    minios-pkg-index 1

    name repoprog
    summary A program that needs repolib and repohello
    depends repohello >= 1.0
    version 1.0
    needs libc.so 1
    needs libpkgfix.so 1
    path repoprog-1.0.mpk
    size 2097
    sha256 dcf4d48dd1e2157ead0d7d5970d4080835c985e3213940296749b37323a0c66f

The keys `name`, `version`, `summary`, `arch`, `depends`, `conflicts`,
`provides` and `needs` are copied from the manifest of the archive and
parsed by the manifest parser, so they follow the rules of the manifest.
`path` locates the archive relative to the repository URL and consists
of letters, digits and the characters `._+-/`, without empty, dot or
dot-dot components. `size` is the length of the archive in bytes and
`sha256` the SHA-256 digest of the whole compressed file in lowercase
hexadecimal. Any other key refuses the index. A repository may list
several versions of a package, each name and version once. The parser
accepts an index of at most 4 MiB and at most 512 entries over all
configured repositories.

`index.sig` holds one line, `ed25519 KEYID SIGNATURE`. KEYID is the
first eight bytes of the SHA-256 of the signing public key, and
SIGNATURE is the 64 byte Ed25519 signature (RFC 8032) of the exact
bytes of `index`, both in hexadecimal.

### Trust model

The server, the network and everything between them are untrusted. The
trust anchor is the set of public keys under `/etc/pkg/keys/` on the
image. A key file there is named `*.pub` and holds one line,
`ed25519 HEX`, with the 32 byte public key. The build installs the
public half of its signing key as `/etc/pkg/keys/build.pub`, and further
files add keys.

`pkg` accepts an index only when the key its signature names is one of
those files and the signature verifies against that key. It accepts an
archive from a repository only when its size and SHA-256 digest equal
those of its entry in a verified index and its manifest names the same
package and version as the entry. What the installer does with the
archive afterwards is the local installation with all its checks. The
digest binds each archive to the signed index, so a party that can
change the index or the archives in transit cannot make `pkg` install
anything that the holder of the key did not list. The size in the index
also bounds the download, so a server cannot fill the disk with an
oversized archive. An index is limited to 4 MiB and a signature file to
1 KiB.

The model does not cover freshness. The index carries no date or
sequence number, so a server can keep offering an older index that was
validly signed, and `pkg update` accepts it. The archives it lists are
older, but each of them was signed. Every key in `/etc/pkg/keys/` is
trusted for every repository, and whoever holds a private key can sign
any index.

### Keys and signing

`tools/pkgsign/pkgsign.c` is the host tool, built to
`build/host/pkgsign` from the `libc/src/crypto/` sources that `pkg`
links. `pkgsign keygen FILE` writes a new secret key with mode 0600, the
32 byte seed of RFC 8032 as `ed25519-secret HEX`, and refuses to
overwrite a file. `pkgsign public FILE` prints the public key file of a
secret key, `pkgsign sign KEY FILE` writes `FILE.sig` and checks the new
signature, `pkgsign verify PUB FILE` checks `FILE.sig` against a public
key file, and `pkgsign digest FILE` prints the size and SHA-256 of a
file. `tools/mkrepo.sh PKGSIGN KEY OUTDIR ARCHIVE...` empties OUTDIR,
copies the archives into it, writes the index from the manifest of each
archive with the name line first, and signs it.

The private key stays out of git. The build uses
`build/pkg/signing.key`, which `pkgsign keygen` creates when it does not
exist, or the file that the make variable `PKG_KEY` names. Every build
derives the public half into `build/pkg/signing.pub` and copies it to
`/etc/pkg/keys/build.pub` in the root tree when its content changed.
`make repo` signs `build/repo/x86_64/` with the same key. The aarch64
build uses `build/aarch64/pkg/signing.key` in the same way and signs
`build/repo/aarch64/`.

To rotate the key, replace `build/pkg/signing.key`, or the file that
`PKG_KEY` names, with a new key from `build/host/pkgsign keygen FILE`.
`make repo` then signs the index with the new key and the next `make`
installs its public half as `/etc/pkg/keys/build.pub`, and an image
built before the rotation refuses the new index until that file is
replaced on it as well.

### Configuration

`/etc/pkg.conf` names the repositories with one `repo NAME URL` line
each, in the order in which they are searched. NAME follows the rules
of a package name, and URL has the form `http://HOST[:PORT]/PATH`.
`timeout SECONDS` bounds the connection and every wait for data, 30
seconds by default. `--config FILE` reads another file, and `--root DIR`
makes `pkg` read `DIR/etc/pkg.conf` and `DIR/etc/pkg/keys/` as it reads
`DIR/lib/abi`.

### Commands

`update` fetches `URL/index` and `URL/index.sig` of every repository
into `<prefix>/lib/pkg/_repos/NAME/` as `index.new` and
`index.sig.new`, verifies the signature, parses the index, and only then
renames both into place and records the URL in `url`. A refused index
is deleted and the last verified one stays in place. The underscore
keeps the directory apart from the package records, since a package
name cannot contain one. The repositories are updated one after another
and independently of each other, and the exit status is 1 when any of
them failed.

Every command that reads the indexes verifies the cached copies again.
It uses a repository only when the recorded URL is the configured one,
and for any other it reports that `pkg update` must run.

`search` prints one line per entry with the name, the version, the
repository and the summary, sorted by name and version. A pattern
selects the entries whose name or summary contains it.

`install` takes archive files and package names on one command line.
An argument that names an existing file, contains a slash or ends in
`.mpk` is an archive file, as before. Any other argument is looked up as
a package name in the indexes and, when no entry has that name, as
NAME-VERSION split at the last dash. A name alone selects the highest
version, and between equal versions the repository listed first wins. A
selected version that is installed already is reported and not fetched.

The dependencies are resolved through the indexes as far as the
dependency and library rules allow. For every `depends` line that
neither an installed package nor an archive of the command line
satisfies, the highest version in the indexes that satisfies it is
added. For every `needs` soname that neither `/lib`, an installed
package, an archive of the command line nor another selected package
provides, a package whose `provides` line names the soname with the same
ABI number is added. The resolution continues with what it added. A
dependency that the indexes cannot supply is left to the checks of
install, which report it. The selected archives are downloaded into
`/tmp/pkg-PID/`, checked against the index and handed, together with
the archive files of the command line, to the installation path of the
previous sections. The directory is removed afterwards whether the
installation succeeded or not.

`upgrade` selects, for every installed package or for the named ones,
the highest version in the indexes when it is higher than the installed
version, and installs the selection as `install` does. It prints `the
installed packages are up to date` when nothing is newer.

`check FILE` runs the checks of install as before. When `/etc/pkg.conf`
exists, it then looks the name and version of the archive up in the
verified indexes and compares size and digest. A match prints `NAME
VERSION matches the index of REPO`, a difference is an error, and an
archive that no index lists is reported without an error. This is how
an archive installed from a local file is checked against a signed
index.

### Failure handling

Transfers use the HTTP client of `minios/http.h` in libc, which
`http(1)` shares (`network.md`). A failure names the repository or the
package and ends the command with status 1, and nothing is installed
after a failed transfer or check. The messages are the following.

| Failure | Message |
|---|---|
| connection refused | `pkg: down: connect to 10.0.2.2:1: Connection refused` |
| host name not resolved | `pkg: main: HOST: ` and the resolver's reason |
| no connection within the timeout | `pkg: main: connect to HOST:PORT: no answer in 30 seconds` |
| no data within the timeout | `pkg: main: HOST:PORT sent nothing for 30 seconds` |
| HTTP status other than 200 | `pkg: main: URL/index: the server returned status 404` |
| connection closed early | `pkg: main: HOST:PORT closed the connection after N of M bytes` |
| body longer than `Content-Length` | `pkg: main: HOST:PORT sent more than its Content-Length of N bytes` |
| archive longer than its entry | `pkg: NAME: HOST:PORT announces N bytes, more than the M expected` |
| archive digest differs | `pkg: NAME: the SHA-256 digest of PATH differs from the index of REPO` |
| signature by an unknown key | `pkg: REPO: the index is signed by key KEYID, which is not in /etc/pkg/keys` |
| signature does not verify | `pkg: REPO: the index signature does not verify with /etc/pkg/keys/build.pub` |
| malformed index | `pkg: REPO: entry N: ` and what is wrong |
| no verified index | `pkg: REPO: no index; run pkg update` |

### Cryptography

`libc/src/crypto/sha2.c` implements SHA-256 and SHA-512 from FIPS 180-4
and RFC 6234, and `libc/src/crypto/ed25519.c` implements Ed25519 from
RFC 8032, with the headers `minios/sha2.h` and `minios/ed25519.h`. Field
elements modulo 2^255 - 19 are five limbs of 51 bits with 128 bit
products. Points use the extended coordinates and the formulas of
section 5.1.4, and scalars modulo the group order are reduced bit by
bit. The constants d and sqrt(-1) and the base point are computed from
their definitions rather than written out. Verification refuses a
signature whose S is not below the group order and a public key that
does not decode, and it compares the encoding of [S]B - [k]A with R.
The scalar multiplication runs the same operations for every scalar, so
signing on the host does not branch on the secret key. Both files
depend on `string.h` alone. The host tools compile them with
`-idirafter libc/include`, which finds the `minios/` headers after the
system headers.

## Program structure

`user/pkg/` is one program, `/bin/pkg`, linked against libc only:

- `manifest.c`: parsing, validation, version comparison, writing.
- `archive.c`: the in-memory ustar reader and writer.
- `db.c`: the lock, the records, ownership lookup across packages, the
  rewriting of the launcher and MIME tables, the ABI table.
- `elf.c`: `DT_NEEDED` and the dynamic symbol table of a file in memory.
- `pkg.c`: the commands, the check order and the messages.

`repo.c` reads the configuration, verifies and parses the indexes,
fetches and checks the archives, and implements `update` and `search`.
The resolution of names and dependencies and `upgrade` are in `pkg.c`,
beside the installation path they feed.

The gzip codec moved from `user/coreutils/gzip.c` to `libc/src/gzip.c`
so that the installer and the gzip program share it. For the same
reason the HTTP client of `http(1)` moved to `libc/src/net/http.c`, and
the SHA-2 and Ed25519 code is in `libc/src/crypto/`.

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

`tests/cases/pkg_repo` runs `/etc/tests/pkg-repo.sh` against the HTTP
server of its host peer, `tests/cases/pkg_repo/server.py`, reached as
10.0.2.2 through QEMU user networking. The peer assembles repositories
with `tools/mkrepo.sh` and the build's key from test archives that
`user/pkg/tests/fixtures.mk` writes to `build/user/pkgrepo/`. They are
repohello in versions 1.0 and 1.1, repolib, which provides
`libpkgfix.so`, and repoprog, which needs that library and depends on
repohello. It also
builds four damaged copies, one with an archive changed after signing,
one with an archive longer than its entry, one with the index changed
after signing and one signed by a key the image does not trust. The
script first runs `/bin/cryptotest`, the RFC vectors. It then checks
`update` and the cached files, `search` with and without a pattern, the
installation of repoprog by name with repohello and repolib resolved
through the index and installed in dependency order, the program
running, a repeated installation, an unknown name and an unknown
version, the refusal of an index fetched from another URL, `upgrade` to
repohello 1.1 and its new file, `pkg check` of a local archive that
matches the index and of one that does not, and the removal. The damaged
repositories must be refused by digest, by size, by signature and by key,
with nothing installed and the previous verified index left in place.
Last come a refused connection beside a working second repository, a
missing index, a response shorter than its `Content-Length` for `pkg`
and for `http`, and a server that sends nothing within a two second
timeout.

`make check-pkg` compiles `user/tests/cryptotest.c` with the host
compiler and runs the same vectors on the host: SHA-256 and SHA-512 of
RFC 6234 TEST1 to TEST4, each fed at once and in pieces, and Ed25519 of
RFC 8032 TEST 1, 2, 3 and SHA(abc). For each Ed25519 vector the test
derives the public key, signs, verifies, and refuses a changed message,
a longer message, a changed R, a changed S, S plus the group order and
another key. `make check` includes it.

## Later

- A window for the installer, opened by Files for `.mpk` files, showing
  the manifest and the checks before installation.
- The index should carry a sequence number, so that `pkg update` refuses
  an index older than the one it holds, and a key should be bound to the
  repositories it signs for.
- The profiler reads the symbol tables of `/lib` only; a library
  installed under the prefix appears in a profile without symbols.
