# Dynamic linking

Programs are linked against shared libraries since 2026-09-06. The C
library, the toolkit stack, the audio client library and the Lua core are
shared objects in `/lib`, loaded into each process by `/lib/ld.so`, the
dynamic loader of minios. `init` and the loader itself are static.

## Measurements

These measurements describe the initial conversion, before the additional
loader test programs and libraries below were installed.

Before the change the 149 programs of the root image took 62.6 MiB, of
which 13.9 MiB was code and data and the rest DWARF debugging information,
because the unstripped link output was installed. Every program contained
the C library (137 KiB of text), and the 25 graphical programs contained
the toolkit, the font engine and the protocol library (about 230 KiB).
After the change the programs take 2.2 MiB together and the seven shared
objects 870 KiB; `echo` is 3 KiB, the file manager 39 KiB, the shell 76 KiB.

| Library | Programs using it | Shared |
|---|---|---|
| libc | all | yes |
| libgui, libfont, libwire | 25 graphical programs | yes |
| libaudio | 10 | yes |
| liblua (the Lua core) | lua, luac | yes; luac links the core statically, since it calls internal functions that the shared object hides |
| libedit | sh | no, static |

## Build

`toolchain.mk` compiles all user code with `-fPIC`. The archives and the
shared objects are built from the same objects. Each library Makefile links
`$(BUILD)/lib/<name>.so` with `ld -shared -z now --hash-style=sysv
-soname <name>.so` and names the libraries it depends on, so that the
loader finds them through `DT_NEEDED`; the compiler driver of the bare
metal target does not pass `-shared` on, hence the direct use of `ld`.
`libgcc.a` is linked into every shared object. The libc rule fails when
the object carries a TLS segment.

Programs link with `ULDFLAGS`: `-Ttext-segment=0x400000`,
`-dynamic-linker /lib/ld.so`, `-z now`, `--hash-style=sysv` and
`--as-needed`, against `-lgui -laudio -lwire -lfont libedit.a -lc`.
`--as-needed` records only the libraries a program references. Programs
stay `ET_EXEC` at `0x400000`; the linker resolves their references to the
libraries with `R_X86_64_JUMP_SLOT` (functions), `R_X86_64_GLOB_DAT`
(data through the global offset table) and, for data referenced from
assembly, `R_X86_64_COPY`. `init` uses `ULDFLAGS_STATIC`.

`user/Makefile` installs every program and library with `objcopy
--strip-debug`, which removes the DWARF sections and keeps the symbol
table the profiler reads. The unstripped files stay in `build/user/` and
`build/lib/` for gdb.

## Kernel

`sched/elf.c` records the address of the program headers when a `PT_LOAD`
segment covers them and the path of `PT_INTERP` in `struct elf_info`.
`elf_load_interp` loads the loader, an `ET_DYN` file, with every segment
shifted by `USER_INTERP_BASE` (`0x7e0000000000`). `load_image` in
`sched/user.c` reads the loader file after the program and starts the
thread at the loader's entry. `user_stack_setup` appends the auxiliary
vector after the environment: `AT_PHDR`, `AT_PHENT`, `AT_PHNUM`,
`AT_PAGESZ`, `AT_BASE` (the loader's base), `AT_ENTRY` (the program's
entry) and `AT_NULL`. A static program receives the same vector with
`AT_BASE` 0; libc uses it to distinguish static startup from the callback
handoff described below.

## The loader

`user/ld/ld.c` and `start.S` build `/lib/ld.so`, a freestanding position
independent object linked with `-Bsymbolic`, hidden visibility, `-fno-plt`
and its own entry `_dl_start`; it uses no libc and makes its system calls
through an inline `syscall`. `_dl_main` receives the initial stack pointer
and returns the program's entry point; `start.S` enters it with the stack
pointer the kernel provided, the finalizer `_dl_finalize` in `rdx` (the
x86-64 ELF convention) and the initializer `_dl_initialize` in `rcx`.

Startup has three phases. The loader first applies its own relocations,
which are all `R_X86_64_RELATIVE`; the address of its dynamic section is
taken through a hidden symbol, so that no global offset table entry is
read before it is relocated. It then builds the dependency graph: the
program's `PT_DYNAMIC` is found through the headers named by `AT_PHDR`,
and the libraries named by `DT_NEEDED` are loaded breadth first, the
program's own libraries in order, then theirs, which gives the symbol
search order. Object records live in a small mmap arena and form a list
without a fixed cap. The third phase, initialization, runs later from the
C library (below).

Every table an object names is checked before use. A range must lie
inside one `PT_LOAD` segment with the needed permission, not merely inside
the image; strings must be terminated inside `DT_STRTAB`; the SysV hash
table's indices and the GNU hash table's buckets and chains are validated
while the symbol count is derived from them; and the entry sizes of the
symbol and relocation tables must be the ELF64 ones. The loader refuses,
with a diagnostic, objects with a TLS segment, `REL` or `RELR`
relocations, symbol versioning, text relocations, IFUNC symbols, a
preinit array in a library, a library name with a slash, overlapping or
misaligned load segments, and initializers outside executable segments.
An error prints `ld.so: what: name` to standard error and exits with
status 127.

A library is `/lib/<soname>`, or `/home/.local/lib/<soname>` when `/lib`
does not hold it, the directory of installed packages (`packages.md`).
Its span is reserved with `PROT_NONE` at the
next address of an arena that starts at `0x7e0010000000` and ends at
`0x7e8000000000`, each library on a 1 MiB boundary or the alignment its
segments ask for, followed by an unmapped page; the mmap area below
`USER_MMAP_TOP` stays as it was. Each `PT_LOAD` segment is mapped
privately from the file, the tail of the last file page is zeroed, and
pages beyond the file are mapped anonymously, including the first page of
a segment without file bytes.

Symbols are looked up through the GNU hash table when an object has one
and through the SysV table otherwise, honouring hidden and internal
visibility, in the order program then libraries; a local or protected
definition binds inside its own object. The relocation types applied are
`RELATIVE`, `GLOB_DAT`, `JUMP_SLOT`, `64` and `COPY`. Relocations run in
two passes, the ordinary ones of every object and then the `COPY`
relocations of the program, because a copied data object may itself hold
relocated pointers; a `COPY` relocation is refused when its source is
smaller than its destination or its owner is not the program. Every
relocation is applied before the program starts, since the objects are
linked with `-z now`. The loader implements neither lazy binding nor
`dlopen`. After
relocation the `PT_GNU_RELRO` range of every object is made read-only up
to its last page boundary.

## Initialization and finalization

`crt0.S` passes the two callbacks from the loader to `__libc_start`, which
trusts them only when the auxiliary vector carries a non-zero `AT_BASE`,
since `sysret` leaves the entry address in `rcx` for a static program.
After the thread control block, `environ` and the standard streams are
set up, libc registers the finalizer with `atexit` and calls the
initializer. `_dl_initialize` runs the program's preinit array, then the
`DT_INIT` function and the `DT_INIT_ARRAY` entries of every object in
dependency order, a depth first walk over `DT_NEEDED` that visits each
object once; the order in which objects finished initializing is recorded
and `_dl_finalize` runs the `DT_FINI_ARRAY` entries and `DT_FINI` in
reverse, removing each record before calling into it, so that a recursive
`exit` cannot finalize an object twice. `_exit` bypasses finalizers. A
static program runs its own arrays from the linker's start and end
symbols.

## Thread local storage

The loader does not implement TLS: no loaded object may carry a `PT_TLS`
segment. The one thread local variable of the C library, the buffer
`term_sgr` returned, became a field of the thread control block
(`sgr_sequence` in `struct pthread`), and the libc link rule checks the
shared object for a TLS segment.

## Profiler

Addresses inside a shared library were numeric to the profiler, which
resolves user addresses through the symbol table of `/bin/<name>`.
`/dev/maps` (`proc_format_maps` in `sched/proc.c`) lists the file backed
regions of every process, one line with the pid, the start and end
addresses, the file offset of the start and the file's path; for that,
`vfs_open` now keeps the canonical path of regular files in `file.path`
as it did for directories. `prof_symtab_add_maps` in the libc profiler
support reads the lines of one process, loads the symbol table of each
library under `/lib` once, and attaches the regions as modules of the
program's table; `prof_symtab_lookup` resolves an address inside a
module by its file offset, which is the link address in the text
segment of a shared object. `prof` and `sysmon` attach the modules after
loading a program's table.

## Tests

`tests/cases/dynlink` runs `/bin/dyntest`. The program checks that the C
library is mapped above 4 GiB and the program below 16 MiB, that
`environ`, `optind`, `errno` and the standard streams are shared with the
library, that `qsort` calls back into the program, and that `exec` of
another dynamically linked program succeeds. It then runs the fixtures
that `user/ld/tests/fixtures.mk` builds: `ldlifecycle`, linked against a
chain of twenty libraries that alternate between GNU only and dual hash
tables, whose constructors and destructors write one character each, so
that the captured output proves the dependency order of initialization
and the reverse order of finalization, `DT_INIT` and `DT_FINI` beside the
arrays, the preinit array running before the libraries, an absolute
symbol with value zero, a weak undefined symbol, and `_exit` skipping the
destructors; `ldstatic`, the same program linked statically; and the
rejection fixtures. `user/ld/tests/fixtures.py` takes one linked library
and one executable and produces, by changing headers and dynamic tables
of the copies, a set of cases with a manifest under
`/usr/share/ldtests/cases` naming the expected exit status and
diagnostic: a relocation into code, a wrong relocation entry size, an
unsupported relocation type, GNU only and SysV only hash tables, a zero
filled segment starting inside a page, program headers after the first
page, an initializer outside code, a symbol name outside the string
table, a misaligned load segment, a missing library, and a write into a
RELRO page, which must end in `SIGSEGV`. A loader crash on a rejection
case is a test failure, since the expected status is 127 and a signal is
reported separately.

`tests/cases/profile` looks up `qsort` inside the shared C library through
`/dev/maps`. The shell kernel tests poll the free page count after the
shell exits, since the release of the file mappings of a program needs
more than one grace period. The cases of every program, the shell, the
filesystems, the graphical session and the audio stack were run after
the change.
